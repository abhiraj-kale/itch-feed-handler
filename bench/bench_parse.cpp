// Parser and book-builder throughput on a capture held in memory.
//
//   1. framing + dispatch: walk the length prefixes, switch on type, read one field
//   2. full decode: also decode every field of every order message
//   3. parse + book: rebuild every instrument's book (BookBuilder)
//   4. per-message latency of parse + book update, TSC timed, overhead subtracted
//
// Usage: bench_parse <file.itch> [cpu=2]
//        bench_parse --gen [messages=20000000] [stocks=500] [cpu=2]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "itch/book.hpp"
#include "itch/file.hpp"
#include "itch/generator.hpp"
#include "itch/parser.hpp"
#include "itch/platform.hpp"

using namespace itch;

namespace {

struct TouchOne : NullHandler {  // reads a single field so the dispatch is not optimized out
  std::uint64_t sum = 0;
  void on_add(AddOrder m) { sum += m.locate(); }
  void on_executed(OrderExecuted m) { sum += m.locate(); }
  void on_executed_with_price(OrderExecutedWithPrice m) { sum += m.locate(); }
  void on_cancel(OrderCancel m) { sum += m.locate(); }
  void on_delete(OrderDelete m) { sum += m.locate(); }
  void on_replace(OrderReplace m) { sum += m.locate(); }
  void on_trade(Trade m) { sum += m.locate(); }
  void on_other(Header m) { sum += m.locate(); }
};

struct DecodeAll : NullHandler {
  std::uint64_t sum = 0;
  void on_add(AddOrder m) { sum += m.order_ref() + m.shares() + m.price() + static_cast<unsigned char>(m.side()) + m.timestamp() + static_cast<unsigned char>(m.stock()[0]); }
  void on_executed(OrderExecuted m) { sum += m.order_ref() + m.executed_shares() + m.match_number() + m.timestamp(); }
  void on_executed_with_price(OrderExecutedWithPrice m) { sum += m.order_ref() + m.executed_shares() + m.price() + m.timestamp(); }
  void on_cancel(OrderCancel m) { sum += m.order_ref() + m.cancelled_shares() + m.timestamp(); }
  void on_delete(OrderDelete m) { sum += m.order_ref() + m.timestamp(); }
  void on_replace(OrderReplace m) { sum += m.original_ref() + m.new_ref() + m.shares() + m.price() + m.timestamp(); }
  void on_trade(Trade m) { sum += m.order_ref() + m.shares() + m.price() + m.match_number(); }
  void on_other(Header m) { sum += m.timestamp(); }
};

template <class F>
double best_of(int reps, F&& f) {
  double best = 1e30;
  for (int i = 0; i < reps; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (s < best) best = s;
  }
  return best;
}

void line(const char* name, std::uint64_t msgs, std::size_t bytes, double secs) {
  std::printf("  %-26s %7.1f M msgs/s | %6.2f ns/msg | %6.2f GB/s\n", name, static_cast<double>(msgs) / secs / 1e6,
              secs * 1e9 / static_cast<double>(msgs), static_cast<double>(bytes) / secs / 1e9);
}

// Captures too big for RAM (a full Nasdaq day is ~12 GB): read 1 GB at a time,
// time only the parsing (never the disk), keep one book across chunks, and
// record per-message latency in a histogram instead of a sample vector.
int stream_bench(const char* path, int cpu) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
  const bool pinned = pin_thread(cpu);
  const double tsc_per_ns = calibrate_tsc_per_ns();
  const std::uint64_t overhead = measure_tsc_overhead();
  const std::size_t CHUNK = std::size_t{1} << 30;
  std::vector<char> buf(CHUNK + 65536);
  auto book = std::make_unique<BookBuilder<>>();
  auto lat_book = std::make_unique<BookBuilder<>>();
  constexpr std::size_t HB = 1 << 16;  // histogram buckets in TSC ticks; last one is overflow
  std::vector<std::uint64_t> hist(HB, 0);
  double t_frame = 0, t_decode = 0, t_book = 0;
  std::uint64_t msgs = 0, bytes = 0, errors = 0, sink = 0;
  std::size_t have = 0;
  using clk = std::chrono::steady_clock;
  for (;;) {
    const std::size_t got = std::fread(buf.data() + have, 1, CHUNK, f);
    if (got == 0) break;
    have += got;
    { TouchOne h; const auto t0 = clk::now(); parse_stream(buf.data(), have, h); t_frame += std::chrono::duration<double>(clk::now() - t0).count(); sink += h.sum; }
    { DecodeAll h; const auto t0 = clk::now(); parse_stream(buf.data(), have, h); t_decode += std::chrono::duration<double>(clk::now() - t0).count(); sink += h.sum; }
    const auto t0 = clk::now();
    const StreamResult r = parse_stream(buf.data(), have, *book);
    t_book += std::chrono::duration<double>(clk::now() - t0).count();
    for (std::size_t off = 0; off < r.consumed;) {
      const std::size_t len = be16(buf.data() + off);
      const std::uint64_t a = tsc_begin();
      dispatch(buf.data() + off + 2, len, *lat_book);
      const std::uint64_t b = tsc_end();
      const std::uint64_t d = b - a > overhead ? b - a - overhead : 0;
      ++hist[d < HB - 1 ? d : HB - 1];
      off += 2 + len;
    }
    msgs += r.messages; errors += r.errors; bytes += r.consumed;
    std::memmove(buf.data(), buf.data() + r.consumed, have - r.consumed);
    have -= r.consumed;
    std::fprintf(stderr, "  %.1f GB, %llu M messages, %zu live orders\r", static_cast<double>(bytes) / 1e9,
                 static_cast<unsigned long long>(msgs / 1000000), book->live_orders());
  }
  std::fclose(f);
  auto pct = [&](double q) {
    const std::uint64_t target = static_cast<std::uint64_t>(q * static_cast<double>(msgs - 1));
    std::uint64_t c = 0;
    for (std::size_t i = 0; i < HB; ++i) { c += hist[i]; if (c > target) return static_cast<double>(i) / tsc_per_ns; }
    return static_cast<double>(HB) / tsc_per_ns;
  };
  std::printf("\n%s: %llu messages, %.2f GB, %llu parse errors | cpu %d%s | TSC %.3f GHz\n\n", path,
              static_cast<unsigned long long>(msgs), static_cast<double>(bytes) / 1e9,
              static_cast<unsigned long long>(errors), cpu, pinned ? " (pinned)" : "", tsc_per_ns);
  line("framing + dispatch", msgs, bytes, t_frame);
  line("full field decode", msgs, bytes, t_decode);
  line("parse + book update", msgs, bytes, t_book);
  std::printf("\n  parse + book, per message  p50 %7.0f ns | p90 %7.0f ns | p99 %7.0f ns | p99.9 %8.0f ns\n", pct(0.5),
              pct(0.9), pct(0.99), pct(0.999));
  const BookStats& st = book->stats();
  std::printf("\nbook: %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces, %llu unknown refs, %zu live orders at end\n",
              static_cast<unsigned long long>(st.adds), static_cast<unsigned long long>(st.executions),
              static_cast<unsigned long long>(st.cancels), static_cast<unsigned long long>(st.deletes),
              static_cast<unsigned long long>(st.replaces), static_cast<unsigned long long>(st.unknown_ref),
              book->live_orders());
  // A correct rebuild of the exchange's displayed book is never crossed.
  std::size_t symbols = 0, two_sided = 0, crossed = 0;
  for (std::uint32_t loc = 0; loc < 65536; ++loc) {
    const auto l = static_cast<std::uint16_t>(loc);
    if (!book->known(l)) continue;
    ++symbols;
    const TopOfBook& t = book->top(l);
    if (t.bid_price && t.ask_price) { ++two_sided; crossed += t.bid_price >= t.ask_price; }
  }
  std::printf("symbols: %zu in directory, %zu with both sides at end, %zu crossed\n", symbols, two_sided, crossed);
  if (sink == 42) std::printf(" ");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<char> data;
  std::string source;
  int cpu = 2;
  if (argc > 1 && std::strcmp(argv[1], "--gen") == 0) {
    GenConfig cfg;
    cfg.messages = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20'000'000;
    cfg.stocks = argc > 3 ? static_cast<std::uint32_t>(std::atoi(argv[3])) : 500;
    cpu = argc > 4 ? std::atoi(argv[4]) : 2;
    data.reserve(cfg.messages * 36);
    Generator gen(cfg);
    gen.run([&](const char* m, std::size_t len) {
      const std::size_t off = data.size();
      data.resize(off + 2 + len);
      put16(data.data() + off, static_cast<std::uint16_t>(len));
      std::memcpy(data.data() + off + 2, m, len);
    });
    source = "synthetic (" + std::to_string(cfg.stocks) + " stocks)";
  } else if (argc > 2 && std::strcmp(argv[1], "--stream") == 0) {
    return stream_bench(argv[2], argc > 3 ? std::atoi(argv[3]) : 2);
  } else if (argc > 1) {
    if (!read_file(argv[1], data)) {
      std::fprintf(stderr, "cannot read %s\n", argv[1]);
      return 1;
    }
    source = argv[1];
    cpu = argc > 2 ? std::atoi(argv[2]) : 2;
  } else {
    std::fprintf(stderr, "usage: bench_parse <file.itch> [cpu] | bench_parse --gen [messages] [stocks] [cpu]\n");
    return 2;
  }

