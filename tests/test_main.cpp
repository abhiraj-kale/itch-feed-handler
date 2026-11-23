// Self-contained tests (no external dependencies).

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "itch/book.hpp"
#include "itch/file.hpp"
#include "itch/generator.hpp"
#include "itch/moldudp64.hpp"
#include "itch/order_map.hpp"
#include "itch/parser.hpp"

using namespace itch;

namespace {

struct TestCase {
  const char* name;
  std::function<void()> fn;
};
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
struct Register {
  Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
int g_failures = 0;

#define TEST(name)                        \
  void name();                            \
  const Register reg_##name(#name, name); \
  void name()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      ++g_failures;                                                              \
      std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
    }                                                                            \
  } while (0)

// Generated session as one BinaryFILE buffer.
std::vector<char> generate(std::uint64_t messages, std::uint32_t stocks, std::uint64_t seed) {
  std::vector<char> out;
  GenConfig cfg;
  cfg.messages = messages;
  cfg.stocks = stocks;
  cfg.seed = seed;
  Generator g(cfg);
  g.run([&](const char* m, std::size_t len) {
    const std::size_t off = out.size();
    out.resize(off + 2 + len);
    put16(out.data() + off, static_cast<std::uint16_t>(len));
    std::memcpy(out.data() + off + 2, m, len);
  });
  return out;
}

// Splits a BinaryFILE buffer into individual messages.
std::vector<std::string> split(const std::vector<char>& buf) {
  std::vector<std::string> msgs;
  for (std::size_t off = 0; off + 2 <= buf.size();) {
    const std::size_t len = be16(buf.data() + off);
    msgs.emplace_back(buf.data() + off + 2, len);
    off += 2 + len;
  }
  return msgs;
}

}  // namespace

// ---------------------------------------------------------------- parsing

