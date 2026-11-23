#pragma once

// Nasdaq TotalView-ITCH 5.0 message layouts.
//
// Every message is a fixed-size, big-endian record that starts with the same
// 11-byte header: type(1) stock_locate(2) tracking_number(2) timestamp(6).
// The views below hold a pointer into the receive buffer and decode a field
// only when it is asked for, so nothing is copied or byte-swapped up front.

#include <cstdint>
#include <string_view>

#include "itch/endian.hpp"

namespace itch {

// Wire size of every message type in the spec, indexed by type byte (0 = unknown).
struct SizeTable {
  std::uint8_t size[256]{};
  constexpr SizeTable() {
    size['S'] = 12; size['R'] = 39; size['H'] = 25; size['Y'] = 20; size['L'] = 26;
    size['V'] = 35; size['W'] = 12; size['K'] = 28; size['J'] = 35; size['h'] = 21;
    size['A'] = 36; size['F'] = 40; size['E'] = 31; size['C'] = 36; size['X'] = 23;
    size['D'] = 19; size['U'] = 35; size['P'] = 44; size['Q'] = 40; size['B'] = 19;
    size['I'] = 50; size['N'] = 20; size['O'] = 48;
  }
};
inline constexpr SizeTable kSizes{};
inline constexpr std::size_t message_size(char type) { return kSizes.size[static_cast<unsigned char>(type)]; }

struct Header {
  const char* p;
  char type() const { return p[0]; }
  std::uint16_t locate() const { return be16(p + 1); }
  std::uint16_t tracking() const { return be16(p + 3); }
  std::uint64_t timestamp() const { return be48(p + 5); }  // ns since midnight
};

inline std::string_view stock_field(const char* p) { return std::string_view(p, 8); }  // space padded

struct SystemEvent : Header {  // 'S'
  char event_code() const { return p[11]; }
};

struct StockDirectory : Header {  // 'R'
  std::string_view stock() const { return stock_field(p + 11); }
  char market_category() const { return p[19]; }
  char financial_status() const { return p[20]; }
  std::uint32_t round_lot_size() const { return be32(p + 21); }
};

struct StockTradingAction : Header {  // 'H'
  std::string_view stock() const { return stock_field(p + 11); }
  char trading_state() const { return p[19]; }  // H halted, P paused, Q quotation only, T trading
};

struct AddOrder : Header {  // 'A' and 'F' (F appends a 4-byte MPID attribution)
  std::uint64_t order_ref() const { return be64(p + 11); }
  char side() const { return p[19]; }  // 'B' or 'S'
  std::uint32_t shares() const { return be32(p + 20); }
  std::string_view stock() const { return stock_field(p + 24); }
  std::uint32_t price() const { return be32(p + 32); }  // 4 implied decimals
};

struct OrderExecuted : Header {  // 'E'
  std::uint64_t order_ref() const { return be64(p + 11); }
  std::uint32_t executed_shares() const { return be32(p + 19); }
  std::uint64_t match_number() const { return be64(p + 23); }
};

struct OrderExecutedWithPrice : Header {  // 'C'
  std::uint64_t order_ref() const { return be64(p + 11); }
  std::uint32_t executed_shares() const { return be32(p + 19); }
  std::uint64_t match_number() const { return be64(p + 23); }
  bool printable() const { return p[31] == 'Y'; }
  std::uint32_t price() const { return be32(p + 32); }
};

struct OrderCancel : Header {  // 'X'
  std::uint64_t order_ref() const { return be64(p + 11); }
  std::uint32_t cancelled_shares() const { return be32(p + 19); }
};

struct OrderDelete : Header {  // 'D'
  std::uint64_t order_ref() const { return be64(p + 11); }
};

struct OrderReplace : Header {  // 'U'
  std::uint64_t original_ref() const { return be64(p + 11); }
  std::uint64_t new_ref() const { return be64(p + 19); }
  std::uint32_t shares() const { return be32(p + 27); }
  std::uint32_t price() const { return be32(p + 31); }
};

struct Trade : Header {  // 'P' (executions against non-displayed orders)
  std::uint64_t order_ref() const { return be64(p + 11); }
  char side() const { return p[19]; }
  std::uint32_t shares() const { return be32(p + 20); }
  std::string_view stock() const { return stock_field(p + 24); }
  std::uint32_t price() const { return be32(p + 32); }
  std::uint64_t match_number() const { return be64(p + 36); }
};

struct CrossTrade : Header {  // 'Q'
  std::uint64_t shares() const { return be64(p + 11); }
  std::string_view stock() const { return stock_field(p + 19); }
  std::uint32_t price() const { return be32(p + 27); }
  std::uint64_t match_number() const { return be64(p + 31); }
  char cross_type() const { return p[39]; }
};

struct BrokenTrade : Header {  // 'B'
  std::uint64_t match_number() const { return be64(p + 11); }
};

// Default handler: every callback is a no-op. Derive and hide the ones you need;
// the parser calls them statically, so unused ones compile away.
struct NullHandler {
  void on_system_event(SystemEvent) {}
  void on_stock_directory(StockDirectory) {}
  void on_trading_action(StockTradingAction) {}
  void on_add(AddOrder) {}
  void on_executed(OrderExecuted) {}
  void on_executed_with_price(OrderExecutedWithPrice) {}
  void on_cancel(OrderCancel) {}
  void on_delete(OrderDelete) {}
  void on_replace(OrderReplace) {}
  void on_trade(Trade) {}
  void on_cross_trade(CrossTrade) {}
  void on_broken_trade(BrokenTrade) {}
  void on_other(Header) {}  // admin / auction / LULD messages we decode the header of only
};

}  // namespace itch
