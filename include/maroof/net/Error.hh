#ifndef MAROOF_NET_ERROR_HH
#define MAROOF_NET_ERROR_HH

#include <cerrno>

namespace maroof::net
{

    enum class ErrorCode
    {

        // The syscall would have blocked (non-blocking recv on empty queue, or send with full buffer).
        // Try again later. Callers should handle this specifically.
        WouldBlock,

        // A signal interrupted the syscall (EINTR). Safe to retry the same call.
        Interrupted,

        // Catch-all for any errno we don't specifically translate. Consult errno at the throw site if needed.
        Unknown,

    };

    // Maps a POSIX errno value to ErrorCode.
    // Kept inline so every translation unit can use it without linking a separate .cc.
    inline ErrorCode error_from_errno(int errno_value) noexcept
    {

        switch (errno_value)
        {

            case EAGAIN:
                //Both are 11 on Linux
                // Have this as an extra guard
                #if EAGAIN != EWOULDBLOCK
                    case EWOULDBLOCK:
                #endif
                return ErrorCode::WouldBlock;

            case EINTR:
                return ErrorCode::Interrupted;

            default:
                return ErrorCode::Unknown;

        }

    }

} // namespace maroof::net

#endif // MAROOF_NET_ERROR_HH
