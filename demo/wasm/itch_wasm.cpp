// C ABI over the feed handler for the browser demo (compiled with Emscripten).
//
// The page generates an ITCH session, then pushes it packet by packet through
// a simulated lossy network into the real MoldUDP64 session, parser, and book
// builder. A simulated retransmission server answers gap requests after a
// short delay. Events (delivered, dropped, requested, retransmitted) are
// written to a buffer the page reads to animate the wire.

#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <vector>

#include "itch/book.hpp"
#include "itch/generator.hpp"
#include "itch/moldudp64.hpp"
#include "itch/parser.hpp"

using namespace itch;

namespace {

const char kSess[10] = {'W', 'E', 'B', 'D', 'E', 'M', 'O', '0', '0', '1'};

struct Event {  // 16 bytes, read from JS as Uint32 x 4
  std::uint32_t kind;   // 0 delivered, 1 dropped, 2 request, 3 retransmit, 4 duplicate, 5 late
  std::uint32_t seq;    // first message sequence number
  std::uint32_t count;  // messages
  std::uint32_t pad;
};

struct Counter : NullHandler {};

std::vector<char> g_data;               // BinaryFILE buffer
std::vector<std::uint32_t> g_off;       // message seq n -> offset of its length prefix (n - 1)
std::uint32_t g_stocks = 0;
std::unique_ptr<BookBuilder<>> g_book;  // fed by the lossy network session
std::unique_ptr<BookBuilder<>> g_ref;   // fed straight from the source, for verification
std::uint64_t g_ref_seq = 1;
std::unique_ptr<mold::MoldSession> g_sess;
std::mt19937_64 g_rng(1);
double g_drop = 0.0, g_dup = 0.0, g_reorder = 0.0;
std::uint64_t g_next = 1;               // next live sequence number to send
std::uint64_t g_step = 0, g_last_progress = 0;
bool g_eos_sent = false;
std::vector<std::string> g_held;         // packets held back for reordering
struct Req { std::uint64_t due, first; std::uint32_t count; };
std::deque<Req> g_reqs;
std::vector<Event> g_events;
std::uint64_t g_type_count[256];
std::vector<std::uint32_t> g_activity;  // messages per locate
std::uint64_t g_last_order_seq = 0;
std::uint64_t g_last_seq[256];
double g_out[16];
std::uint32_t g_levels[2][64 * 3];
char g_sym[16];

std::string build_packet(std::uint64_t first, std::uint64_t limit, std::uint64_t* got) {
  mold::PacketBuilder pb(kSess, 1400);
  pb.start(first);
  std::uint64_t n = 0;
  const std::uint64_t total = g_off.size();
  while (first + n <= total && n < limit) {
    const char* p = g_data.data() + g_off[first + n - 1];
    const std::size_t len = be16(p);
    if (!pb.fits(len)) break;
    pb.add(p + 2, len);
    ++n;
  }
  *got = n;
  const auto& v = pb.finish();
  return std::string(v.data(), v.size());
}

void push_event(std::uint32_t kind, std::uint64_t seq, std::uint64_t count) {
  if (g_events.size() < 4096) g_events.push_back(Event{kind, static_cast<std::uint32_t>(seq), static_cast<std::uint32_t>(count), 0});
}

void deliver(const std::string& pkt) {
  const std::uint64_t before = g_sess->expected();
  g_sess->on_packet(
      pkt.data(), pkt.size(),
      [](std::uint64_t seq, const char* m, std::size_t len) {
        dispatch(m, len, *g_book);
        ++g_type_count[static_cast<unsigned char>(m[0])];
        const std::uint16_t loc = be16(m + 1);
        if (loc < g_activity.size()) ++g_activity[loc];
        g_last_seq[static_cast<unsigned char>(m[0])] = seq;
        if (m[0] == 'A' || m[0] == 'U' || m[0] == 'E' || m[0] == 'D' || m[0] == 'X' || m[0] == 'F') g_last_order_seq = seq;
      },
      [](std::uint64_t first, std::uint16_t count) {
        g_reqs.push_back(Req{g_step + 6 + (g_rng() % 6), first, count});
        push_event(2, first, count);
      });
  if (g_sess->expected() > before) g_last_progress = g_step;
}

}  // namespace

