#pragma once
#include <cstddef>
#include <cstdint>

namespace indexcraft {

// Standard CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
// Pass the previous return value as `seed` to checksum data in several pieces.
std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t seed = 0);

}  // namespace indexcraft
