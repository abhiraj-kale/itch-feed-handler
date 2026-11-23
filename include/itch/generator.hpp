#pragma once

// Synthetic ITCH 5.0 session generator.
//
// Produces a spec-conformant message stream (system events, stock directory,
// trading actions, then order flow) with a message mix close to a real Nasdaq
// day: mostly adds and deletes, then replaces, executions, cancels, hidden
// trades, and a few auction / imbalance messages. Order references are always
// consistent (every execute / cancel / delete / replace targets a live order),
// so a correct book builder ends with zero unknown references. Books stay
// uncrossed like a real exchange's: bids rest below the mid, asks above it, and
// when the mid moves through resting orders those orders are executed.
//
// Used by the tests, the benchmarks when no real sample file is at hand, and
// the browser demo.

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <unordered_map>
#include <string>
#include <vector>

#include "itch/endian.hpp"
#include "itch/messages.hpp"

namespace itch {

struct GenConfig {
  std::uint32_t stocks = 100;
  std::uint64_t messages = 1'000'000;  // order-flow messages after the preamble
  std::uint64_t seed = 1;
};

class Generator {
 public:
  explicit Generator(const GenConfig& cfg) : cfg_(cfg), rng_(cfg.seed) {
    stocks_.resize(cfg.stocks);
    std::uniform_int_distribution<int> px(1000, 50000);  // $10 .. $500 in cents
    for (std::uint32_t i = 0; i < cfg.stocks; ++i) {
      stocks_[i].mid = static_cast<std::uint32_t>(px(rng_)) * 100;  // 4 implied decimals
      stocks_[i].symbol = make_symbol(i);
    }
  }

  // sink(const char* msg, std::size_t len) is called once per message.
  template <class Sink>
  void run(Sink&& sink) {
    ts_ = 4ull * 3600 * 1'000'000'000;  // 04:00, start of system hours
    emit_system('O', sink);
    emit_system('S', sink);
    for (std::uint32_t i = 0; i < cfg_.stocks; ++i) emit_directory(i, sink);
    for (std::uint32_t i = 0; i < cfg_.stocks; ++i) emit_trading_action(i, sink);
    ts_ = 9ull * 3600 * 1'000'000'000 + 30ull * 60 * 1'000'000'000;  // 09:30
    emit_system('Q', sink);
    std::uniform_int_distribution<int> pct(0, 9999);
    for (std::uint64_t n = 0; n < cfg_.messages; ++n) {
      ts_ += 1 + (rng_() % 20000);
      const std::uint32_t s = pick_stock();
      Stock& st = stocks_[s];
      if ((rng_() & 255) == 0) {
        st.mid = nudge(st.mid);
        sweep(s, sink);
      }
      const int r = pct(rng_);
      if (st.live.empty() || r < 4200) emit_add(s, false, sink);
      else if (r < 4350) emit_add(s, true, sink);
      else if (r < 8350) emit_delete(s, sink);
      else if (r < 9250) emit_replace(s, sink);
      else if (r < 9450) emit_cancel(s, sink);
      else if (r < 9780) emit_executed(s, false, sink);
      else if (r < 9810) emit_executed(s, true, sink);
      else if (r < 9970) emit_trade(s, sink);
      else emit_imbalance(s, sink);
    }
    emit_system('M', sink);
    emit_system('E', sink);
    emit_system('C', sink);
  }

  const std::string& symbol(std::uint32_t i) const { return stocks_[i].symbol; }
  std::uint64_t live_orders() const {
    std::uint64_t n = 0;
    for (const auto& s : stocks_) n += s.live.size();
    return n;
  }

 private:
  struct GenOrder {
    std::uint64_t ref;
    std::uint32_t price;
    std::uint32_t shares;
    std::uint8_t side;
  };
  struct Stock {
    std::uint32_t mid = 0;
    std::string symbol;
    std::vector<GenOrder> live;                                   // for random picks
    std::unordered_map<std::uint64_t, std::uint32_t> pos;         // ref -> index in live
    std::map<std::uint32_t, std::vector<std::uint64_t>> lv[2];    // price -> refs, per side
  };

  // Unique 3-letter tickers (4 past 17,576 stocks), scrambled so neighbors differ.
  static std::string make_symbol(std::uint32_t i) {
    if (i >= 17576) return "Z" + make_symbol(i - 17576);
    const std::uint32_t v = static_cast<std::uint32_t>((static_cast<std::uint64_t>(i) * 7919 + 4111) % 17576);
    return {static_cast<char>('A' + v / 676), static_cast<char>('A' + (v / 26) % 26), static_cast<char>('A' + v % 26)};
  }

