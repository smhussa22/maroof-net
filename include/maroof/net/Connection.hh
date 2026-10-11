#ifndef MAROOF_NET_CONNECTION_HH
#define MAROOF_NET_CONNECTION_HH

#include <maroof/net/ConnectionStats.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/ReliableOrderedChannel.hh>
#include <maroof/net/UnreliableUnorderedChannel.hh>
#include <maroof/net/UnreliableSequencedChannel.hh>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace maroof::net
{

    // Peer relationship on top of stateless UDP: handshake state machine + channel dispatch.
    // Pure state; does not touch a socket.
    class Connection
    {
    public:

        enum class State : std::uint8_t
        {
            Disconnected,
            Connecting,
            Established,
        };

        explicit Connection(const Endpoint& peer);

        const Endpoint& peer()  const noexcept { return m_peer;  }
        State state() const noexcept { return m_state; }

        // Client: Disconnected -> Connecting.
        PacketHeader make_hello();

        // Server: Disconnected -> Established (idempotent on duplicate Hello).
        // hello_sequence seeds the receive window so the server knows what Data seq to expect;
        // ignored on a duplicate Hello to avoid rewinding already-delivered progress.
        PacketHeader accept_hello(std::uint64_t hello_sequence);

        // Client: Connecting -> Established. No-op if already Established or Disconnected.
        void on_hello_ack();

        void note_received(std::uint64_t sequence);

        // Advance reliable channel retransmit timers. Transitions to Disconnected if the channel
        // dies OR if no packet has arrived from the peer within HEARTBEAT_DEATH.
        std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now);

        void handle_ack(std::uint64_t sequence);
        PacketHeader make_ack(std::uint64_t received_sequence);

        // Build a Ping and remember the send time so on_pong can compute RTT.
        // Ping/Pong use their own sequence counter, independent of the channel counters.
        PacketHeader make_ping(std::chrono::steady_clock::time_point now);

        // Build the Pong reply for an incoming Ping — echoes the ping's sequence.
        PacketHeader on_ping(const PacketHeader& ping) const noexcept;

        // Match the pong to a remembered Ping and fold the RTT sample into the EWMA.
        // No-op if the pong's sequence isn't in the pending-pings map (stale, duplicate, or spoofed).
        void on_pong(const PacketHeader& pong, std::chrono::steady_clock::time_point now);

        // Smoothed round-trip time. std::nullopt until the first Pong lands.
        std::optional<std::chrono::nanoseconds> rtt() const noexcept { return m_smoothed_rtt; }

        // Per-channel send. Takes a readable byte payload, returns the fully serialized wire bytes
        // (header + payload) in channel-owned storage. Hand the spans to socket.send_to immediately —
        // they are invalidated by the next call to send on the same channel.
        //
        // send_reliable may fragment: callers must send every span in the returned vector, in order,
        // for payloads that exceed one path MTU. Unreliable/sequenced always return a single span.
        std::vector<std::span<const std::byte>> send_reliable  (std::span<const std::byte> payload);  // channel 0
        std::span<const std::byte>              send_unreliable(std::span<const std::byte> payload);  // channel 1
        std::span<const std::byte>              send_sequenced (std::span<const std::byte> payload);  // channel 2

        // Dispatch incoming Data to the channel indicated by header.channel_id.
        // Returns true if the caller should ACK (only ever true for the reliable channel);
        // returns false for unreliable/sequenced (never ACK), unknown channel_id (dropped), or
        // reliable beyond-window / oversized (sender must keep retrying).
        template<typename OnProcessPayload>
        bool receive_data(const PacketHeader& header, std::span<const std::byte> payload, OnProcessPayload&& deliver);

        void note_activity(std::chrono::steady_clock::time_point now) { m_last_heard_from = now; }
        std::chrono::steady_clock::time_point last_heard_from() const noexcept { return m_last_heard_from; }

        // Observability counters — safe to read any time; written by the channel on protocol events.
        const ConnectionStats& stats() const noexcept { return m_stats; }
        ConnectionStats&       stats_mut() noexcept   { return m_stats; }

        // Piggyback plumbing. The dispatch loop sets this to a sequence it needs to ACK; the next
        // outgoing send on ANY channel attaches it to its header and clears the slot. If no send
        // happens, the dispatch loop flushes a stand-alone ACK instead.
        void                            set_pending_piggyback_ack(std::uint64_t seq)       noexcept { m_pending_piggyback_ack = seq; }
        void                            clear_pending_piggyback_ack()                      noexcept { m_pending_piggyback_ack.reset(); }
        std::optional<std::uint64_t>    pending_piggyback_ack() const                      noexcept { return m_pending_piggyback_ack; }
        std::optional<std::uint64_t>    take_pending_piggyback_ack()                       noexcept { auto v = m_pending_piggyback_ack; m_pending_piggyback_ack.reset(); return v; }

    private:

        // Peer is considered dead if no packet arrives within this window. Any inbound packet
        // (Data/Ack/Pong/…) resets the timer via note_activity.
        static constexpr std::chrono::seconds HEARTBEAT_DEATH { 5 };

        Endpoint m_peer;
        State m_state { State::Disconnected };
        std::uint64_t m_next_send_seq { 0 };         // handshake + make_data_header only; channels own their own seqs
        std::uint64_t m_highest_received_seq { 0 };

        ReliableOrderedChannel m_channel_0 {};
        UnreliableUnorderedChannel m_channel_1 {};
        UnreliableSequencedChannel m_channel_2 {};
        std::chrono::steady_clock::time_point m_last_heard_from {};
        ConnectionStats m_stats {};
        std::optional<std::uint64_t> m_pending_piggyback_ack {};

        // Ping/Pong state — sequence counter and send-time map for RTT sampling.
        std::uint64_t m_next_ping_seq { 0 };
        std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> m_ping_send_times {};
        std::optional<std::chrono::nanoseconds> m_smoothed_rtt {};

    };

    template<typename OnProcessPayload>
    bool Connection::receive_data(const PacketHeader& header, std::span<const std::byte> payload, OnProcessPayload&& deliver)
    {

        switch (header.channel_id)
        {

            case ReliableOrderedChannel::CHANNEL_ID:
                return m_channel_0.receive(header, payload, std::forward<OnProcessPayload>(deliver));

            case UnreliableUnorderedChannel::CHANNEL_ID:
                m_channel_1.receive(header.sequence, payload, std::forward<OnProcessPayload>(deliver));
                return false;

            case UnreliableSequencedChannel::CHANNEL_ID:
                m_channel_2.receive(header.sequence, payload, std::forward<OnProcessPayload>(deliver));
                return false;

            default:
                return false;

        }

    }

} // namespace maroof::net

#endif // MAROOF_NET_CONNECTION_HH
