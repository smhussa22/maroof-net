#ifndef MAROOF_NET_IO_URING_REACTOR_HH
#define MAROOF_NET_IO_URING_REACTOR_HH

#include <maroof/net/Error.hh>

#include <cstddef>
#include <expected>
#include <span>

// Forward-declare so <liburing.h> stays out of this public header. The impl includes it.
struct io_uring;
struct msghdr;

namespace maroof::net
{

    // Thin RAII wrapper over liburing. Completion-based async I/O: callers submit ops into a
    // ring, the kernel processes them, callers reap results from a separate ring. On a well-
    // tuned setup (SQPOLL mode, registered buffers) this can run with zero syscalls on the
    // hot path. We don't enable SQPOLL by default — it burns a CPU core continuously and the
    // benefit only kicks in at very high packet rates.
    //
    // UDP-focused API: submit_recvmsg / submit_sendmsg take a caller-provided msghdr that must
    // outlive the submission (until the matching completion is reaped). Callers typically keep
    // one msghdr per outstanding slot and recycle them.
    class IoUringReactor
    {
    public:

        struct Completion
        {
            int   result;   // bytes transferred on success, -errno on failure
            void* data;     // user data pointer provided at submission time
        };

        // ring_entries sets both the SQ and CQ depth. Must be a power of 2 (liburing requirement).
        explicit IoUringReactor(unsigned ring_entries = 256);
        ~IoUringReactor();

        IoUringReactor(const IoUringReactor&)            = delete;
        IoUringReactor& operator=(const IoUringReactor&) = delete;

        IoUringReactor(IoUringReactor&& other) noexcept;
        IoUringReactor& operator=(IoUringReactor&& other) noexcept;

        // Queue a recvmsg. msg must stay alive and unmodified until the matching completion is reaped.
        // Returns false if the SQ is full (caller should submit() to drain and retry).
        [[nodiscard]] bool submit_recvmsg(int fd, msghdr* msg, void* data) noexcept;

        // Queue a sendmsg. Same lifetime contract as recvmsg.
        [[nodiscard]] bool submit_sendmsg(int fd, const msghdr* msg, void* data) noexcept;

        // Flush all queued submissions to the kernel. Returns the number submitted on success,
        // or an error on failure.
        [[nodiscard]] std::expected<unsigned, ErrorCode> submit() noexcept;

        // Block until at least one completion is available, then drain up to out.size().
        // Returns the number filled.
        [[nodiscard]] std::expected<std::size_t, ErrorCode>
        wait_batch(std::span<Completion> out) noexcept;

        // Non-blocking: drain whatever completions are already ready. Returns 0 if none.
        [[nodiscard]] std::size_t peek_batch(std::span<Completion> out) noexcept;

    private:

        // Owned by the ring; released in dtor via io_uring_queue_exit.
        // Allocated via `new` so the public header doesn't need to see io_uring's size/layout.
        struct io_uring* m_ring { nullptr };

    };

} // namespace maroof::net

#endif // MAROOF_NET_IO_URING_REACTOR_HH