extern "C" {

// Generates a session and resets the network simulation. Returns message count.
EMSCRIPTEN_KEEPALIVE int itch_generate(int messages, int stocks, int seed) {
  g_data.clear();
  g_off.clear();
  g_data.reserve(static_cast<std::size_t>(messages) * 34);
  GenConfig cfg;
  cfg.messages = static_cast<std::uint64_t>(messages);
  cfg.stocks = static_cast<std::uint32_t>(stocks);
  cfg.seed = static_cast<std::uint64_t>(seed);
  Generator gen(cfg);
  gen.run([](const char* m, std::size_t len) {
    const std::size_t off = g_data.size();
    g_off.push_back(static_cast<std::uint32_t>(off));
    g_data.resize(off + 2 + len);
    put16(g_data.data() + off, static_cast<std::uint16_t>(len));
    std::memcpy(g_data.data() + off + 2, m, len);
  });
  g_stocks = static_cast<std::uint32_t>(stocks);
  return static_cast<int>(g_off.size());
}

EMSCRIPTEN_KEEPALIVE void itch_reset(double drop, double dup, double reorder, int seed) {
  g_book = std::make_unique<BookBuilder<>>(1 << 14);
  g_ref = std::make_unique<BookBuilder<>>(1 << 14);
  g_ref_seq = 1;
  g_sess = std::make_unique<mold::MoldSession>();
  g_rng.seed(static_cast<std::uint64_t>(seed));
  g_drop = drop;
  g_dup = dup;
  g_reorder = reorder;
  g_next = 1;
  g_step = g_last_progress = 0;
  g_eos_sent = false;
  g_held.clear();
  g_reqs.clear();
  g_events.clear();
  std::memset(g_type_count, 0, sizeof g_type_count);
  g_activity.assign(65536, 0);
  g_last_order_seq = 0;
  std::memset(g_last_seq, 0, sizeof g_last_seq);
}

EMSCRIPTEN_KEEPALIVE void itch_set_loss(double drop, double dup, double reorder) {
  g_drop = drop;
  g_dup = dup;
  g_reorder = reorder;
}

// Advances the simulation by up to `packets` live packets. Returns 1 when done.
EMSCRIPTEN_KEEPALIVE int itch_step(int packets) {
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const std::uint64_t total = g_off.size();
  for (int i = 0; i < packets; ++i) {
    ++g_step;
    // Retransmission server: answer requests that are due (it loses some too).
    while (!g_reqs.empty() && g_reqs.front().due <= g_step) {
      Req r = g_reqs.front();
      g_reqs.pop_front();
      std::uint64_t seq = r.first, left = r.count;
      for (int k = 0; k < 32 && left > 0 && seq <= total; ++k) {
        std::uint64_t got = 0;
        const std::string pkt = build_packet(seq, left, &got);
        if (got == 0) break;
        if (u(g_rng) < g_drop * 0.5) push_event(1, seq, got);
        else { push_event(3, seq, got); deliver(pkt); }
        seq += got;
        left -= got;
      }
    }
    if (g_next <= total) {
      std::uint64_t got = 0;
      const std::string pkt = build_packet(g_next, 0xFFFE, &got);
      const double r = u(g_rng);
      if (r < g_drop) {
        push_event(1, g_next, got);
      } else if (r < g_drop + g_reorder) {
        g_held.push_back(pkt);  // arrives after the next packet
        push_event(5, g_next, got);
      } else {
        push_event(0, g_next, got);
        deliver(pkt);
        if (u(g_rng) < g_dup) { push_event(4, g_next, got); deliver(pkt); }
        if (!g_held.empty()) {
          for (auto& h : g_held) deliver(h);
          g_held.clear();
        }
      }
      g_next += got;
    } else if (!g_eos_sent || (g_step % 8) == 0) {
      for (auto& h : g_held) deliver(h);
      g_held.clear();
      char eos[mold::kHeader];
      mold::write_header(eos, kSess, total + 1, mold::kEndOfSession);
      deliver(std::string(eos, sizeof eos));
      g_eos_sent = true;
    }
    if (g_sess->in_gap() && g_step - g_last_progress > 40 && g_reqs.empty()) {
      g_last_progress = g_step;
      g_sess->on_timeout([](std::uint64_t first, std::uint16_t count) {
        g_reqs.push_back(Req{g_step + 4, first, count});
        push_event(2, first, count);
      });
    }
    if (g_sess->finished()) return 1;
  }
  return g_sess->finished() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE Event* itch_events() { return g_events.data(); }
EMSCRIPTEN_KEEPALIVE int itch_event_count() { return static_cast<int>(g_events.size()); }
EMSCRIPTEN_KEEPALIVE void itch_clear_events() { g_events.clear(); }

// [packets, messages, gaps, requests, recovered, duplicates, expected, total,
//  live orders, tob updates, unknown refs, next live seq]
EMSCRIPTEN_KEEPALIVE double* itch_stats() {
  const auto& s = g_sess->stats();
  g_out[0] = static_cast<double>(s.packets);
  g_out[1] = static_cast<double>(s.messages);
  g_out[2] = static_cast<double>(s.gaps);
  g_out[3] = static_cast<double>(s.requests);
  g_out[4] = static_cast<double>(s.recovered);
  g_out[5] = static_cast<double>(s.duplicates);
  g_out[6] = static_cast<double>(g_sess->expected());
  g_out[7] = static_cast<double>(g_off.size());
  g_out[8] = static_cast<double>(g_book->live_orders());
  g_out[9] = static_cast<double>(g_book->stats().tob_updates);
  g_out[10] = static_cast<double>(g_book->stats().unknown_ref);
  g_out[11] = static_cast<double>(g_next);
  return g_out;
}

EMSCRIPTEN_KEEPALIVE double itch_type_count(int t) { return static_cast<double>(g_type_count[t & 255]); }
EMSCRIPTEN_KEEPALIVE std::uint32_t* itch_activity() { return g_activity.data(); }

EMSCRIPTEN_KEEPALIVE const char* itch_symbol(int loc) {
  const std::string s = g_book->symbol(static_cast<std::uint16_t>(loc));
  std::memset(g_sym, 0, sizeof g_sym);
  std::memcpy(g_sym, s.data(), s.size() < 15 ? s.size() : 15);
  return g_sym;
}

// Top of book for one instrument: [bid px, bid shares, ask px, ask shares].
EMSCRIPTEN_KEEPALIVE double* itch_top(int loc) {
  const TopOfBook& t = g_book->top(static_cast<std::uint16_t>(loc));
  g_out[0] = t.bid_price;
  g_out[1] = t.bid_shares;
  g_out[2] = t.ask_price;
  g_out[3] = t.ask_shares;
  return g_out;
}

EMSCRIPTEN_KEEPALIVE int itch_depth(int loc, int side, int max) {
  if (max > 64) max = 64;
  int n = 0;
  g_book->for_each_level(static_cast<std::uint16_t>(loc), side, [&](std::uint32_t p, std::uint64_t s, std::uint32_t o) {
    if (n < max) {
      g_levels[side][n * 3] = p;
      g_levels[side][n * 3 + 1] = static_cast<std::uint32_t>(s);
      g_levels[side][n * 3 + 2] = o;
      ++n;
    }
  });
  return n;
}
EMSCRIPTEN_KEEPALIVE std::uint32_t* itch_depth_ptr(int side) { return g_levels[side]; }

// Most recent order message delivered: pointer to its bytes, length via itch_msg_len.
EMSCRIPTEN_KEEPALIVE const char* itch_last_order() {
  if (!g_last_order_seq) return nullptr;
  return g_data.data() + g_off[g_last_order_seq - 1] + 2;
}
// Most recent message of one type (0 if none yet), and its sequence number.
EMSCRIPTEN_KEEPALIVE const char* itch_last_of(int type) {
  const std::uint64_t seq = g_last_seq[type & 255];
  return seq ? g_data.data() + g_off[seq - 1] + 2 : nullptr;
}
EMSCRIPTEN_KEEPALIVE double itch_last_seq_of(int type) { return static_cast<double>(g_last_seq[type & 255]); }
EMSCRIPTEN_KEEPALIVE double itch_last_order_seq() { return static_cast<double>(g_last_order_seq); }

// Catches the reference book up to the network session and compares the two.
// Returns 1 match, 0 mismatch, -1 while a gap is still being recovered.
EMSCRIPTEN_KEEPALIVE int itch_verify() {
  if (g_sess->in_gap()) return -1;
  const std::uint64_t upto = g_sess->expected();
  while (g_ref_seq < upto) {
    const char* p = g_data.data() + g_off[g_ref_seq - 1];
    dispatch(p + 2, be16(p), *g_ref);
    ++g_ref_seq;
  }
  return g_ref->checksum() == g_book->checksum() ? 1 : 0;
}

// Times framing-only, full decode, and parse + book on the generated session.
// Results: [messages, framing ms, book ms, bytes].
EMSCRIPTEN_KEEPALIVE double* itch_bench() {
  struct Touch : NullHandler {
    std::uint64_t sum = 0;
    void on_add(AddOrder m) { sum += m.order_ref(); }
    void on_delete(OrderDelete m) { sum += m.order_ref(); }
    void on_replace(OrderReplace m) { sum += m.new_ref(); }
    void on_executed(OrderExecuted m) { sum += m.order_ref(); }
    void on_other(Header m) { sum += m.locate(); }
  } t;
  double t0 = emscripten_get_now();
  const StreamResult r = parse_stream(g_data.data(), g_data.size(), t);
  const double framing = emscripten_get_now() - t0;
  auto b = std::make_unique<BookBuilder<>>(1 << 14);
  t0 = emscripten_get_now();
  parse_stream(g_data.data(), g_data.size(), *b);
  const double book = emscripten_get_now() - t0;
  g_out[0] = static_cast<double>(r.messages);
  g_out[1] = framing;
  g_out[2] = book;
  g_out[3] = static_cast<double>(g_data.size());
  g_out[4] = static_cast<double>(t.sum & 1);
  return g_out;
}

}  // extern "C"
