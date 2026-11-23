#pragma once

// MoldUDP64: the UDP framing Nasdaq uses to multicast ITCH.
//
// Packet:  session(10) sequence(8) count(2) then `count` blocks of
//          length(2) + message. `sequence` is the number of the first message
//          in the packet. count 0 = heartbeat (sequence = next expected),
//          count 0xFFFF = end of session.
// Request: session(10) sequence(8) count(2), sent unicast to the
//          retransmission server to ask for messages [sequence, sequence+count).
//
// MoldSession is the receive side's sequencing logic, independent of sockets
// so it can be tested deterministically:
//   - in-order packet      -> deliver its messages
//   - duplicate / overlap  -> deliver only the messages not seen yet
//   - packet from the future (a gap) -> park it, ask for the missing range,
//     deliver parked packets as soon as the gap is filled.

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "itch/endian.hpp"

namespace itch::mold {

inline constexpr std::size_t kHeader = 20;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct PacketHeader {
  char session[10];
  std::uint64_t sequence;
  std::uint16_t count;
};

inline bool read_header(const char* p, std::size_t len, PacketHeader& h) {
  if (len < kHeader) return false;
  std::memcpy(h.session, p, 10);
  h.sequence = be64(p + 10);
  h.count = be16(p + 18);
  return true;
}

inline void write_header(char* p, const char session[10], std::uint64_t seq, std::uint16_t count) {
  std::memcpy(p, session, 10);
  put64(p + 10, seq);
  put16(p + 18, count);
}

struct SessionStats {
  std::uint64_t packets = 0, messages = 0, duplicates = 0, gaps = 0, requests = 0, recovered = 0, parked = 0;
};

class MoldSession {
 public:
  // Called for each message in sequence order: f(seq, msg_ptr, msg_len).
  // Called when a range must be requested: g(first_seq, count).
  explicit MoldSession(std::uint64_t first_expected = 1) : expected_(first_expected) {}

  template <class OnMessage, class OnGap>
  void on_packet(const char* p, std::size_t len, OnMessage&& on_msg, OnGap&& on_gap) {
    PacketHeader h{};
    if (!read_header(p, len, h)) return;
    ++stats_.packets;
    if (h.count == kEndOfSession) {
      end_seq_ = h.sequence;
      ended_ = true;
      if (h.sequence > expected_) request_gap(h.sequence, on_gap);
      return;
    }
    if (h.count == 0) {  // heartbeat carries the next sequence number
      if (h.sequence > expected_) request_gap(h.sequence, on_gap);
      return;
    }
    if (h.sequence > expected_) {
      // Future packet: keep it and ask for what is missing in front of it.
      if (parked_.emplace(h.sequence, std::string(p, len)).second) ++stats_.parked;
      request_gap(parked_.begin()->first, on_gap);
      return;
    }
    deliver(p, len, h, on_msg);
    drain(on_msg);
    // A second hole further on shows up once the first one is filled.
    if (!parked_.empty()) request_gap(parked_.begin()->first, on_gap);
  }

  // Re-issues the outstanding request if nothing arrived for a while.
  template <class OnGap>
  void on_timeout(OnGap&& on_gap) {
    if (!parked_.empty()) request_gap(parked_.begin()->first, on_gap, true);
    else if (ended_ && end_seq_ > expected_) request_gap(end_seq_, on_gap, true);
  }

  std::uint64_t expected() const { return expected_; }
  bool in_gap() const { return !parked_.empty() || (ended_ && end_seq_ > expected_); }
  bool finished() const { return ended_ && expected_ >= end_seq_ && parked_.empty(); }
  const SessionStats& stats() const { return stats_; }

 private:
  template <class OnMessage>
  void deliver(const char* p, std::size_t len, const PacketHeader& h, OnMessage& on_msg) {
    std::size_t off = kHeader;
    std::uint64_t seq = h.sequence;
    for (std::uint16_t i = 0; i < h.count; ++i, ++seq) {
      if (off + 2 > len) return;
      const std::size_t mlen = be16(p + off);
      if (off + 2 + mlen > len) return;
      if (seq == expected_) {
        on_msg(seq, p + off + 2, mlen);
        ++expected_;
        ++stats_.messages;
        if (seq < recovering_until_) ++stats_.recovered;
      } else {
        ++stats_.duplicates;
      }
      off += 2 + mlen;
    }
  }

  template <class OnMessage>
  void drain(OnMessage& on_msg) {
    while (!parked_.empty()) {
      auto it = parked_.begin();
      if (it->first > expected_) break;
      PacketHeader h{};
      read_header(it->second.data(), it->second.size(), h);
      deliver(it->second.data(), it->second.size(), h, on_msg);
      parked_.erase(it);
    }
  }

  // Asks for [expected_, upto). While a request is outstanding, newer packets
  // only get parked; the timeout path re-asks if the answer was lost too.
  template <class OnGap>
  void request_gap(std::uint64_t upto, OnGap& on_gap, bool force = false) {
    if (upto <= expected_) return;
    if (!force && requested_upto_ > expected_) return;
    if (!force) ++stats_.gaps;
    requested_upto_ = upto;
    if (upto > recovering_until_) recovering_until_ = upto;
    const std::uint64_t n = upto - expected_;
    on_gap(expected_, static_cast<std::uint16_t>(n > 0xFFFE ? 0xFFFE : n));
    ++stats_.requests;
  }

  std::uint64_t expected_;
  std::map<std::uint64_t, std::string> parked_;
  std::uint64_t requested_upto_ = 0, recovering_until_ = 0;
  std::uint64_t end_seq_ = 0;
  bool ended_ = false;
  SessionStats stats_;
};

// Packs length-prefixed messages into MoldUDP64 packets (sender side).
class PacketBuilder {
 public:
  explicit PacketBuilder(const char session[10], std::size_t max_payload = 1400) : max_(max_payload) {
    std::memcpy(session_, session, 10);
    buf_.reserve(max_payload);
  }
  void start(std::uint64_t seq) {
    buf_.assign(kHeader, 0);
    seq_ = seq;
    count_ = 0;
  }
  bool fits(std::size_t mlen) const { return buf_.size() + 2 + mlen <= max_ && count_ < 0xFFFE; }
  void add(const char* m, std::size_t mlen) {
    const std::size_t off = buf_.size();
    buf_.resize(off + 2 + mlen);
    put16(buf_.data() + off, static_cast<std::uint16_t>(mlen));
    std::memcpy(buf_.data() + off + 2, m, mlen);
    ++count_;
  }
  const std::vector<char>& finish() {
    write_header(buf_.data(), session_, seq_, count_);
    return buf_;
  }
  std::uint16_t count() const { return count_; }

 private:
  char session_[10];
  std::size_t max_;
  std::vector<char> buf_;
  std::uint64_t seq_ = 1;
  std::uint16_t count_ = 0;
};

}  // namespace itch::mold