  // Skewed toward low indices, like real volume concentration in a few names.
  std::uint32_t pick_stock() {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double x = u(rng_);
    return static_cast<std::uint32_t>(x * x * x * cfg_.stocks) % cfg_.stocks;
  }
  std::uint32_t nudge(std::uint32_t mid) {
    const std::uint32_t step = 100;  // one cent
    if (rng_() & 1) return mid + step;
    return mid > 20 * step ? mid - step : mid;
  }
  std::uint32_t shares() {
    const std::uint32_t r = static_cast<std::uint32_t>(rng_() % 100);
    if (r < 12) return 1 + static_cast<std::uint32_t>(rng_() % 99);  // odd lot
    return 100 * (1 + static_cast<std::uint32_t>(rng_() % (r < 90 ? 5 : 30)));
  }
  std::size_t pick_live(const Stock& st) {
    // Favor recent orders: real deletes / replaces hit fresh quotes most often.
    const std::size_t n = st.live.size();
    if ((rng_() & 3) != 0 && n > 16) return n - 1 - static_cast<std::size_t>(rng_() % 16);
    return static_cast<std::size_t>(rng_() % n);
  }
  void add_live(Stock& st, const GenOrder& o) {
    st.pos[o.ref] = static_cast<std::uint32_t>(st.live.size());
    st.live.push_back(o);
    st.lv[o.side][o.price].push_back(o.ref);
  }
  void unindex(Stock& st, const GenOrder& o) {
    auto it = st.lv[o.side].find(o.price);
    auto& refs = it->second;
    for (auto& r : refs)
      if (r == o.ref) { r = refs.back(); refs.pop_back(); break; }
    if (refs.empty()) st.lv[o.side].erase(it);
    st.pos.erase(o.ref);
  }
  void drop_live(Stock& st, std::size_t i) {
    unindex(st, st.live[i]);
    if (i + 1 != st.live.size()) {
      st.live[i] = st.live.back();
      st.pos[st.live[i].ref] = static_cast<std::uint32_t>(i);
    }
    st.live.pop_back();
  }
  // After the mid moves, execute resting orders it moved through.
  template <class Sink>
  void sweep(std::uint32_t s, Sink& sink) {
    Stock& st = stocks_[s];
    while (!st.lv[1].empty() && st.lv[1].begin()->first <= st.mid) {
      const std::vector<std::uint64_t> refs = st.lv[1].begin()->second;
      for (std::uint64_t r : refs) emit_full_exec(s, st.pos[r], sink);
    }
    while (!st.lv[0].empty() && st.lv[0].rbegin()->first >= st.mid) {
      const std::vector<std::uint64_t> refs = st.lv[0].rbegin()->second;
      for (std::uint64_t r : refs) emit_full_exec(s, st.pos[r], sink);
    }
  }
  template <class Sink>
  void emit_full_exec(std::uint32_t s, std::size_t i, Sink& sink) {
    Stock& st = stocks_[s];
    char* m = header('E', loc(s));
    put64(m + 11, st.live[i].ref);
    put32(m + 19, st.live[i].shares);
    put64(m + 23, next_match_++);
    drop_live(st, i);
    sink(m, message_size('E'));
  }

  // --- message encoding
  char* header(char type, std::uint16_t locate) {
    std::memset(buf_, 0, sizeof buf_);
    buf_[0] = type;
    put16(buf_ + 1, locate);
    put16(buf_ + 3, static_cast<std::uint16_t>(rng_() & 7));
    put48(buf_ + 5, ts_);
    return buf_;
  }
  void stock8(char* p, const std::string& s) {
    std::memset(p, ' ', 8);
    std::memcpy(p, s.data(), s.size() < 8 ? s.size() : 8);
  }
  static std::uint16_t loc(std::uint32_t s) { return static_cast<std::uint16_t>(s + 1); }

