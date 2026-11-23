#pragma once

// Reading Nasdaq BinaryFILE captures (2-byte length + message, back to back),
// the format of the public sample files at emi.nasdaq.com/ITCH (gunzip first).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itch/parser.hpp"

namespace itch {

inline long long file_size(std::FILE* f) {
#if defined(_WIN32)
  _fseeki64(f, 0, SEEK_END);
  const long long n = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
#else
  fseeko(f, 0, SEEK_END);
  const long long n = ftello(f);
  fseeko(f, 0, SEEK_SET);
#endif
  return n;
}

// Loads a whole capture into memory (for benchmarks and the replay server).
inline bool read_file(const std::string& path, std::vector<char>& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const long long n = file_size(f);
  out.resize(static_cast<std::size_t>(n));
  const std::size_t got = n > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
  std::fclose(f);
  return got == out.size();
}

// Streams a capture of any size through the handler in large chunks, carrying
// a message split across a chunk boundary over to the next read.
template <class H>
inline StreamResult parse_file(const std::string& path, H& h, std::size_t chunk = std::size_t{1} << 26) {
  StreamResult total;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return total;
  std::vector<char> buf(chunk + 65536);
  std::size_t have = 0;
  for (;;) {
    const std::size_t got = std::fread(buf.data() + have, 1, chunk, f);
    if (got == 0) break;
    have += got;
    const StreamResult r = parse_stream(buf.data(), have, h);
    total.messages += r.messages;
    total.errors += r.errors;
    total.consumed += r.consumed;
    std::memmove(buf.data(), buf.data() + r.consumed, have - r.consumed);
    have -= r.consumed;
  }
  std::fclose(f);
  return total;
}

}  // namespace itch
