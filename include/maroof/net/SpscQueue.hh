#ifndef MAROOF_NET_SPSC_QUEUE_HH
#define MAROOF_NET_SPSC_QUEUE_HH

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace maroof::net
{

    // Single-producer / single-consumer lock-free ring buffer. Safe when exactly one thread calls
    // try_push() and exactly one (different) thread calls try_pop(). The two threads coordinate
    // only through two atomic indices — no mutex, no futex, no syscall on the hot path.
    //
    // Capacity must be a power of two so wrap-around is a cheap bitmask (& (N-1)) instead of %.
    //
    // Memory ordering follows the classic SPSC pattern:
    //   Producer:  load self (relaxed), load peer (acquire — see consumer's progress),
    //              write slot, store self (release — publish the write).
    //   Consumer:  symmetric.
    // Each thread writes only its own index; both read both. Separate cache lines prevent
    // false sharing between the two.
    template <typename T, std::size_t N>
    class SpscQueue
    {
        static_assert(N >= 2,                   "SpscQueue: capacity must be at least 2");
        static_assert((N & (N - 1)) == 0,       "SpscQueue: capacity must be a power of two");
        static_assert(std::is_nothrow_move_constructible_v<T> || std::is_nothrow_copy_constructible_v<T>,
                      "SpscQueue: T must be nothrow move- or copy-constructible");

    public:

        SpscQueue()  = default;
        ~SpscQueue() = default;

        SpscQueue(const SpscQueue&)            = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;
        SpscQueue(SpscQueue&&)                 = delete;
        SpscQueue& operator=(SpscQueue&&)      = delete;

        static constexpr std::size_t capacity() noexcept { return N; }

        // Producer-only. Returns false if the queue is full.
        [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_constructible_v<T>)
        {
            const auto w = m_write_idx.load(std::memory_order_relaxed);
            const auto r = m_read_idx.load (std::memory_order_acquire);
            if (w - r == N) return false;                        // full
            m_storage[w & (N - 1)] = item;
            m_write_idx.store(w + 1, std::memory_order_release); // publish
            return true;
        }

        [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            const auto w = m_write_idx.load(std::memory_order_relaxed);
            const auto r = m_read_idx.load (std::memory_order_acquire);
            if (w - r == N) return false;
            m_storage[w & (N - 1)] = std::move(item);
            m_write_idx.store(w + 1, std::memory_order_release);
            return true;
        }

        // Consumer-only. Returns std::nullopt if the queue is empty.
        [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            const auto r = m_read_idx.load (std::memory_order_relaxed);
            const auto w = m_write_idx.load(std::memory_order_acquire);
            if (r == w) return std::nullopt;                      // empty
            std::optional<T> out { std::move(m_storage[r & (N - 1)]) };
            m_read_idx.store(r + 1, std::memory_order_release);
            return out;
        }

        // Approximate size — snapshot of indices from the caller's thread, may be slightly stale
        // from the other thread's perspective. Fine for diagnostics; do not use for correctness.
        std::size_t approx_size() const noexcept
        {
            const auto w = m_write_idx.load(std::memory_order_acquire);
            const auto r = m_read_idx.load (std::memory_order_acquire);
            return w - r;
        }

        bool empty() const noexcept { return approx_size() == 0; }
        bool full()  const noexcept { return approx_size() == N; }

    private:

        // 64 is a safe guess for the destructive interference size on x86_64/ARM. Using the
        // standard library constant where available would be portable; hardcoding keeps this
        // header short and consistent across compilers that haven't shipped the constants.
        static constexpr std::size_t CACHE_LINE { 64 };

        // Separate cache lines for the two indices — otherwise every producer write would
        // invalidate the consumer's cache line (false sharing), killing the whole point.
        alignas(CACHE_LINE) std::atomic<std::size_t> m_write_idx { 0 };
        alignas(CACHE_LINE) std::atomic<std::size_t> m_read_idx  { 0 };

        alignas(CACHE_LINE) std::array<T, N> m_storage {};

    };

} // namespace maroof::net

#endif // MAROOF_NET_SPSC_QUEUE_HH
