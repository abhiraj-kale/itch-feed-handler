// ITCH 5.0 feed handler.
//
//   network thread:  UDP (multicast or unicast) -> MoldUDP64 sequencing and gap
//                    recovery -> zero-copy ITCH parser -> per-instrument books
//                    -> top-of-book updates pushed onto a lock-free SPSC queue
//   strategy thread: pops top-of-book updates (stand-in for a trading engine)
//
// Usage: feed_handler [--listen 239.1.1.1:31000] [--request 127.0.0.1:31001]
//                     [--verify file.itch] [--watch SYM1,SYM2] [--idle 5]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "itch/book.hpp"
#include "itch/file.hpp"
#include "itch/moldudp64.hpp"
#include "itch/parser.hpp"
#include "itch/platform.hpp"
#include "itch/spsc_queue.hpp"
#include "itch/udp.hpp"

using namespace itch;
using clk = std::chrono::steady_clock;

namespace {

using TobQueue = SpscQueue<TopOfBook, 1u << 16>;

struct Push {
  TobQueue* q;
  std::uint64_t* overflow;
  void operator()(const TopOfBook& t) const {
    if (!q->try_push(t)) ++*overflow;  // strategy fell behind: count, never block the feed
  }
};

bool split_host_port(const std::string& s, std::string& host, std::uint16_t& port) {
  const auto c = s.rfind(':');
  if (c == std::string::npos) return false;
  host = s.substr(0, c);
  port = static_cast<std::uint16_t>(std::atoi(s.c_str() + c + 1));
  return true;
}

double px(std::uint32_t p) { return p / 10000.0; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen_host = "239.1.1.1", req_host = "127.0.0.1", verify, watch;
  std::uint16_t listen_port = 31000, req_port = 31001;
  double idle_s = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--listen") split_host_port(next(), listen_host, listen_port);
    else if (k == "--request") split_host_port(next(), req_host, req_port);
    else if (k == "--verify") verify = next();
    else if (k == "--watch") watch = next();
    else if (k == "--idle") idle_s = std::atof(next().c_str());
  }

  net::NetInit net_init;
  net::UdpSocket sock;
  sock.reuse_addr();
  sock.recv_buffer(64 << 20);
  if (!sock.bind("0.0.0.0", listen_port)) {
    std::fprintf(stderr, "cannot bind port %u\n", listen_port);
    return 1;
  }
  if (net::is_multicast(listen_host) && !sock.join(listen_host)) {
    std::fprintf(stderr, "cannot join multicast group %s\n", listen_host.c_str());
    return 1;
  }
  sock.recv_timeout_ms(5);
  const sockaddr_in req_addr = net::make_addr(req_host, req_port);

  auto q = std::make_unique<TobQueue>();
  std::uint64_t overflow = 0;
  auto book = std::make_unique<BookBuilder<Push>>(1 << 16, Push{q.get(), &overflow});
  mold::MoldSession session;

  // ---- strategy thread: consumes top-of-book updates
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> consumed{0};
  std::thread strategy([&] {
    pin_thread(4);
    TopOfBook t;
    std::uint64_t n = 0;
    for (;;) {
      if (q->try_pop(t)) {
        ++n;
        continue;
      }
      if (done.load(std::memory_order_acquire)) {
        while (q->try_pop(t)) ++n;
        break;
      }
    }
    consumed = n;
  });

  // ---- network thread (this one)
  pin_thread(2);
  std::vector<char> buf(65536);
  auto on_msg = [&](std::uint64_t, const char* m, std::size_t len) { dispatch(m, len, *book); };
  auto on_gap = [&](std::uint64_t first, std::uint16_t count) {
    char r[mold::kHeader];
    mold::write_header(r, "ITCHDEMO01", first, count);
    sock.send_to(r, sizeof r, req_addr);
  };

  std::printf("listening on %s:%u, retransmission requests to %s:%u\n", listen_host.c_str(), listen_port,
              req_host.c_str(), req_port);
  clk::time_point first{}, last_rx = clk::now();
  bool started = false;
  for (;;) {
    const int n = sock.recv_from(buf.data(), static_cast<int>(buf.size()));
    if (n > 0) {
      if (!started) { started = true; first = clk::now(); }
      last_rx = clk::now();
      session.on_packet(buf.data(), static_cast<std::size_t>(n), on_msg, on_gap);
      if (session.finished()) break;
      continue;
    }
    if (n < 0) {
      std::fprintf(stderr, "recv error\n");
      break;
    }
    session.on_timeout(on_gap);  // 5 ms of silence: re-ask for anything still missing
    if (session.finished()) break;
    if (std::chrono::duration<double>(clk::now() - last_rx).count() > idle_s) {
      std::printf("idle for %.0f s, stopping\n", idle_s);
      break;
    }
  }
  const double secs = started ? std::chrono::duration<double>(clk::now() - first).count() : 0;
  done.store(true, std::memory_order_release);
  strategy.join();

  const auto& ss = session.stats();
  const auto& bs = book->stats();
  std::printf("\nsession   %llu packets, %llu messages in %.2f s (%.2f M msgs/s incl. waiting on the sender)\n",
              static_cast<unsigned long long>(ss.packets), static_cast<unsigned long long>(ss.messages), secs,
              secs > 0 ? static_cast<double>(ss.messages) / secs / 1e6 : 0.0);
  std::printf("recovery  %llu gaps, %llu requests, %llu messages recovered, %llu duplicates ignored\n",
              static_cast<unsigned long long>(ss.gaps), static_cast<unsigned long long>(ss.requests),
              static_cast<unsigned long long>(ss.recovered), static_cast<unsigned long long>(ss.duplicates));
  std::printf("book      %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces, %llu unknown refs\n",
              static_cast<unsigned long long>(bs.adds), static_cast<unsigned long long>(bs.executions),
              static_cast<unsigned long long>(bs.cancels), static_cast<unsigned long long>(bs.deletes),
              static_cast<unsigned long long>(bs.replaces), static_cast<unsigned long long>(bs.unknown_ref));
  std::printf("strategy  %llu top-of-book updates consumed, %llu dropped on a full queue\n",
              static_cast<unsigned long long>(consumed.load()), static_cast<unsigned long long>(overflow));

  if (!watch.empty()) {
    std::size_t s = 0;
    while (s < watch.size()) {
      std::size_t e = watch.find(',', s);
      if (e == std::string::npos) e = watch.size();
      const std::string sym = watch.substr(s, e - s);
      for (std::uint32_t loc = 0; loc < 65536; ++loc)
        if (book->known(static_cast<std::uint16_t>(loc)) && book->symbol(static_cast<std::uint16_t>(loc)) == sym) {
          const TopOfBook& t = book->top(static_cast<std::uint16_t>(loc));
          std::printf("  %-6s bid %10.4f x %-7u ask %10.4f x %-7u\n", sym.c_str(), px(t.bid_price), t.bid_shares,
                      px(t.ask_price), t.ask_shares);
        }
      s = e + 1;
    }
  }

  if (!verify.empty()) {
    auto ref = std::make_unique<BookBuilder<>>();
    parse_file(verify, *ref);
    const bool match = ref->checksum() == book->checksum() && ref->live_orders() == book->live_orders();
    std::printf("verify    book rebuilt from the network %s the book built from %s (%zu live orders, checksum %016llx)\n",
                match ? "MATCHES" : "DOES NOT MATCH", verify.c_str(), book->live_orders(),
                static_cast<unsigned long long>(book->checksum()));
    return match ? 0 : 1;
  }
  return 0;
}
