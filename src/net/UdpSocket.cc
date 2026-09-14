#include <maroof/net/UdpSocket.hh>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <system_error>

namespace maroof::net
{

    UdpSocket::UdpSocket() : m_socket_fd { ::socket(AF_INET, SOCK_DGRAM, 0) }
    {

        if (m_socket_fd == -1)
        {

            throw std::system_error(errno, std::generic_category(), "socket");

        }
        
    }

    UdpSocket::~UdpSocket()
    {

        if (m_socket_fd != -1)
        {

            ::close(m_socket_fd);

        }

    }

    UdpSocket::UdpSocket(UdpSocket&& other) noexcept : m_socket_fd { other.m_socket_fd }
    {

        other.m_socket_fd = -1;

    }

    UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
    {

        if (this == &other)
        {

            return *this;

        }

        if (m_socket_fd != -1)
        {
        
            ::close(m_socket_fd);
        
        }
        
        m_socket_fd = other.m_socket_fd;
        other.m_socket_fd = -1;

        return *this;
        
    }

    std::expected<void, ErrorCode> UdpSocket::bind(const Endpoint& local)
    {
        
        // call ::bind, translate errno on failure
        sockaddr_in addr { local.to_sockaddr_in() };
        if(::bind(m_socket_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == -1)
        {

            return std::unexpected(error_from_errno(errno));

        }

        return {};

    }

    // Sends the given bytes to the peer. Returns number of bytes actually sent on success.
    std::expected<size_t, ErrorCode> UdpSocket::send_to(std::span<const std::byte> data, const Endpoint& peer)
    {

        // your implementation: convert peer -> sockaddr_in, call ::sendto, translate errno on failure
        sockaddr_in addr { peer.to_sockaddr_in() };

        ssize_t bytes_sent { ::sendto(m_socket_fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) };
        
        if(bytes_sent == -1)
        {

            return std::unexpected(error_from_errno(errno));

        }

        return static_cast<size_t>(bytes_sent);

    }

    std::expected<std::pair<std::size_t, Endpoint>, ErrorCode> UdpSocket::recv_from(std::span<std::byte> buffer)
    {

        sockaddr_in addr {};
        socklen_t addr_len { sizeof(addr) };

        ssize_t bytes_received { ::recvfrom(m_socket_fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&addr), &addr_len) };
        if(bytes_received == -1)
        {

            return std::unexpected(error_from_errno(errno));

        }

        return std::pair<std::size_t, Endpoint> { static_cast<size_t>(bytes_received), Endpoint::from_sockaddr_in(addr) };

    }

    std::expected<void, ErrorCode> UdpSocket::set_nonblocking()
    {

        int flags { ::fcntl(m_socket_fd, F_GETFL, 0) };

        if (flags == -1)
        {

            return std::unexpected(error_from_errno(errno));

        }

        if (::fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {

            return std::unexpected(error_from_errno(errno));

        }

        return {};

    }

} // namespace maroof::net
