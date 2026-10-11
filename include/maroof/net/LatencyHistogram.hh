#ifndef MAROOF_NET_LATENCY_HISTOGRAM_HH
#define MAROOF_NET_LATENCY_HISTOGRAM_HH

#include <chrono>

// forward-declare the library opaque type so <hdr/hdr_histogram.h> stays out of this public header.
struct hdr_histogram;

namespace maroof::net
{

    // thin RAII wrapper over HdrHistogram_c. Records latency samples into log-linear buckets with
    // configurable precision; record() and percentile() are O(1) and allocation-free.
    class LatencyHistogram
    {
    public:

        // lowest / highest are the trackable range in ns; sig_digs within 1 to 5 controls precision
        // throws std::invalid_argument on bad args.
        LatencyHistogram(std::chrono::nanoseconds lowest, std::chrono::nanoseconds highest, int sig_digs = 3);
        ~LatencyHistogram();

        LatencyHistogram(const LatencyHistogram&)            = delete;
        LatencyHistogram& operator=(const LatencyHistogram&) = delete;

        // Move = transfer the owning pointer; source goes null and the dtor's null-guard handles it.
        // Copy stays deleted so you can't silently duplicate ~2 KB of bucket state behind your back.
        LatencyHistogram(LatencyHistogram&& other) noexcept;
        LatencyHistogram& operator=(LatencyHistogram&& other) noexcept;

        // record one sample. values outside [lowest, highest] saturate at the nearest edge.
        void record(std::chrono::nanoseconds sample_length) noexcept;

        // p is a percentile in [0.0, 100.0] — percentile(99.9) returns the value below which
        // 99.9% of recorded samples fall.
        std::chrono::nanoseconds percentile(double p) const;

    private:

        // owning pointer to the C library's histogram state (allocated by hdr_init, freed by hdr_close).
        // mon-null for the lifetime of the object — ctor throws instead of leaving it null.
        struct hdr_histogram* m_histogram { nullptr };

    }; // LatencyHistogram

} // namespace maroof::net

#endif // MAROOF_NET_LATENCY_HISTOGRAM_HH
