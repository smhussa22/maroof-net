#include <gtest/gtest.h>

#include <maroof/net/LatencyHistogram.hh>

#include <chrono>
#include <stdexcept>

using namespace maroof::net;
using namespace std::chrono_literals;

TEST(LatencyHistogram, EmptyPercentileIsZero)
{
    LatencyHistogram h { 1ns, 60s };
    EXPECT_EQ(h.percentile(50.0),  0ns);
    EXPECT_EQ(h.percentile(99.9),  0ns);
    EXPECT_EQ(h.percentile(100.0), 0ns);
}

TEST(LatencyHistogram, SingleSampleShowsUpAtEveryPercentile)
{
    LatencyHistogram h { 1ns, 60s };
    h.record(1ms);

    // With one sample, p0 / p50 / p100 all round-trip to roughly 1ms. 3-sigfig quantization
    // puts us within ~0.1% of the recorded value — 10µs of slack is way more than enough.
    EXPECT_NEAR(static_cast<double>(h.percentile(  0.0).count()), 1'000'000.0, 10'000.0);
    EXPECT_NEAR(static_cast<double>(h.percentile( 50.0).count()), 1'000'000.0, 10'000.0);
    EXPECT_NEAR(static_cast<double>(h.percentile(100.0).count()), 1'000'000.0, 10'000.0);
}

TEST(LatencyHistogram, DistributionTailShowsUpAtTopPercentile)
{
    LatencyHistogram h { 1ns, 60s };
    for (int i = 0; i < 100; ++i) h.record(1ms);
    h.record(100ms);                       // one straggler — the "tail"

    // Bulk sits at p50/p99 near 1ms, the single straggler surfaces at p100.
    EXPECT_NEAR(static_cast<double>(h.percentile( 50.0).count()),   1'000'000.0,    10'000.0);
    EXPECT_NEAR(static_cast<double>(h.percentile( 99.0).count()),   1'000'000.0,    10'000.0);
    EXPECT_NEAR(static_cast<double>(h.percentile(100.0).count()), 100'000'000.0, 1'000'000.0);
}

TEST(LatencyHistogram, BadRangeThrows)
{
    // hdr_init requires highest >= 2 * lowest. Giving highest < lowest must trip the throw.
    EXPECT_THROW(LatencyHistogram(100ms, 50ms), std::invalid_argument);
}
