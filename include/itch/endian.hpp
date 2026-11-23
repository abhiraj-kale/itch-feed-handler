#pragma once

// Big-endian loads and stores straight from a byte buffer. memcpy + bswap
// compiles to a single mov + bswap (or movbe) with no alignment requirement,
// which is what lets the parser read fields in place without copying messages.

#include <cstdint>
#include <cstring>

namespace itch {

inline std::uint16_t be16(const char* p) {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline std::uint32_t be32(const char* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline std::uint64_t be64(const char* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
// ITCH timestamps are 6-byte nanoseconds since midnight.
inline std::uint64_t be48(const char* p) {
  return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2);
}

inline void put16(char* p, std::uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void put32(char* p, std::uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void put64(char* p, std::uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline void put48(char* p, std::uint64_t v) {
  put16(p, static_cast<std::uint16_t>(v >> 32));
  put32(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace itch
