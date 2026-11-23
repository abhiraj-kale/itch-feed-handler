// Writes a synthetic ITCH 5.0 session in Nasdaq BinaryFILE format.
//
// Usage: itch_gen <out.itch> [messages=10000000] [stocks=500] [seed=1]

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "itch/endian.hpp"
#include "itch/generator.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: itch_gen <out.itch> [messages=10000000] [stocks=500] [seed=1]\n");
    return 2;
  }
  itch::GenConfig cfg;
  cfg.messages = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 10'000'000;
  cfg.stocks = argc > 3 ? static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10)) : 500;
  cfg.seed = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;

  std::FILE* f = std::fopen(argv[1], "wb");
  if (!f) {
    std::perror(argv[1]);
    return 1;
  }
  std::vector<char> buf;
  buf.reserve(1 << 22);
  std::uint64_t n = 0, bytes = 0;
  itch::Generator gen(cfg);
  gen.run([&](const char* m, std::size_t len) {
    const std::size_t off = buf.size();
    buf.resize(off + 2 + len);
    itch::put16(buf.data() + off, static_cast<std::uint16_t>(len));
    std::memcpy(buf.data() + off + 2, m, len);
    ++n;
    if (buf.size() > (1 << 22) - 64) {
      std::fwrite(buf.data(), 1, buf.size(), f);
      bytes += buf.size();
      buf.clear();
    }
  });
  std::fwrite(buf.data(), 1, buf.size(), f);
  bytes += buf.size();
  std::fclose(f);
  std::printf("wrote %llu messages (%.1f MB), %u stocks, %llu orders left resting -> %s\n",
              static_cast<unsigned long long>(n), static_cast<double>(bytes) / 1e6, cfg.stocks,
              static_cast<unsigned long long>(gen.live_orders()), argv[1]);
  return 0;
}
