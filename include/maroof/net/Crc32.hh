#ifndef MAROOF_NET_CRC32_HH
#define MAROOF_NET_CRC32_HH

#include <cstddef>
#include <cstdint>
#include <span>

namespace maroof::net
{

    // IEEE 802.3 CRC-32 (polynomial 0xEDB88320, reflected form). Same variant used
    // by Ethernet, gzip, PNG, and most application-level UDP protocols that add
    // their own integrity check.
    //
    // Table-driven for speed. Incremental so you can hash a header and payload
    // without concatenating them first.
    class Crc32
    {
    public:

        // Feed more bytes into the running CRC.
        void update(std::span<const std::byte> data) noexcept;

        // The finalized checksum. The XOR with 0xFFFFFFFF is IEEE 802.3's
        // required output post-processing.
        std::uint32_t value() const noexcept { return m_state ^ 0xFFFFFFFFu; }

    private:

        // Init is all-ones. Starting at zero would make the CRC of any all-zero
        // input also zero, which lets corruption of leading bytes go undetected.
        std::uint32_t m_state { 0xFFFFFFFFu };

    };

} // namespace maroof::net

#endif // MAROOF_NET_CRC32_HH
