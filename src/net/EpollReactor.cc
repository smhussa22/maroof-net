#include <maroof/net/EpollReactor.hh>

#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace maroof::net
{

    EpollReactor::EpollReactor()
        : m_epoll_fd { ::epoll_create1(EPOLL_CLOEXEC) }
    {
        if (m_epoll_fd == -1)
        {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
    }

    EpollReactor::~EpollReactor()
    {
        if (m_epoll_fd != -1) ::close(m_epoll_fd);
    }

    EpollReactor::EpollReactor(EpollReactor&& other) noexcept
        : m_epoll_fd { other.m_epoll_fd }
    {
        other.m_epoll_fd = -1;
    }

    EpollReactor& EpollReactor::operator=(EpollReactor&& other) noexcept
    {
        if (this != &other)
        {
            if (m_epoll_fd != -1) ::close(m_epoll_fd);
            m_epoll_fd       = other.m_epoll_fd;
            other.m_epoll_fd = -1;
        }
        return *this;
    }

    std::expected<void, ErrorCode> EpollReactor::add_read(int fd, void* data)
    {
        epoll_event ev {};
        ev.events   = EPOLLIN;                              // level-triggered read-ready
        ev.data.ptr = data;
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            return std::unexpected(error_from_errno(errno));
        }
        return {};
    }

    std::expected<void, ErrorCode> EpollReactor::remove(int fd)
    {
        if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr) == -1)
        {
            return std::unexpected(error_from_errno(errno));
        }
        return {};
    }

    std::expected<std::size_t, ErrorCode>
    EpollReactor::wait(std::span<Event> out, std::chrono::milliseconds timeout) noexcept
    {
        // Fixed scratch buffer so wait() never allocates; cap matches a reasonable
        // "events per wakeup" ceiling, and we loop internally if `out` is larger.
        static constexpr std::size_t WAIT_BATCH { 64 };
        std::array<epoll_event, WAIT_BATCH> raw {};

        const int n_requested = static_cast<int>(std::min(out.size(), WAIT_BATCH));
        const int ms          = (timeout.count() < 0) ? -1 : static_cast<int>(timeout.count());

        const int got = ::epoll_wait(m_epoll_fd, raw.data(), n_requested, ms);
        if (got == -1)
        {
            if (errno == EINTR) return 0u;  // signal — not an error for our callers
            return std::unexpected(error_from_errno(errno));
        }

        for (int i = 0; i < got; ++i)
        {
            // We only track fd for diagnostics; the opaque data pointer is what the caller cares about.
            // epoll_event.data is a union — we always use .ptr, so pull it back out verbatim.
            out[i].fd   = -1;
            out[i].data = raw[i].data.ptr;
        }
        return static_cast<std::size_t>(got);
    }

} // namespace maroof::net
