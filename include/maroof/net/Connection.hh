#ifndef MAROOF_NET_CONNECTION_HH
#define MAROOF_NET_CONNECTION_HH

#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/PendingPacket.hh>

#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace maroof::net
{

    // Represents one peer relationship on top of stateless UDP.
    // Connection is the user-space bookkeeping that gives us the state we need
    // to run a real protocol on top of UDP. It's a pure state machine and does
    // not touch a socket. Callers drive send/recv; Connection just reports what
    // state it's in and produces the packet headers that should be sent next.
    class Connection
    {
    public:

        // Three-state handshake state machine for now:
        // Disconnected -> Connecting  via make_hello    (client)
        // Connecting   -> Established via on_hello_ack  (client)
        // Disconnected -> Established via accept_hello  (server)
        enum class State : std::uint8_t
        {

            Disconnected,
            Connecting,
            Established,

        };

        explicit Connection(const Endpoint& peer);

        const Endpoint& peer()  const noexcept { return m_peer;  }
        State state() const noexcept { return m_state; }

        // Client-side. Transitions Disconnected to Connecting and returns the
        // Hello header the caller should serialize and send. Calling this while
        // already Connecting produces another Hello with a fresh sequence number,
        PacketHeader make_hello();

        // Server-side. Transitions Disconnected to Established and returns the
        // HelloAck header to send back. Idempotent: calling this on an already
        // Established connection just produces another HelloAck, which is the
        // right behavior when a duplicate Hello arrives (our previous HelloAck
        // was probably dropped). Also seeds the reliable receive window's
        // expected sequence from hello_sequence + 1, but only the first time -
        // a duplicate Hello arriving after we're already Established must not
        // reseed and undo delivery progress.
        PacketHeader accept_hello(std::uint64_t hello_sequence);

        // Client-side. Transitions Connecting to Established. No-op if we're
        // already Established. Ignored if we're Disconnected (stray packet).
        void on_hello_ack();

        // Build a Data header with the next sequence number. Sequence counter
        // is per-connection for now. Phase 2 will make it per-channel so that
        // a drop on one channel doesn't affect another.
        PacketHeader make_data_header(std::uint8_t channel_id);

        // Record that we saw a packet from this peer. For now just tracks the
        // highest sequence number seen. Phase 1 grows this into proper ACK
        // bookkeeping and duplicate detection.
        void note_received(std::uint64_t sequence);

        // Serialize a reliable Data packet with the given payload and register
        // it for ACK tracking. Returns a span into the internally stored bytes
        // that the caller should hand to socket.send_to. The span points into
        // storage owned by this Connection; it stays valid until the packet is
        // acked, the retransmit budget runs out, or another call to
        // make_reliable_data / tick / handle_ack occurs (any of those may
        // rehash or erase the entry). In practice: send the returned bytes
        // immediately, then move on.
        std::span<const std::byte> make_reliable_data(std::span<const std::byte> payload, std::uint8_t channel_id);

        // Advance the retransmit timers. Call this on every main-loop iteration
        // (or at least often enough that the shortest retransmit window won't
        // be missed by much). For each unacked packet whose retransmit time
        // has passed, this bumps its retry_count, pushes its next-fire out with
        // exponential backoff, and adds a span of its bytes to the returned
        // vector for the caller to send. If any packet has exceeded MAX_RETRIES,
        // the Connection transitions to Disconnected and the caller should
        // treat the peer as dead.
        std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now);

        // Peer acknowledged this sequence number. Remove the corresponding
        // entry from the pending map. No-op if we don't have that sequence
        // pending (duplicate ACK, or ACK for something we've already
        // considered acked).
        void handle_ack(std::uint64_t sequence);

        // Build an Ack packet header acknowledging received_sequence. The Ack's
        // own sequence field carries the acked sequence directly (we don't
        // consume a fresh sequence number since Ack packets are stateless -
        // they never get retransmit-tracked themselves).
        PacketHeader make_ack(std::uint64_t received_sequence);

        // Ingests a reliable Data packet with dedup and in-order delivery.
        // Invokes deliver(span) for each in-order payload (this one plus any
        // now-consecutive stashed ones). No-op on duplicate, out-of-order stash,
        // beyond-window, or oversized payload. Spans passed to deliver are
        // valid only for the duration of that call. Returns true if sequence
        // was accounted for (delivered, stashed, or a duplicate of either) -
        // the caller should Ack only in that case. Returns false for
        // beyond-window or oversized payloads, which were dropped without
        // being tracked; the caller must NOT Ack those, so the sender keeps
        // retransmitting until the window can actually take the packet.
        template <typename OnProcessPayload>
        bool receive_reliable_data(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

    private:

        // Retransmit tuning. Initial timeout doubles per retry, capped at max.
        // After MAX_RETRIES resends on the same packet, we give up on the peer.
        static constexpr std::uint32_t MAX_RETRIES { 8 };
        static constexpr std::chrono::milliseconds INITIAL_TIMEOUT { 100 };
        static constexpr std::chrono::milliseconds MAX_TIMEOUT { 1600 };

        // Receive-side reliable sliding window. Bounded fixed-size storage so
        // there are no per-packet heap allocations on the receive hot path.
        static constexpr std::size_t RELIABLE_WINDOW_SIZE = 128;
        static constexpr std::size_t MAX_RELIABLE_PAYLOAD = 1500;  // one UDP MTU

        struct ReorderSlot
        {
            std::uint16_t length { 0 };
            std::array<std::byte, MAX_RELIABLE_PAYLOAD> bytes {};
        };

        Endpoint m_peer;
        State m_state { State::Disconnected };
        std::uint64_t m_next_send_seq { 0 };
        std::uint64_t m_highest_received_seq { 0 };
        std::unordered_map<std::uint64_t, PendingPacket> m_sent_reliable {}; // in flight reliable packets awaiting ACK; seq number -> pendingpacket struct

        // Sliding window state for reliable receive.
        std::optional<std::uint64_t> m_next_expected_reliable_seq {};
        std::size_t m_window_base { 0 };
        std::bitset<RELIABLE_WINDOW_SIZE> m_reliable_received {};
        std::array<ReorderSlot, RELIABLE_WINDOW_SIZE> m_reliable_slots {};

        // will need this if and probably when i add batching multiple commands
        // std::deque<PendingPacket> m_outgoing_reliable {};                    // queued but not yet in flight reliable packets

    };

    template <typename OnProcessPayload>
    bool Connection::receive_reliable_data(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
    {

        if (!m_next_expected_reliable_seq.has_value())
        {
            m_next_expected_reliable_seq = sequence;
        }

        const std::uint64_t next = *m_next_expected_reliable_seq;

        // Below window: already delivered. Still Ack it - the sender is only
        // retransmitting because our earlier Ack was lost.
        if (sequence < next)
        {
            return true;
        }

        // Beyond window: no slot to hold it. Do NOT Ack - the sender must
        // keep retrying until the window has slid far enough to accept it.
        if (sequence >= next + RELIABLE_WINDOW_SIZE)
        {
            return false;
        }

        // Payload wouldn't fit a slot. Same as beyond-window: can't track it.
        if (payload.size() > MAX_RELIABLE_PAYLOAD)
        {
            return false;
        }

        const std::size_t slot_idx = (m_window_base + (sequence - next)) % RELIABLE_WINDOW_SIZE;

        // Duplicate: bit already set. Already accounted for, Ack again.
        if (m_reliable_received.test(slot_idx))
        {
            return true;
        }

        // Stash into the slot.
        auto& slot = m_reliable_slots[slot_idx];
        slot.length = static_cast<std::uint16_t>(payload.size());
        std::memcpy(slot.bytes.data(), payload.data(), payload.size());
        m_reliable_received.set(slot_idx);

        // If not at the front of the window, we're done - just stashed.
        if (sequence != next)
        {
            return true;
        }

        // At the front. Drain consecutive slots and slide the window.
        while (m_reliable_received.test(m_window_base))
        {
            auto& front = m_reliable_slots[m_window_base];
            deliver(std::span<const std::byte>(front.bytes.data(), front.length));
            m_reliable_received.reset(m_window_base);
            m_next_expected_reliable_seq = *m_next_expected_reliable_seq + 1;
            m_window_base = (m_window_base + 1) % RELIABLE_WINDOW_SIZE;
        }

        return true;

    }

} // namespace maroof::net

#endif // MAROOF_NET_CONNECTION_HH
