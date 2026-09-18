#ifndef MAROOF_NET_CONNECTION_HH
#define MAROOF_NET_CONNECTION_HH

#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/ReliableOrderedChannel.hh>

#include <chrono>
#include <cstdint>
#include <span>
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

        // Unreliable send stub. Moves to UnreliableUnorderedChannel in Phase 2.
        PacketHeader make_data_header(std::uint8_t channel_id);

        void note_received(std::uint64_t sequence);

        std::span<const std::byte> make_reliable_data(std::span<const std::byte> payload, std::uint8_t channel_id);

        std::vector<std::span<const std::byte>> tick(std::chrono::steady_clock::time_point now);

        void handle_ack(std::uint64_t sequence);

        PacketHeader make_ack(std::uint64_t received_sequence);

        template <typename OnProcessPayload>
        bool receive_reliable_data(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver);

    private:

        Endpoint m_peer;
        State m_state { State::Disconnected };
        std::uint64_t m_next_send_seq { 0 };         // handshake + make_data_header only; channels own their own seqs
        std::uint64_t m_highest_received_seq { 0 };

        ReliableOrderedChannel m_channel_0 {};

    };

    template <typename OnProcessPayload>
    bool Connection::receive_reliable_data(std::uint64_t sequence, std::span<const std::byte> payload, OnProcessPayload&& deliver)
    {
        return m_channel_0.receive(sequence, payload, std::forward<OnProcessPayload>(deliver));
    }

} // namespace maroof::net

#endif // MAROOF_NET_CONNECTION_HH
