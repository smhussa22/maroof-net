#ifndef MAROOF_NET_CONNECTION_STATS_HH
#define MAROOF_NET_CONNECTION_STATS_HH

#include <cstdint>

namespace maroof::net
{

    // Lightweight per-connection counters for observability. Updated on the hot path, read via
    // Connection::stats(). All counters are plain u64; no atomics — a connection is single-threaded.
    struct ConnectionStats
    {

        // Packets (datagrams) + bytes crossing the wire for THIS connection.
        std::uint64_t packets_sent       { 0 };
        std::uint64_t packets_received   { 0 };
        std::uint64_t bytes_sent         { 0 };
        std::uint64_t bytes_received     { 0 };

        // Reliable-channel protocol events worth watching.
        std::uint64_t retransmits               { 0 };  // tick() fired a timer
        std::uint64_t duplicate_acks_ignored    { 0 };  // ACK for already-cleared seq
        std::uint64_t beyond_window_dropped     { 0 };  // seq too far ahead; receiver refused
        std::uint64_t below_window_reacked      { 0 };  // seq already delivered; re-ACKed
        std::uint64_t malformed_dropped         { 0 };  // parse() failed

    };

} // namespace maroof::net

#endif // MAROOF_NET_CONNECTION_STATS_HH
