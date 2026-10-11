#ifndef MAROOF_NET_SERVER_HH
#define MAROOF_NET_SERVER_HH

#include <maroof/net/Connection.hh>
#include <maroof/net/Endpoint.hh>
#include <maroof/net/PacketHeader.hh>
#include <maroof/net/UdpSocket.hh>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <unordered_map>

namespace maroof::net
{

    // Higher-level server wrapper. Owns a UdpSocket, a connection table, and the dispatch loop
    // that every demo was reimplementing by hand. Callers register callbacks and either call
    // tick() per frame or run() to block.
    //
    // Thread model: single-threaded. For multi-threaded fanout use SO_REUSEPORT and run one
    // Server per worker thread.
    class Server
    {
    public:

        using ReceiveCallback    = std::function<void(Connection&, std::span<const std::byte> payload, std::uint8_t channel)>;
        using ConnectCallback    = std::function<void(Connection&)>;
        using DisconnectCallback = std::function<void(const Endpoint&)>;

        // Binds the socket and sets it non-blocking. Throws std::system_error on bind failure.
        explicit Server(const Endpoint& bind_addr);

        // Callback registrars; chainable.
        Server& on_receive   (ReceiveCallback    cb) { m_on_receive    = std::move(cb); return *this; }
        Server& on_connect   (ConnectCallback    cb) { m_on_connect    = std::move(cb); return *this; }
        Server& on_disconnect(DisconnectCallback cb) { m_on_disconnect = std::move(cb); return *this; }

        // One iteration of the IO loop: drain incoming datagrams, fire callbacks, advance
        // retransmit timers on every tracked connection. Returns the number of packets processed.
        std::size_t tick();

        // Blocking loop. Exits when `stop()` is called from a callback.
        void run();
        void stop() noexcept { m_running = false; }

        UdpSocket&       socket()       noexcept { return m_socket; }
        const UdpSocket& socket() const noexcept { return m_socket; }

        std::size_t connection_count() const noexcept { return m_connections.size(); }

    private:

        void handle_packet(const Endpoint& peer, const PacketHeader& header, std::span<const std::byte> payload);
        void drive_tick();

        UdpSocket m_socket;
        std::unordered_map<Endpoint, Connection> m_connections;

        ReceiveCallback    m_on_receive    {};
        ConnectCallback    m_on_connect    {};
        DisconnectCallback m_on_disconnect {};

        bool m_running { false };

        // Reused buffers so the main loop never allocates.
        std::array<std::byte, 2048> m_recv_buf {};
        std::array<std::byte, 2048> m_send_buf {};

    };

} // namespace maroof::net

#endif // MAROOF_NET_SERVER_HH
