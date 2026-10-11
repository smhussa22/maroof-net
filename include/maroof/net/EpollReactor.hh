#ifndef MAROOF_NET_EPOLL_REACTOR_HH
#define MAROOF_NET_EPOLL_REACTOR_HH

#include <maroof/net/Error.hh>

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>

namespace maroof::net
{

    // Thin RAII wrapper over Linux epoll. Level-triggered by default — simpler semantics than
    // edge-triggered and the per-event cost difference is negligible for UDP workloads where
    // one wakeup drains the whole socket via recvmmsg anyway.
    //
    // Use case: multi-fd servers that need to know which fd has data without a thread per fd.
    // For single-fd clients (latency_bench, echo_client) the plain non-blocking recv loop is
    // still simpler and often faster.
    class EpollReactor
    {
    public:

        struct Event
        {
            int   fd;
            void* data;   // opaque pointer passed back from the matching add_read call
        };

        EpollReactor();
        ~EpollReactor();

        EpollReactor(const EpollReactor&)            = delete;
        EpollReactor& operator=(const EpollReactor&) = delete;

        EpollReactor(EpollReactor&& other) noexcept;
        EpollReactor& operator=(EpollReactor&& other) noexcept;

        // Register fd for read-ready notifications. `data` surfaces back on wait() to let
        // the caller recover its own per-fd context (connection, channel, etc) with no map lookup.
        [[nodiscard]] std::expected<void, ErrorCode> add_read(int fd, void* data);

        [[nodiscard]] std::expected<void, ErrorCode> remove(int fd);

        // Block until at least one fd is ready or `timeout` elapses. Writes up to out.size()
        // events into `out` and returns how many were filled. timeout == -1 → block forever.
        [[nodiscard]] std::expected<std::size_t, ErrorCode> wait(std::span<Event> out,
                                                                 std::chrono::milliseconds timeout) noexcept;

        int fd() const noexcept { return m_epoll_fd; }

    private:

        int m_epoll_fd { -1 };

    };

} // namespace maroof::net

#endif // MAROOF_NET_EPOLL_REACTOR_HH
