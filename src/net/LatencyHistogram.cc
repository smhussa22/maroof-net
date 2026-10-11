#include <maroof/net/LatencyHistogram.hh>

#include <hdr/hdr_histogram.h>

#include <stdexcept>

namespace maroof::net
{

    LatencyHistogram::LatencyHistogram(std::chrono::nanoseconds lowest, std::chrono::nanoseconds highest, int sig_digs)
    {
        
        // hdr_init allocates the bucket array and returns 0 on success. 
        const int rc = hdr_init(lowest.count(), highest.count(), sig_digs, &m_histogram);
        if (rc != 0)
        {
            throw std::invalid_argument("LatencyHistogram: hdr_init failed (bad range or sig_digs)");
        }
    }

    LatencyHistogram::LatencyHistogram(LatencyHistogram&& other) noexcept
        : m_histogram { other.m_histogram }
    {
        other.m_histogram = nullptr;
    }

    LatencyHistogram& LatencyHistogram::operator=(LatencyHistogram&& other) noexcept
    {
        if (this != &other)
        {
            if (m_histogram) hdr_close(m_histogram);
            m_histogram       = other.m_histogram;
            other.m_histogram = nullptr;
        }
        return *this;
    }

    LatencyHistogram::~LatencyHistogram()
    {
        // Null-guard required: a moved-from object has m_histogram == nullptr, and hdr_close
        // dereferences its argument before freeing.
        if (m_histogram) hdr_close(m_histogram);
    }

    void LatencyHistogram::record(std::chrono::nanoseconds sample_length) noexcept
    {
        hdr_record_value(m_histogram, sample_length.count());
    }

    std::chrono::nanoseconds LatencyHistogram::percentile(double p) const
    {
        return std::chrono::nanoseconds { hdr_value_at_percentile(m_histogram, p) };
    }

} // namespace maroof::net
