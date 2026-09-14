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

      // Puts the socket in non-blocking mode. After this, an empty-queue recv_from
      // returns WouldBlock instead of hanging the calling thread.
      [[nodiscard]] std::expected<void, ErrorCode> set_nonblocking();

   private:

      // The file descriptor number for the raw POSIX socket that this class manages.
      // It init to -1 because 0 resolves to stdin; -1 is an invalid fd; no socket held.
      int m_socket_fd { -1 };
      
   }; 

} // namespace maroof::net

#endif // MAROOF_NET_UDP_SOCKET_HH