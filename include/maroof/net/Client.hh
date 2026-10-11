#ifndef MAROOF_NET_CLIENT_HH
#define MAROOF_NET_CLIENT_HH

#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace maroof::net
{

    // Client-side counterpart to Server. Owns a UdpSocket + one Connection to a specific peer.
    // Drives handshake retries and the IO loop; callers register callbacks for connect/receive/
    // disconnect events and either call tick() each frame or run() to block.
    class Client
    {
    public:

        using ReceiveCallback    = std::function<void(std::span<const std::byte> payload, std::uint8_t channel)>;
        using ConnectCallback    = std::function<void()>;
        using DisconnectCallback = std::function<void()>;

        explicit Client(const Endpoint& server);

        Client& on_receive   (ReceiveCallback    cb) { m_on_receive    = std::move(cb); return *this; }
        Client& on_connect   (ConnectCallback    cb) { m_on_connect    = std::move(cb); return *this; }
        Client& on_disconnect(DisconnectCallback cb) { m_on_disconnect = std::move(cb); return *this; }

        // Initiates the handshake. Returns false if already connecting or established.
        bool connect();

        // Enqueue to send. For reliable the fragments are tracked for retransmit internally.
        void send_reliable  (std::span<const std::byte> payload);
        void send_unreliable(std::span<const std::byte> payload);
        void send_sequenced (std::span<const std::byte> payload);

        // Send a Ping now; the matching Pong (if it comes back) updates the RTT estimate.
        void send_ping();

        // One iteration: drain socket, dispatch, retry handshake if needed, flush retransmits.
        std::size_t tick();

        // Blocking loop until stop() is called.
        void run();
        void stop() noexcept { m_running = false; }

        Connection&       connection()       noexcept { return m_conn; }
        const Connection& connection() const noexcept { return m_conn; }

        UdpSocket&       socket()       noexcept { return m_socket; }
        const UdpSocket& socket() const noexcept { return m_socket; }

    private:

        void handle_packet(const PacketHeader& header, std::span<const std::byte> payload);
        void retry_hello_if_needed(std::chrono::steady_clock::time_point now);

        UdpSocket  m_socket;
        Connection m_conn;

        ReceiveCallback    m_on_receive    {};
        ConnectCallback    m_on_connect    {};
        DisconnectCallback m_on_disconnect {};

        bool m_running { false };
        bool m_fired_connect { false };  // on_connect fires exactly once per lifetime

        // Handshake retry state.
        static constexpr std::chrono::milliseconds HELLO_RETRY_INTERVAL { 200 };
        static constexpr int                       MAX_HELLO_ATTEMPTS   { 5 };
        int                                        m_hello_attempts     { 0 };
        std::chrono::steady_clock::time_point      m_last_hello_at      {};

        std::array<std::byte, 2048> m_recv_buf {};
        std::array<std::byte, 2048> m_send_buf {};

    };

} // namespace maroof::net

#endif // MAROOF_NET_CLIENT_HH
