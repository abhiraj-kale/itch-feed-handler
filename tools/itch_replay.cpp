// Replays an ITCH capture as a MoldUDP64 feed, the way Nasdaq disseminates it,
// plus a MoldUDP64 retransmission server so receivers can recover gaps.
//
//   - Packs messages into MoldUDP64 packets (<= 1400 bytes payload) and sends
//     them to a multicast group or unicast address at a fixed message rate.
//   - --drop p skips a random fraction of packets on purpose so the receiver
//     has to detect the gap and request a retransmission.
//   - A second thread answers retransmission requests on --req-port.
//   - Ends with end-of-session packets while still serving requests.
//
// Usage: itch_replay <file.itch> [--dest 239.1.1.1:31000] [--req-port 31001]
//                    [--rate 2000000] [--drop 0.001] [--seed 1] [--linger 3]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "itch/endian.hpp"
#include "itch/file.hpp"
#include "itch/moldudp64.hpp"
#include "itch/udp.hpp"

using namespace itch;
using clk = std::chrono::steady_clock;

namespace {

const char kSession[10] = {'I', 'T', 'C', 'H', 'D', 'E', 'M', 'O', '0', '1'};

struct Args {
  std::string file, host = "239.1.1.1";
  std::uint16_t port = 31000, req_port = 31001;
  double rate = 2'000'000, drop = 0.0, linger = 3.0;
  std::uint64_t seed = 1;
};

bool split_host_port(const std::string& s, std::string& host, std::uint16_t& port) {
  const auto c = s.rfind(':');
  if (c == std::string::npos) return false;
  host = s.substr(0, c);
  port = static_cast<std::uint16_t>(std::atoi(s.c_str() + c + 1));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (k == "--dest") split_host_port(next(), a.host, a.port);
    else if (k == "--req-port") a.req_port = static_cast<std::uint16_t>(std::atoi(next()));
    else if (k == "--rate") a.rate = std::atof(next());
    else if (k == "--drop") a.drop = std::atof(next());
    else if (k == "--seed") a.seed = std::strtoull(next(), nullptr, 10);
    else if (k == "--linger") a.linger = std::atof(next());
    else a.file = k;
  }
  if (a.file.empty()) {
    std::fprintf(stderr, "usage: itch_replay <file.itch> [--dest host:port] [--req-port N] [--rate msgs/s] [--drop p]\n");
    return 2;
  }

  std::vector<char> data;
  if (!read_file(a.file, data)) {
    std::fprintf(stderr, "cannot read %s\n", a.file.c_str());
    return 1;
  }
  // Index every message so any sequence range can be rebuilt for retransmission.
  std::vector<std::uint64_t> msg_off;  // offset of each length prefix; seq n is msg_off[n - 1]
  for (std::size_t off = 0; off + 2 <= data.size();) {
    const std::size_t len = be16(data.data() + off);
    if (off + 2 + len > data.size()) break;
    msg_off.push_back(off);
    off += 2 + len;
  }
  const std::uint64_t total = msg_off.size();
  std::printf("loaded %llu messages from %s\n", static_cast<unsigned long long>(total), a.file.c_str());

  net::NetInit net_init;
  net::UdpSocket feed;
  if (net::is_multicast(a.host)) feed.multicast_loopback(true);
  const sockaddr_in dest = net::make_addr(a.host, a.port);

  // Builds the packet starting at `seq` with as many messages as fit (<= limit).
  auto build = [&](mold::PacketBuilder& pb, std::uint64_t seq, std::uint64_t limit) -> std::uint64_t {
    pb.start(seq);
    std::uint64_t n = 0;
    while (seq + n <= total && n < limit) {
      const char* p = data.data() + msg_off[seq + n - 1];
      const std::size_t len = be16(p);
      if (!pb.fits(len)) break;
      pb.add(p + 2, len);
      ++n;
    }
    return n;
  };

  // ---- retransmission server
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> retrans_packets{0}, requests{0};
  std::thread server([&] {
    net::UdpSocket req;
    req.reuse_addr();
    if (!req.bind("0.0.0.0", a.req_port)) {
      std::fprintf(stderr, "cannot bind request port %u\n", a.req_port);
      return;
    }
    req.recv_timeout_ms(50);
    mold::PacketBuilder pb(kSession);
    char buf[64];
    while (!stop.load(std::memory_order_relaxed)) {
      sockaddr_in from{};
      const int n = req.recv_from(buf, sizeof buf, &from);
      mold::PacketHeader h;
      if (n < static_cast<int>(mold::kHeader) || !mold::read_header(buf, static_cast<std::size_t>(n), h)) continue;
      ++requests;
      std::uint64_t seq = h.sequence, left = h.count;
      int sent = 0;
      while (left > 0 && seq >= 1 && seq <= total && sent < 64) {  // cap per request, client re-asks
        const std::uint64_t got = build(pb, seq, left);
        if (got == 0) break;
        const auto& pkt = pb.finish();
        req.send_to(pkt.data(), static_cast<int>(pkt.size()), from);
        seq += got;
        left -= got;
        ++sent;
        ++retrans_packets;
      }
    }
  });

  // ---- live feed
  std::mt19937_64 rng(a.seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  mold::PacketBuilder pb(kSession);
  std::uint64_t seq = 1, packets = 0, dropped = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the receiver settle
  const auto t0 = clk::now();
  while (seq <= total) {
    const std::uint64_t n = build(pb, seq, 0xFFFE);
    if (a.rate > 0) {
      const auto due = t0 + std::chrono::nanoseconds(static_cast<long long>(static_cast<double>(seq - 1) * 1e9 / a.rate));
      while (clk::now() < due) {
      }
    }
    const auto& pkt = pb.finish();
    if (a.drop > 0 && u(rng) < a.drop) ++dropped;
    else feed.send_to(pkt.data(), static_cast<int>(pkt.size()), dest);
    seq += n;
    ++packets;
  }
  const double secs = std::chrono::duration<double>(clk::now() - t0).count();
  std::printf("sent %llu packets (%llu dropped on purpose) in %.2f s, %.2f M msgs/s\n",
              static_cast<unsigned long long>(packets), static_cast<unsigned long long>(dropped), secs,
              static_cast<double>(total) / secs / 1e6);

  // End of session, repeated while late requests are still served.
  char eos[mold::kHeader];
  mold::write_header(eos, kSession, total + 1, mold::kEndOfSession);
  const auto t_end = clk::now() + std::chrono::milliseconds(static_cast<long long>(a.linger * 1000));
  while (clk::now() < t_end) {
    feed.send_to(eos, sizeof eos, dest);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop = true;
  server.join();
  std::printf("served %llu retransmission requests with %llu packets\n",
              static_cast<unsigned long long>(requests.load()), static_cast<unsigned long long>(retrans_packets.load()));
  return 0;
}
