#pragma once

// Zero-copy ITCH 5.0 parser.
//
// Both Nasdaq's BinaryFILE format (the public sample files) and MoldUDP64
// packet payloads frame messages the same way: a 2-byte big-endian length,
// then the message. parse_stream() walks that framing in place and calls the
// handler with a view into the buffer; nothing is copied.

#include <cstddef>
#include <cstdint>

#include "itch/messages.hpp"

namespace itch {

enum class ParseStatus : std::uint8_t { Ok, UnknownType, BadLength };

// Dispatches one message (without its length prefix) to the handler.
template <class H>
inline ParseStatus dispatch(const char* m, std::size_t len, H& h) {
  const std::size_t want = message_size(m[0]);
  if (want == 0) return ParseStatus::UnknownType;
  if (len != want) return ParseStatus::BadLength;
  switch (m[0]) {
    case 'A':
    case 'F': h.on_add(AddOrder{m}); break;
    case 'E': h.on_executed(OrderExecuted{m}); break;
    case 'C': h.on_executed_with_price(OrderExecutedWithPrice{m}); break;
    case 'X': h.on_cancel(OrderCancel{m}); break;
    case 'D': h.on_delete(OrderDelete{m}); break;
    case 'U': h.on_replace(OrderReplace{m}); break;
    case 'P': h.on_trade(Trade{m}); break;
    case 'Q': h.on_cross_trade(CrossTrade{m}); break;
    case 'B': h.on_broken_trade(BrokenTrade{m}); break;
    case 'S': h.on_system_event(SystemEvent{m}); break;
    case 'R': h.on_stock_directory(StockDirectory{m}); break;
    case 'H': h.on_trading_action(StockTradingAction{m}); break;
    default: h.on_other(Header{m}); break;
  }
  return ParseStatus::Ok;
}

struct StreamResult {
  std::size_t consumed = 0;   // bytes of complete messages processed
  std::uint64_t messages = 0;
  std::uint64_t errors = 0;   // unknown types or length mismatches (skipped)
};

// Parses as many complete length-prefixed messages as buf holds. A message cut
// off at the end of the buffer is left unconsumed so the caller can carry it
// over to the next read.
// Handlers that keep large lookup tables (the book builder) can expose
// prefetch(msg); the parser then calls it kLookahead messages early so the
// cache miss overlaps with the work on the current message.
template <class H>
concept Prefetching = requires(H& h, const char* m) { h.prefetch(m); };
inline constexpr int kLookahead = 8;

template <class H>
inline StreamResult parse_stream(const char* buf, std::size_t len, H& h) {
  StreamResult r;
  std::size_t off = 0;
  [[maybe_unused]] std::size_t ahead = 0;
  if constexpr (Prefetching<H>) {
    for (int i = 0; i < kLookahead && ahead + 2 <= len; ++i) {
      const std::size_t n = be16(buf + ahead);
      if (ahead + 2 + n > len) break;
      if (n) h.prefetch(buf + ahead + 2);
      ahead += 2 + n;
    }
  }
  while (off + 2 <= len) {
    if constexpr (Prefetching<H>) {
      if (ahead + 2 <= len) {
        const std::size_t n = be16(buf + ahead);
        if (ahead + 2 + n <= len) {
          if (n) h.prefetch(buf + ahead + 2);
          ahead += 2 + n;
        }
      }
    }
    const std::size_t mlen = be16(buf + off);
    if (off + 2 + mlen > len) break;
    if (mlen == 0 || dispatch(buf + off + 2, mlen, h) != ParseStatus::Ok) ++r.errors;
    else ++r.messages;
    off += 2 + mlen;
  }
  r.consumed = off;
  return r;
}

}  // namespace itch