  const bool pinned = pin_thread(cpu);
  const double tsc_per_ns = calibrate_tsc_per_ns();
  const std::uint64_t overhead = measure_tsc_overhead();
  const std::size_t bytes = data.size();

  std::uint64_t msgs = 0;
  {
    TouchOne h;
    msgs = parse_stream(data.data(), bytes, h).messages;
  }
  std::printf("%s: %llu messages, %.1f MB | cpu %d%s | TSC %.3f GHz\n\n", source.c_str(),
              static_cast<unsigned long long>(msgs), static_cast<double>(bytes) / 1e6, cpu,
              pinned ? " (pinned)" : "", tsc_per_ns);

  std::uint64_t sink = 0;
  const double s1 = best_of(3, [&] { TouchOne h; parse_stream(data.data(), bytes, h); sink += h.sum; });
  line("framing + dispatch", msgs, bytes, s1);
  const double s2 = best_of(3, [&] { DecodeAll h; parse_stream(data.data(), bytes, h); sink += h.sum; });
  line("full field decode", msgs, bytes, s2);

  std::size_t live = 0;
  BookStats st{};
  const double s3 = best_of(3, [&] {
    auto b = std::make_unique<BookBuilder<>>();
    parse_stream(data.data(), bytes, *b);
    live = b->live_orders();
    st = b->stats();
  });
  line("parse + book update", msgs, bytes, s3);

  // Per-message latency: time each dispatch individually.
  std::vector<std::uint64_t> lat;
  lat.reserve(msgs);
  {
    auto b = std::make_unique<BookBuilder<>>();
    std::size_t off = 0;
    while (off + 2 <= bytes) {
      const std::size_t len = be16(data.data() + off);
      const std::uint64_t t0 = tsc_begin();
      dispatch(data.data() + off + 2, len, *b);
      const std::uint64_t t1 = tsc_end();
      lat.push_back(t1 - t0 > overhead ? t1 - t0 - overhead : 0);
      off += 2 + len;
    }
  }
  std::printf("\n");
  print_row("parse + book, per message", percentiles_ns(std::move(lat), tsc_per_ns));
  std::printf("\nbook: %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces, %llu unknown refs, %zu live orders at end\n",
              static_cast<unsigned long long>(st.adds), static_cast<unsigned long long>(st.executions),
              static_cast<unsigned long long>(st.cancels), static_cast<unsigned long long>(st.deletes),
              static_cast<unsigned long long>(st.replaces), static_cast<unsigned long long>(st.unknown_ref), live);
  if (sink == 42) std::printf(" ");
  return 0;
}
