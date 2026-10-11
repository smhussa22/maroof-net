#ifndef MAROOF_NET_UDP_SOCKET_HH
#define MAROOF_NET_UDP_SOCKET_HH

#include <maroof/net/Endpoint.hh>
#include <maroof/net/Error.hh>

#include <cstddef>
#include <expected>
#include <span>
#include <utility>

namespace maroof::net
{

   class UdpSocket
   {
   public:

      // Claims a UDP socket from the OS; throws on failure.
      UdpSocket();
      
      // Releases the socket back to the OS; no-op if none is held. 
      ~UdpSocket();

      // Copying a file descriptor doesnt make any sense; only one wrapper around each socket
      UdpSocket(const UdpSocket&) = delete;
      UdpSocket& operator=(const UdpSocket&) = delete;

      // Transfers ownership of the fd; the moved-from socket is left with no fd held (-1).
      UdpSocket(UdpSocket&&) noexcept;
      UdpSocket& operator=(UdpSocket&&) noexcept;

      // Binds the socket to a local address so incoming packets to that port arrive here.
      // Server-side only; clients skip this and let the OS pick an ephemeral source port on first send.
      [[nodiscard]] std::expected<void, ErrorCode> bind(const Endpoint& local);

      // Sends the given bytes to the peer. Returns number of bytes actually sent on success.
      [[nodiscard]] std::expected<size_t, ErrorCode> send_to(std::span<const std::byte> data, const Endpoint& peer);

      // Reads one datagram into the buffer and reports who sent it.
      // Returns {bytes_written, sender_endpoint} on success.
      [[nodiscard]] std::expected<std::pair<std::size_t, Endpoint>, ErrorCode> recv_from(std::span<std::byte> buffer);

      // --- Batched send/recv (recvmmsg / sendmmsg, Linux) ---
      //
      // One syscall carries up to BATCH_LIMIT (64) datagrams. For larger slot counts the caller
      // can loop — short counts are the kernel's convention, not an error. These exist to
      // amortize syscall overhead at high packet rates (10K+ msgs/sec) where ~100ns per syscall
      // becomes a sizable fraction of per-packet work.

      static constexpr std::size_t BATCH_LIMIT { 64 };

      struct RecvSlot
      {
          std::span<std::byte> buf;      // caller-provided receive buffer for this slot
          std::size_t          received; // written on return: bytes received into buf
          Endpoint             peer;     // written on return: sender address
      };

      struct SendSlot
      {
          std::span<const std::byte> bytes;
          Endpoint                   peer;
      };

      // Returns the number of datagrams actually received (0..min(slots.size(), BATCH_LIMIT)).
      [[nodiscard]] std::expected<std::size_t, ErrorCode> recv_many(std::span<RecvSlot> slots) noexcept;

      // Returns the number of datagrams actually sent.
      [[nodiscard]] std::expected<std::size_t, ErrorCode> send_many(std::span<const SendSlot> slots) noexcept;

      // Puts the socket in non-blocking mode. After this, an empty-queue recv_from
      // returns WouldBlock instead of hanging the calling thread.
      [[nodiscard]] std::expected<void, ErrorCode> set_nonblocking();

      // --- Multicast ------------------------------------------------------------
      //
      // Subscribers: bind() to the group port (or any port), then join_multicast_group(group)
      // to receive packets published to that group. Publishers: just send_to(group, ...).
      //
      // group.address() must be in the 224.0.0.0 / 4 range. Local-interface selection uses the
      // kernel's default route; override by setting `iface` to a specific host IP.

      [[nodiscard]] std::expected<void, ErrorCode>
      join_multicast_group(const Endpoint& group, const char* iface_ip = nullptr);

      [[nodiscard]] std::expected<void, ErrorCode>
      leave_multicast_group(const Endpoint& group, const char* iface_ip = nullptr);

      // Source-Specific Multicast: subscribe to a group but only accept packets whose source IP
      // matches `source`. Used by market feeds for A/B arbitration between primary + backup pubs.
      [[nodiscard]] std::expected<void, ErrorCode>
      join_multicast_source(const Endpoint& group, const Endpoint& source,
                            const char* iface_ip = nullptr);

      // Enable SO_REUSEPORT so multiple sockets can bind to the same (addr, port). On Linux the
      // kernel load-balances incoming datagrams across them by 4-tuple hash — this is the standard
      // multi-threaded UDP scaling pattern. Must be called before bind().
      [[nodiscard]] std::expected<void, ErrorCode> set_reuse_port();

      // Set the kernel socket receive buffer. The default (~212 KB) is tiny; 8-16 MB prevents
      // drops under bursty load. Linux clamps to 2× your value and caps at sysctl net.core.rmem_max.
      [[nodiscard]] std::expected<void, ErrorCode> set_recv_buffer_size(int bytes);

      // Raw fd — needed to register the socket with EpollReactor (or io_uring, etc).
      int fd() const noexcept { return m_socket_fd; }

   private:

      // The file descriptor number for the raw POSIX socket that this class manages.
      // It init to -1 because 0 resolves to stdin; -1 is an invalid fd; no socket held.
      int m_socket_fd { -1 };
      
   }; 

} // namespace maroof::net

#endif // MAROOF_NET_UDP_SOCKET_HH