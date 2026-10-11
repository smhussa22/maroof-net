#include <maroof/net/UdpSocket.hh>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>
#include <array>
#include <cstring>
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

    std::expected<std::size_t, ErrorCode> UdpSocket::recv_many(std::span<RecvSlot> slots) noexcept
    {

        const std::size_t n = std::min(slots.size(), BATCH_LIMIT);
        if (n == 0) return 0u;

        // Build fixed-size parallel arrays on the stack. One mmsghdr per slot, each holding
        // one iovec (points at the caller's buffer) and one sockaddr_in (filled by the kernel).
        std::array<mmsghdr,     BATCH_LIMIT> msgs {};
        std::array<iovec,       BATCH_LIMIT> iovs {};
        std::array<sockaddr_in, BATCH_LIMIT> addrs {};

        for (std::size_t i = 0; i < n; ++i)
        {
            iovs[i].iov_base       = slots[i].buf.data();
            iovs[i].iov_len        = slots[i].buf.size();
            msgs[i].msg_hdr.msg_iov      = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen   = 1;
            msgs[i].msg_hdr.msg_name     = &addrs[i];
            msgs[i].msg_hdr.msg_namelen  = sizeof(sockaddr_in);
            msgs[i].msg_hdr.msg_control  = nullptr;
            msgs[i].msg_hdr.msg_controllen = 0;
            msgs[i].msg_hdr.msg_flags    = 0;
            msgs[i].msg_len              = 0;
        }

        const int got = ::recvmmsg(m_socket_fd, msgs.data(), static_cast<unsigned int>(n), 0, nullptr);
        if (got == -1)
        {
            return std::unexpected(error_from_errno(errno));
        }

        // Populate the slot outputs. msgs[i].msg_len is the datagram length; msg_hdr.msg_name
        // may have been overwritten with a shorter namelen for shorter sockaddr variants (not
        // relevant for IPv4 here).
        for (int i = 0; i < got; ++i)
        {
            slots[i].received = msgs[i].msg_len;
            slots[i].peer     = Endpoint::from_sockaddr_in(addrs[i]);
        }

        return static_cast<std::size_t>(got);

    }

    std::expected<std::size_t, ErrorCode> UdpSocket::send_many(std::span<const SendSlot> slots) noexcept
    {

        const std::size_t n = std::min(slots.size(), BATCH_LIMIT);
        if (n == 0) return 0u;

        std::array<mmsghdr,     BATCH_LIMIT> msgs {};
        std::array<iovec,       BATCH_LIMIT> iovs {};
        std::array<sockaddr_in, BATCH_LIMIT> addrs {};

        for (std::size_t i = 0; i < n; ++i)
        {
            addrs[i] = slots[i].peer.to_sockaddr_in();

            // iov_base is non-const; the kernel treats send iovecs as read-only but the struct
            // field is void*. cast away const here is the standard pattern (and safe).
            iovs[i].iov_base       = const_cast<std::byte*>(slots[i].bytes.data());
            iovs[i].iov_len        = slots[i].bytes.size();
            msgs[i].msg_hdr.msg_iov      = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen   = 1;
            msgs[i].msg_hdr.msg_name     = &addrs[i];
            msgs[i].msg_hdr.msg_namelen  = sizeof(sockaddr_in);
            msgs[i].msg_hdr.msg_control  = nullptr;
            msgs[i].msg_hdr.msg_controllen = 0;
            msgs[i].msg_hdr.msg_flags    = 0;
            msgs[i].msg_len              = 0;
        }

        const int sent = ::sendmmsg(m_socket_fd, msgs.data(), static_cast<unsigned int>(n), 0);
        if (sent == -1)
        {
            return std::unexpected(error_from_errno(errno));
        }
        return static_cast<std::size_t>(sent);

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