  template <class Sink>
  void emit_system(char code, Sink& sink) {
    char* m = header('S', 0);
    m[11] = code;
    sink(m, message_size('S'));
  }
  template <class Sink>
  void emit_directory(std::uint32_t s, Sink& sink) {
    char* m = header('R', loc(s));
    stock8(m + 11, stocks_[s].symbol);
    m[19] = 'Q';  // Nasdaq Global Select
    m[20] = 'N';  // normal
    put32(m + 21, 100);
    m[25] = 'N';
    m[26] = 'C';
    m[27] = 'Z';
    m[28] = ' ';
    m[29] = 'P';
    m[30] = 'N';
    m[31] = ' ';
    m[32] = '1';
    m[33] = 'N';
    put32(m + 34, 0);
    m[38] = 'N';
    sink(m, message_size('R'));
  }
  template <class Sink>
  void emit_trading_action(std::uint32_t s, Sink& sink) {
    char* m = header('H', loc(s));
    stock8(m + 11, stocks_[s].symbol);
    m[19] = 'T';
    std::memset(m + 21, ' ', 4);
    sink(m, message_size('H'));
  }
  template <class Sink>
  void emit_add(std::uint32_t s, bool mpid, Sink& sink) {
    Stock& st = stocks_[s];
    const std::uint8_t side = rng_() & 1;
    std::geometric_distribution<int> off(0.3);
    const std::uint32_t ticks = 1 + static_cast<std::uint32_t>(off(rng_));
    std::uint32_t price = side == 0 ? st.mid - 100 * ticks : st.mid + 100 * ticks;
    if (side == 0 && st.mid <= 100 * ticks) price = 100;
    const GenOrder o{next_ref_++, price, shares(), side};
    char* m = header(mpid ? 'F' : 'A', loc(s));
    put64(m + 11, o.ref);
    m[19] = side == 0 ? 'B' : 'S';
    put32(m + 20, o.shares);
    stock8(m + 24, st.symbol);
    put32(m + 32, o.price);
    if (mpid) std::memcpy(m + 36, "GSCO", 4);
    add_live(st, o);
    sink(m, message_size(m[0]));
  }
  template <class Sink>
  void emit_delete(std::uint32_t s, Sink& sink) {
    Stock& st = stocks_[s];
    const std::size_t i = pick_live(st);
    char* m = header('D', loc(s));
    put64(m + 11, st.live[i].ref);
    drop_live(st, i);
    sink(m, message_size('D'));
  }
  template <class Sink>
  void emit_replace(std::uint32_t s, Sink& sink) {
    Stock& st = stocks_[s];
    const std::size_t i = pick_live(st);
    const GenOrder old = st.live[i];
    const std::int64_t move = static_cast<std::int64_t>(rng_() % 3) - 1;
    std::int64_t np = static_cast<std::int64_t>(old.price) + move * 100;
    // Stay on the right side of the mid so the book never crosses.
    if (old.side == 0 && np > static_cast<std::int64_t>(st.mid) - 100) np = static_cast<std::int64_t>(st.mid) - 100;
    if (old.side == 1 && np < static_cast<std::int64_t>(st.mid) + 100) np = static_cast<std::int64_t>(st.mid) + 100;
    if (np < 100) np = 100;
    const GenOrder nu{next_ref_++, static_cast<std::uint32_t>(np), shares(), old.side};
    drop_live(st, i);
    add_live(st, nu);
    char* m = header('U', loc(s));
    put64(m + 11, old.ref);
    put64(m + 19, nu.ref);
    put32(m + 27, nu.shares);
    put32(m + 31, nu.price);
    sink(m, message_size('U'));
  }
  template <class Sink>
  void emit_cancel(std::uint32_t s, Sink& sink) {
    Stock& st = stocks_[s];
    const std::size_t i = pick_live(st);
    GenOrder& o = st.live[i];
    if (o.shares < 2) { emit_delete(s, sink); return; }
    const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng_() % (o.shares - 1));
    char* m = header('X', loc(s));
    put64(m + 11, o.ref);
    put32(m + 19, q);
    o.shares -= q;
    sink(m, message_size('X'));
  }
  template <class Sink>
  void emit_executed(std::uint32_t s, bool with_price, Sink& sink) {
    Stock& st = stocks_[s];
    const std::size_t i = pick_live(st);
    GenOrder& o = st.live[i];
    const std::uint32_t q = (rng_() & 1) ? o.shares : 1 + static_cast<std::uint32_t>(rng_() % o.shares);
    char* m = header(with_price ? 'C' : 'E', loc(s));
    put64(m + 11, o.ref);
    put32(m + 19, q);
    put64(m + 23, next_match_++);
    if (with_price) {
      m[31] = 'Y';
      put32(m + 32, o.price);
    }
    o.shares -= q;
    if (o.shares == 0) drop_live(st, i);
    sink(m, message_size(m[0]));
  }
  template <class Sink>
  void emit_trade(std::uint32_t s, Sink& sink) {
    char* m = header('P', loc(s));
    put64(m + 11, 0);  // hidden orders carry no reference
    m[19] = (rng_() & 1) ? 'B' : 'S';
    put32(m + 20, shares());
    stock8(m + 24, stocks_[s].symbol);
    put32(m + 32, stocks_[s].mid);
    put64(m + 36, next_match_++);
    sink(m, message_size('P'));
  }
  template <class Sink>
  void emit_imbalance(std::uint32_t s, Sink& sink) {
    char* m = header('I', loc(s));
    put64(m + 11, 100 * (rng_() % 500));
    put64(m + 19, 100 * (rng_() % 50));
    m[27] = (rng_() & 1) ? 'B' : 'S';
    stock8(m + 28, stocks_[s].symbol);
    put32(m + 36, stocks_[s].mid);
    put32(m + 40, stocks_[s].mid);
    put32(m + 44, stocks_[s].mid);
    m[48] = 'C';
    m[49] = ' ';
    sink(m, message_size('I'));
  }

  GenConfig cfg_;
  std::mt19937_64 rng_;
  std::vector<Stock> stocks_;
  std::uint64_t ts_ = 0;
  std::uint64_t next_ref_ = 1;
  std::uint64_t next_match_ = 1;
  char buf_[64];
};

}  // namespace itch