TEST(message_sizes_match_the_spec) {
  const std::pair<char, int> spec[] = {{'S', 12}, {'R', 39}, {'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35},
                                       {'W', 12}, {'K', 28}, {'J', 35}, {'h', 21}, {'A', 36}, {'F', 40},
                                       {'E', 31}, {'C', 36}, {'X', 23}, {'D', 19}, {'U', 35}, {'P', 44},
                                       {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
  int known = 0;
  for (auto [t, n] : spec) {
    CHECK(message_size(t) == static_cast<std::size_t>(n));
    ++known;
  }
  for (int c = 0; c < 256; ++c) known -= message_size(static_cast<char>(c)) != 0;
  CHECK(known == 0);  // nothing outside the spec has a size
}

TEST(add_order_fields_decode_from_raw_bytes) {
  // Built byte by byte from the spec, independent of the encoder helpers.
  const unsigned char raw[36] = {'A',  0x00, 0x2A, 0x00, 0x07, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xE2, 0x40, 'S',  0x00, 0x00,
                                 0x01, 0x2C, 'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',  0x00,
                                 0x1A, 0x6E, 0x34};
  const AddOrder m{reinterpret_cast<const char*>(raw)};
  CHECK(m.type() == 'A');
  CHECK(m.locate() == 42);
  CHECK(m.tracking() == 7);
  CHECK(m.timestamp() == 0x0000'0102'0304ull);
  CHECK(m.order_ref() == 123456);
  CHECK(m.side() == 'S');
  CHECK(m.shares() == 300);
  CHECK(m.stock() == "AAPL    ");
  CHECK(m.price() == 1732148);  // $173.2148
}

TEST(stream_keeps_partial_message_and_skips_bad_ones) {
  std::vector<char> buf = generate(200, 5, 1);
  struct Count : NullHandler {
    int n = 0;
    void on_add(AddOrder) { ++n; }
    void on_delete(OrderDelete) { ++n; }
    void on_replace(OrderReplace) { ++n; }
    void on_cancel(OrderCancel) { ++n; }
    void on_executed(OrderExecuted) { ++n; }
    void on_executed_with_price(OrderExecutedWithPrice) { ++n; }
    void on_trade(Trade) { ++n; }
    void on_system_event(SystemEvent) { ++n; }
    void on_stock_directory(StockDirectory) { ++n; }
    void on_trading_action(StockTradingAction) { ++n; }
    void on_other(Header) { ++n; }
  } c;
  const std::size_t total = split(buf).size();
  const StreamResult whole = parse_stream(buf.data(), buf.size(), c);
  CHECK(whole.messages == total && whole.errors == 0 && whole.consumed == buf.size());
  // Cut in the middle of the last message: everything before it is consumed.
  Count c2;
  const StreamResult cut = parse_stream(buf.data(), buf.size() - 5, c2);
  CHECK(cut.messages == total - 1 && cut.consumed < buf.size() - 5);
  // Corrupt one type byte and one length: both are skipped, the rest parse.
  std::vector<char> bad = buf;
  bad[2] = '?';
  Count c3;
  const StreamResult r = parse_stream(bad.data(), bad.size(), c3);
  CHECK(r.errors == 1 && r.messages == total - 1);
}

TEST(file_parse_is_identical_across_chunk_boundaries) {
  const std::vector<char> buf = generate(50'000, 20, 2);
  const std::string path = "itch_test_tmp.itch";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  BookBuilder<> a, b;
  parse_stream(buf.data(), buf.size(), a);
  const StreamResult r = parse_file(path, b, 37);  // tiny chunks: most messages straddle a boundary
  CHECK(r.errors == 0 && r.messages == split(buf).size());
  CHECK(a.checksum() == b.checksum());
  std::remove(path.c_str());
}

// ---------------------------------------------------------------- order map

TEST(order_map_matches_unordered_map_through_growth) {
  OrderMap m(16);  // forces several doublings
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  std::mt19937_64 rng(5);
  for (int i = 0; i < 400'000; ++i) {
    const std::uint64_t k = 1 + rng() % 20'000;
    if (rng() % 3) {
      const bool fresh = !ref.count(k);
      CHECK(m.insert(OrderRec{k, static_cast<std::uint32_t>(i), 1, 0, 0}) == fresh);
      if (fresh) ref[k] = static_cast<std::uint32_t>(i);
    } else if (OrderRec* r = m.find(k)) {
      CHECK(ref.count(k) == 1);
      m.erase(r);
      ref.erase(k);
    } else {
      CHECK(ref.count(k) == 0);
    }
    if (i % 997 == 0) {
      for (auto& [kk, v] : ref) {
        const OrderRec* r = m.find(kk);
        CHECK(r && r->price == v);
      }
    }
  }
  CHECK(m.size() == ref.size());
}

// ---------------------------------------------------------------- book

namespace {
struct Enc {  // tiny encoder for hand-written scenarios
  std::vector<char> buf;
  char m[64];
  void push(std::size_t n) {
    const std::size_t off = buf.size();
    buf.resize(off + 2 + n);
    put16(buf.data() + off, static_cast<std::uint16_t>(n));
    std::memcpy(buf.data() + off + 2, m, n);
  }
  void hdr(char t, std::uint16_t loc) {
    std::memset(m, 0, sizeof m);
    m[0] = t;
    put16(m + 1, loc);
  }
  void add(std::uint64_t ref, char side, std::uint32_t sh, std::uint32_t px, std::uint16_t loc = 1) {
    hdr('A', loc); put64(m + 11, ref); m[19] = side; put32(m + 20, sh); std::memcpy(m + 24, "TEST    ", 8); put32(m + 32, px); push(36);
  }
  void exec(std::uint64_t ref, std::uint32_t sh) { hdr('E', 1); put64(m + 11, ref); put32(m + 19, sh); push(31); }
  void cancel(std::uint64_t ref, std::uint32_t sh) { hdr('X', 1); put64(m + 11, ref); put32(m + 19, sh); push(23); }
  void del(std::uint64_t ref) { hdr('D', 1); put64(m + 11, ref); push(19); }
  void replace(std::uint64_t o, std::uint64_t n, std::uint32_t sh, std::uint32_t px) {
    hdr('U', 1); put64(m + 11, o); put64(m + 19, n); put32(m + 27, sh); put32(m + 31, px); push(35);
  }
};
using Depth = std::vector<std::tuple<std::uint32_t, std::uint64_t, std::uint32_t>>;
template <class B>
Depth depth(const B& b, std::uint16_t loc, int side) {
  Depth d;
  b.for_each_level(loc, side, [&](std::uint32_t p, std::uint64_t s, std::uint32_t o) { d.emplace_back(p, s, o); });
  return d;
}
}  // namespace

TEST(book_applies_every_order_message) {
  std::vector<TopOfBook> tobs;
  auto rec = [&tobs](const TopOfBook& t) { tobs.push_back(t); };
  BookBuilder<decltype(rec)> b(64, rec);
  Enc e;
  e.add(1, 'B', 100, 1000000);  // bid 100 @ 100.0000
  e.add(2, 'B', 200, 1000000);  // same level
  e.add(3, 'B', 50, 999900);    // deeper
  e.add(4, 'S', 300, 1000100);  // ask
  e.exec(1, 40);                // 1 -> 60
  e.cancel(2, 200);             // 2 gone (full cancel)
  e.replace(3, 5, 70, 1000050); // 3 -> 5 at a better price: new best bid
  e.del(4);                     // asks empty
  e.add(6, 'B', 10, 990000);    // deep bid: top unchanged, no update
  parse_stream(e.buf.data(), e.buf.size(), b);
  CHECK((depth(b, 1, 0) == Depth{{1000050, 70, 1}, {1000000, 60, 1}, {990000, 10, 1}}));
  CHECK(depth(b, 1, 1).empty());
  CHECK(b.live_orders() == 3 && b.stats().unknown_ref == 0);
  // Top-of-book published only when it changed: add1, add2, add4, exec, cancel, replace, delete.
  CHECK(tobs.size() == 7);
  CHECK(tobs.back().bid_price == 1000050 && tobs.back().bid_shares == 70 && tobs.back().ask_price == 0);
}

TEST(unknown_references_are_counted_not_fatal) {
  BookBuilder<> b(64);
  Enc e;
  e.del(99);
  e.exec(98, 10);
  e.replace(97, 96, 1, 1);
  parse_stream(e.buf.data(), e.buf.size(), b);
  CHECK(b.stats().unknown_ref == 3 && b.live_orders() == 0);
}

namespace {
// Straightforward reference: std::map levels per instrument and side.
struct RefBook : NullHandler {
  struct O { std::uint16_t loc; int side; std::uint32_t px, sh; };
  std::unordered_map<std::uint64_t, O> orders;
  std::map<std::pair<std::uint16_t, int>, std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>> lv;
  void add_lv(const O& o) { auto& l = lv[{o.loc, o.side}][o.px]; l.first += o.sh; ++l.second; }
  void sub_lv(const O& o, std::uint32_t q, bool gone) {
    auto& side = lv[{o.loc, o.side}];
    auto& l = side[o.px];
    l.first -= q;
    if (gone && --l.second == 0) side.erase(o.px);
  }
  void reduce(std::uint64_t ref, std::uint32_t q) {
    auto it = orders.find(ref);
    if (it == orders.end()) return;
    q = std::min(q, it->second.sh);
    const bool gone = q == it->second.sh;
    sub_lv(it->second, q, gone);
    if (gone) orders.erase(it); else it->second.sh -= q;
  }
  void on_add(AddOrder m) { O o{m.locate(), m.side() == 'B' ? 0 : 1, m.price(), m.shares()}; orders[m.order_ref()] = o; add_lv(o); }
  void on_executed(OrderExecuted m) { reduce(m.order_ref(), m.executed_shares()); }
  void on_executed_with_price(OrderExecutedWithPrice m) { reduce(m.order_ref(), m.executed_shares()); }
  void on_cancel(OrderCancel m) { reduce(m.order_ref(), m.cancelled_shares()); }
  void on_delete(OrderDelete m) { auto it = orders.find(m.order_ref()); if (it != orders.end()) reduce(m.order_ref(), it->second.sh); }
  void on_replace(OrderReplace m) {
    auto it = orders.find(m.original_ref());
    if (it == orders.end()) return;
    O o = it->second;
    reduce(m.original_ref(), o.sh);
    o.px = m.price(); o.sh = m.shares();
    orders[m.new_ref()] = o;
    add_lv(o);
  }
  Depth depth(std::uint16_t loc, int side) {
    Depth d;
    auto& m = lv[{loc, side}];
    if (side == 0) for (auto it = m.rbegin(); it != m.rend(); ++it) d.emplace_back(it->first, it->second.first, it->second.second);
    else for (auto& [p, l] : m) d.emplace_back(p, l.first, l.second);
    return d;
  }
};
}  // namespace

TEST(differential_vs_std_map_reference) {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    const std::uint32_t stocks = 40;
    const std::vector<char> buf = generate(300'000, stocks, seed);
    auto fast = std::make_unique<BookBuilder<>>(1 << 12);  // small start: exercises map growth
    RefBook ref;
    const auto msgs = split(buf);
    std::size_t i = 0;
    bool ok = true;
    for (const auto& m : msgs) {
      dispatch(m.data(), m.size(), *fast);
      dispatch(m.data(), m.size(), ref);
      if (++i % 25'000 == 0 || i == msgs.size()) {
        for (std::uint16_t loc = 1; loc <= stocks && ok; ++loc)
          for (int s = 0; s < 2 && ok; ++s)
            if (depth(*fast, loc, s) != ref.depth(loc, s)) {
              ok = false;
              std::printf("    depth diverged: seed %llu msg %zu locate %u side %d\n",
                          static_cast<unsigned long long>(seed), i, loc, s);
            }
      }
    }
    CHECK(ok);
    // Like a real exchange feed, the generated books never cross.
    bool uncrossed = true;
    for (std::uint16_t loc = 1; loc <= stocks; ++loc) {
      const TopOfBook& t = fast->top(loc);
      if (t.bid_price && t.ask_price && t.bid_price >= t.ask_price) uncrossed = false;
    }
    CHECK(uncrossed);
    CHECK(fast->live_orders() == ref.orders.size());
    CHECK(fast->stats().unknown_ref == 0);
    std::printf("    seed %llu: %zu messages, %zu live orders, identical depth\n",
                static_cast<unsigned long long>(seed), msgs.size(), fast->live_orders());
  }
}

// ---------------------------------------------------------------- MoldUDP64

namespace {
const char kSess[10] = {'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};

std::vector<std::string> packetize(const std::vector<std::string>& msgs, std::uint64_t from, std::uint64_t upto,
                                   std::size_t max_payload) {
  std::vector<std::string> pkts;
  mold::PacketBuilder pb(kSess, max_payload);
  std::uint64_t seq = from;
  while (seq < upto) {
    pb.start(seq);
    while (seq < upto && pb.fits(msgs[seq - 1].size())) {
      pb.add(msgs[seq - 1].data(), msgs[seq - 1].size());
      ++seq;
    }
    const auto& p = pb.finish();
    pkts.emplace_back(p.data(), p.size());
  }
  return pkts;
}
}  // namespace

TEST(mold_session_recovers_from_loss_duplicates_and_reordering) {
  const auto msgs = split(generate(60'000, 30, 9));
  const std::uint64_t total = msgs.size();
  auto live = packetize(msgs, 1, total + 1, 600);
  std::mt19937_64 rng(11);

  mold::MoldSession s;
  std::vector<std::uint64_t> delivered_seq;
  std::uint64_t hash_rx = 1469598103934665603ull, hash_tx = hash_rx;
  for (const auto& m : msgs) for (char c : m) hash_tx = (hash_tx ^ static_cast<unsigned char>(c)) * 1099511628211ull;

  std::vector<std::pair<std::uint64_t, std::uint16_t>> requests;
  auto on_msg = [&](std::uint64_t seq, const char* p, std::size_t n) {
    delivered_seq.push_back(seq);
    for (std::size_t i = 0; i < n; ++i) hash_rx = (hash_rx ^ static_cast<unsigned char>(p[i])) * 1099511628211ull;
  };
  auto on_gap = [&](std::uint64_t first, std::uint16_t count) { requests.emplace_back(first, count); };

  // Retransmission server that itself loses 20% of its answers.
  auto serve = [&] {
    auto reqs = std::move(requests);
    requests.clear();
    for (auto [first, count] : reqs) {
      const std::uint64_t upto = std::min<std::uint64_t>(first + count, total + 1);
      for (const auto& p : packetize(msgs, first, upto, 600))
        if (rng() % 5) s.on_packet(p.data(), p.size(), on_msg, on_gap);
    }
  };

  std::vector<std::string> pending;  // a few packets held back to arrive late (reordering)
  for (std::size_t i = 0; i < live.size(); ++i) {
    const auto r = rng() % 100;
    if (r < 3) continue;                                    // lost
    if (r < 6) { pending.push_back(live[i]); continue; }    // delayed
    s.on_packet(live[i].data(), live[i].size(), on_msg, on_gap);
    if (r < 9) s.on_packet(live[i].data(), live[i].size(), on_msg, on_gap);  // duplicated
    if (!pending.empty() && rng() % 4 == 0) {
      s.on_packet(pending.back().data(), pending.back().size(), on_msg, on_gap);
      pending.pop_back();
    }
    if (rng() % 8 == 0) serve();
  }
  char eos[mold::kHeader];
  mold::write_header(eos, kSess, total + 1, mold::kEndOfSession);
  s.on_packet(eos, sizeof eos, on_msg, on_gap);
  for (int round = 0; round < 200 && !s.finished(); ++round) {
    serve();
    s.on_timeout(on_gap);
  }
  CHECK(s.finished());
  CHECK(delivered_seq.size() == total);
  bool in_order = true;
  for (std::size_t i = 0; i < delivered_seq.size(); ++i) in_order &= delivered_seq[i] == i + 1;
  CHECK(in_order);
  CHECK(hash_rx == hash_tx);
  std::printf("    %llu messages, %llu gaps, %llu requests, %llu recovered, %llu duplicates ignored\n",
              static_cast<unsigned long long>(total), static_cast<unsigned long long>(s.stats().gaps),
              static_cast<unsigned long long>(s.stats().requests), static_cast<unsigned long long>(s.stats().recovered),
              static_cast<unsigned long long>(s.stats().duplicates));
}

TEST(mold_heartbeat_reveals_a_trailing_gap) {
  const auto msgs = split(generate(100, 3, 4));
  const auto pkts = packetize(msgs, 1, msgs.size() + 1, 300);
  mold::MoldSession s;
  std::vector<std::pair<std::uint64_t, std::uint16_t>> req;
  auto on_msg = [](std::uint64_t, const char*, std::size_t) {};
  auto on_gap = [&](std::uint64_t f, std::uint16_t c) { req.emplace_back(f, c); };
  for (std::size_t i = 0; i + 1 < pkts.size(); ++i) s.on_packet(pkts[i].data(), pkts[i].size(), on_msg, on_gap);
  CHECK(req.empty());
  const std::uint64_t got = s.expected();
  char hb[mold::kHeader];
  mold::write_header(hb, kSess, msgs.size() + 1, 0);  // heartbeat: next seq is past what we have
  s.on_packet(hb, sizeof hb, on_msg, on_gap);
  CHECK(req.size() == 1 && req[0].first == got && req[0].first + req[0].second == msgs.size() + 1);
}

int main() {
  int failed = 0;
  for (const auto& t : registry()) {
    const int before = g_failures;
    std::printf("[ RUN  ] %s\n", t.name);
    t.fn();
    const bool ok = g_failures == before;
    if (!ok) ++failed;
    std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
  }
  std::printf("\n%zu tests, %d failed\n", registry().size(), failed);
  return failed == 0 ? 0 : 1;
}
