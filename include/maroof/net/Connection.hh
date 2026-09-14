#ifndef MAROOF_NET_CONNECTION_HH
#define MAROOF_NET_CONNECTION_HH

#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>

#include <cstdint>

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
        // was probably dropped).
        PacketHeader accept_hello();

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

    private:

        Endpoint m_peer;
        State m_state { State::Disconnected };
        std::uint64_t m_next_send_seq { 0 };
        std::uint64_t m_highest_received_seq { 0 };

    };

} // namespace maroof::net

#endif // MAROOF_NET_CONNECTION_HH
