#pragma once

// Builds a market-by-price book for every instrument from ITCH order messages.
//
// ITCH is market-by-order: the exchange tells you about every individual
// order (add, execute, cancel, delete, replace) and you rebuild the book.
// BookBuilder is a parser handler that does exactly that and, whenever the top
// of an instrument's book changes, hands a TopOfBook update to the listener.
//
//   - Live orders: OrderMap keyed by order reference (inline 24-byte records).
//   - Levels: per instrument and side, a vector sorted worst -> best, so the
//     best price is at the back and most updates touch the last few entries.
//     A level that empties below the touch stays in place (orders == 0) and is
//     reused when that price comes back, so the vector rarely shifts; empty
//     levels are only popped off the back, which keeps back() the real best.
//   - Instruments are indexed directly by the 16-bit stock locate code.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "itch/messages.hpp"
#include "itch/order_map.hpp"

namespace itch {

struct Level {
  std::uint32_t price;
  std::uint32_t orders;
  std::uint64_t shares;
};

struct TopOfBook {  // 32 bytes, two per cache line on the SPSC queue
  std::uint64_t timestamp;
  std::uint32_t bid_price, bid_shares;
  std::uint32_t ask_price, ask_shares;
  std::uint16_t locate;
  std::uint8_t pad[6];
};
static_assert(sizeof(TopOfBook) == 32);

struct BookStats {
  std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
  std::uint64_t trades = 0, other = 0, unknown_ref = 0, tob_updates = 0;
  std::uint64_t executed_shares = 0;
};

struct NoTopOfBook {
  void operator()(const TopOfBook&) const {}
};

template <class OnTopOfBook = NoTopOfBook>
class BookBuilder : public NullHandler {
 public:
  // The order table starts small and doubles as needed: a table sized far
  // beyond the live order count turns every lookup into a cache miss.
  explicit BookBuilder(std::size_t expected_orders = 1 << 16, OnTopOfBook on_tob = {})
      : orders_(expected_orders), books_(65536), on_tob_(std::move(on_tob)) {}

  // ---- parser callbacks
  void on_stock_directory(StockDirectory m) {
    Book& b = books_[m.locate()];
    std::memcpy(b.symbol, m.p + 11, 8);
    b.known = true;
  }
  void on_add(AddOrder m) {
    ++stats_.adds;
    const std::uint8_t side = m.side() == 'B' ? 0 : 1;
    const OrderRec r{m.order_ref(), m.price(), m.shares(), m.locate(), side};
    if (!orders_.insert(r)) return;
    add_to_level(r.locate, side, r.price, r.shares);
    publish(r.locate, m.timestamp());
  }
  void on_executed(OrderExecuted m) {
    ++stats_.executions;
    stats_.executed_shares += m.executed_shares();
    reduce(m.order_ref(), m.executed_shares(), m.timestamp());
  }
  void on_executed_with_price(OrderExecutedWithPrice m) {
    ++stats_.executions;
    stats_.executed_shares += m.executed_shares();
    reduce(m.order_ref(), m.executed_shares(), m.timestamp());
  }
  void on_cancel(OrderCancel m) {
    ++stats_.cancels;
    reduce(m.order_ref(), m.cancelled_shares(), m.timestamp());
  }
  void on_delete(OrderDelete m) {
    ++stats_.deletes;
    OrderRec* o = orders_.find(m.order_ref());
    if (!o) { ++stats_.unknown_ref; return; }
    const std::uint16_t loc = o->locate;
    remove_from_level(loc, o->side, o->price, o->shares);
    orders_.erase(o);
    publish(loc, m.timestamp());
  }
  // Replace = delete the original and add a new order (new ref, new price and
  // size) on the same side of the same instrument. It loses time priority.
  void on_replace(OrderReplace m) {
    ++stats_.replaces;
    OrderRec* o = orders_.find(m.original_ref());
    if (!o) { ++stats_.unknown_ref; return; }
    const OrderRec old = *o;
    remove_from_level(old.locate, old.side, old.price, old.shares);
    orders_.erase(o);
    const OrderRec r{m.new_ref(), m.price(), m.shares(), old.locate, old.side};
    if (orders_.insert(r)) add_to_level(r.locate, r.side, r.price, r.shares);
    publish(old.locate, m.timestamp());
  }
  void on_trade(Trade) { ++stats_.trades; }
  void on_cross_trade(CrossTrade) { ++stats_.trades; }
  void on_system_event(SystemEvent) { ++stats_.other; }
  void on_trading_action(StockTradingAction) { ++stats_.other; }
  void on_broken_trade(BrokenTrade) { ++stats_.other; }
  void on_other(Header) { ++stats_.other; }

  // Called by the parser on a message a few positions ahead of the one being
  // applied: start loading the order's hash slot so it is in cache on arrival.
  void prefetch(const char* m) const {
    switch (m[0]) {
      case 'E': case 'C': case 'X': case 'D': case 'U': orders_.prefetch(be64(m + 11)); break;
      default: break;
    }
  }

