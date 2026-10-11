#include <maroof/net/IoUringReactor.hh>

#include <liburing.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

namespace maroof::net
{

    IoUringReactor::IoUringReactor(unsigned ring_entries)
        : m_ring { static_cast<io_uring*>(std::malloc(sizeof(io_uring))) }
    {
        if (!m_ring)
        {
            throw std::system_error(ENOMEM, std::generic_category(), "io_uring: malloc");
        }
        std::memset(m_ring, 0, sizeof(io_uring));

        // io_uring_queue_init returns a NEGATIVE errno on failure (liburing convention),
        // not -1 + errno. Treat the magnitude as the actual error code.
        const int rc = io_uring_queue_init(ring_entries, m_ring, 0);
        if (rc < 0)
        {
            std::free(m_ring);
            m_ring = nullptr;
            throw std::system_error(-rc, std::generic_category(), "io_uring_queue_init");
        }
    }

    IoUringReactor::~IoUringReactor()
    {
        if (m_ring)
        {
            io_uring_queue_exit(m_ring);
            std::free(m_ring);
        }
    }

    IoUringReactor::IoUringReactor(IoUringReactor&& other) noexcept
        : m_ring { other.m_ring }
    {
        other.m_ring = nullptr;
    }

    IoUringReactor& IoUringReactor::operator=(IoUringReactor&& other) noexcept
    {
        if (this != &other)
        {
            if (m_ring)
            {
                io_uring_queue_exit(m_ring);
                std::free(m_ring);
            }
            m_ring       = other.m_ring;
            other.m_ring = nullptr;
        }
        return *this;
    }

    bool IoUringReactor::submit_recvmsg(int fd, msghdr* msg, void* data) noexcept
    {
        io_uring_sqe* sqe = io_uring_get_sqe(m_ring);
        if (!sqe) return false;                     // SQ full — caller should submit() and retry
        io_uring_prep_recvmsg(sqe, fd, msg, 0);
        io_uring_sqe_set_data(sqe, data);
        return true;
    }

    bool IoUringReactor::submit_sendmsg(int fd, const msghdr* msg, void* data) noexcept
    {
        io_uring_sqe* sqe = io_uring_get_sqe(m_ring);
        if (!sqe) return false;
        // liburing's send prep wants a non-const msghdr* even for send (API quirk) — cast is safe.
        io_uring_prep_sendmsg(sqe, fd, const_cast<msghdr*>(msg), 0);
        io_uring_sqe_set_data(sqe, data);
        return true;
    }

    std::expected<unsigned, ErrorCode> IoUringReactor::submit() noexcept
    {
        const int rc = io_uring_submit(m_ring);
        if (rc < 0) return std::unexpected(error_from_errno(-rc));
        return static_cast<unsigned>(rc);
    }

    std::expected<std::size_t, ErrorCode>
    IoUringReactor::wait_batch(std::span<Completion> out) noexcept
    {
        if (out.empty()) return 0u;

        // Block for the first one; the rest are drained non-blocking below.
        io_uring_cqe* cqe = nullptr;
        const int wait_rc = io_uring_wait_cqe(m_ring, &cqe);
        if (wait_rc < 0)
        {
            if (wait_rc == -EINTR) return 0u;      // signal — benign for our callers
            return std::unexpected(error_from_errno(-wait_rc));
        }

        std::size_t n = 0;
        out[n].result = cqe->res;
        out[n].data   = io_uring_cqe_get_data(cqe);
        io_uring_cqe_seen(m_ring, cqe);
        ++n;

        // Drain any additional ready completions without blocking.
        while (n < out.size())
        {
            const int peek_rc = io_uring_peek_cqe(m_ring, &cqe);
            if (peek_rc == -EAGAIN) break;         // no more ready
            if (peek_rc < 0) break;                // other error — just return what we have
            out[n].result = cqe->res;
            out[n].data   = io_uring_cqe_get_data(cqe);
            io_uring_cqe_seen(m_ring, cqe);
            ++n;
        }
        return n;
    }

    std::size_t IoUringReactor::peek_batch(std::span<Completion> out) noexcept
    {
        std::size_t n = 0;
        while (n < out.size())
        {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(m_ring, &cqe) < 0) break;
            out[n].result = cqe->res;
            out[n].data   = io_uring_cqe_get_data(cqe);
            io_uring_cqe_seen(m_ring, cqe);
            ++n;
        }
        return n;
    }

} // namespace maroof::net
