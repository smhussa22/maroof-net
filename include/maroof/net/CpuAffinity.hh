#ifndef MAROOF_NET_CPU_AFFINITY_HH
#define MAROOF_NET_CPU_AFFINITY_HH

#if defined(__linux__)
#   include <sched.h>
#endif

namespace maroof::net
{

    // Pin the calling thread to a single CPU core. Thin wrapper over sched_setaffinity so the
    // library doesn't impose scheduling choices — the caller opts in. Linux-only; returns false
    // on other platforms (and on syscall failure: bad cpu_id, permission denied, etc.).
    //
    // Typical use: in main() right after flag parsing, before any hot-path work.
    inline bool pin_current_thread_to(int cpu_id) noexcept
    {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu_id, &set);
        return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
        (void) cpu_id;
        return false;
#endif
    }

} // namespace maroof::net

#endif // MAROOF_NET_CPU_AFFINITY_HH