  // ---- queries
  const BookStats& stats() const { return stats_; }
  std::size_t live_orders() const { return orders_.size(); }
  std::string symbol(std::uint16_t loc) const {
    std::string s(books_[loc].symbol, 8);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
  }
  bool known(std::uint16_t loc) const { return books_[loc].known; }
  // Visits levels best first: f(price, shares, orders).
  template <class F>
  void for_each_level(std::uint16_t loc, int side, F&& f) const {
    const auto& v = books_[loc].side[side];
    for (auto it = v.rbegin(); it != v.rend(); ++it)
      if (it->orders) f(it->price, it->shares, it->orders);
  }
  std::size_t level_count(std::uint16_t loc, int side) const {
    std::size_t n = 0;
    for (const Level& l : books_[loc].side[side]) n += l.orders != 0;
    return n;
  }
  const TopOfBook& top(std::uint16_t loc) const { return books_[loc].last; }

  // Order-independent fingerprint of every level of every book, for comparing
  // a book rebuilt from a lossy network feed against one built from the file.
  std::uint64_t checksum() const {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (std::size_t loc = 0; loc < books_.size(); ++loc)
      for (int s = 0; s < 2; ++s)
        for (const Level& l : books_[loc].side[s]) {
          if (!l.orders) continue;
          mix(loc); mix(static_cast<std::uint64_t>(s)); mix(l.price); mix(l.shares); mix(l.orders);
        }
    mix(orders_.size());
    return h;
  }

 private:
  struct Book {
    std::vector<Level> side[2];  // [0] bids ascending, [1] asks descending: best at back
    TopOfBook last{};
    char symbol[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bool known = false;
  };

  // Position of price in a worst->best sorted side.
  static std::vector<Level>::iterator seek(std::vector<Level>& v, int side, std::uint32_t price) {
    // Fast path: most activity is at or near the touch, i.e. the back.
    const std::size_t n = v.size();
    const std::size_t scan = n < 8 ? n : 8;
    for (std::size_t k = 1; k <= scan; ++k) {
      Level& l = v[n - k];
      if (l.price == price) return v.begin() + static_cast<std::ptrdiff_t>(n - k);
      const bool worse = side == 0 ? l.price < price : l.price > price;
      if (worse) return v.begin() + static_cast<std::ptrdiff_t>(n - k + 1);
    }
    if (side == 0)
      return std::lower_bound(v.begin(), v.end() - static_cast<std::ptrdiff_t>(scan), price,
                              [](const Level& l, std::uint32_t p) { return l.price < p; });
    return std::lower_bound(v.begin(), v.end() - static_cast<std::ptrdiff_t>(scan), price,
                            [](const Level& l, std::uint32_t p) { return l.price > p; });
  }

  void add_to_level(std::uint16_t loc, int side, std::uint32_t price, std::uint32_t shares) {
    auto& v = books_[loc].side[side];
    auto it = seek(v, side, price);
    if (it != v.end() && it->price == price) {
      it->shares += shares;
      ++it->orders;
    } else {
      v.insert(it, Level{price, 1, shares});
    }
  }

  void remove_from_level(std::uint16_t loc, int side, std::uint32_t price, std::uint32_t shares) {
    auto& v = books_[loc].side[side];
    auto it = seek(v, side, price);
    if (it == v.end() || it->price != price) return;  // cannot happen for a consistent feed
    it->shares -= shares;
    if (--it->orders == 0) trim(v);
  }

  // Pops empty levels off the back so back() is always the best live price.
  static void trim(std::vector<Level>& v) {
    while (!v.empty() && v.back().orders == 0) v.pop_back();
  }

  void reduce(std::uint64_t ref, std::uint32_t shares, std::uint64_t ts) {
    OrderRec* o = orders_.find(ref);
    if (!o) { ++stats_.unknown_ref; return; }
    const std::uint32_t q = shares < o->shares ? shares : o->shares;
    const std::uint16_t loc = o->locate;
    auto& v = books_[loc].side[o->side];
    auto it = seek(v, o->side, o->price);
    if (it != v.end() && it->price == o->price) {
      it->shares -= q;
      if (q == o->shares && --it->orders == 0) trim(v);
    }
    if (q == o->shares) orders_.erase(o);
    else o->shares -= q;
    publish(loc, ts);
  }

  void publish(std::uint16_t loc, std::uint64_t ts) {
    Book& b = books_[loc];
    TopOfBook t{};
    t.locate = loc;
    t.timestamp = ts;
    if (!b.side[0].empty()) { t.bid_price = b.side[0].back().price; t.bid_shares = sat(b.side[0].back().shares); }
    if (!b.side[1].empty()) { t.ask_price = b.side[1].back().price; t.ask_shares = sat(b.side[1].back().shares); }
    if (t.bid_price != b.last.bid_price || t.bid_shares != b.last.bid_shares || t.ask_price != b.last.ask_price ||
        t.ask_shares != b.last.ask_shares) {
      b.last = t;
      ++stats_.tob_updates;
      on_tob_(t);
    }
  }
  static std::uint32_t sat(std::uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); }

  OrderMap orders_;
  std::vector<Book> books_;
  OnTopOfBook on_tob_;
  BookStats stats_;
};

}  // namespace itch
