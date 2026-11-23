#pragma once

// Open-addressing hash map from ITCH order reference number to the live order.
// Records are stored inline (24 bytes), probing is linear, deletion uses
// backward shift so heavy add/delete churn never leaves tombstones behind, and
// the table doubles when it passes 50% load. Reference 0 marks an empty slot.

#include <bit>
#include <cstdint>
#include <vector>

namespace itch {

struct OrderRec {
  std::uint64_t ref = 0;
  std::uint32_t price = 0;
  std::uint32_t shares = 0;
  std::uint16_t locate = 0;
  std::uint8_t side = 0;  // 0 buy, 1 sell
};
static_assert(sizeof(OrderRec) == 24);

class OrderMap {
 public:
  explicit OrderMap(std::size_t expected = 1 << 16) {
    const std::size_t cap = std::bit_ceil(expected * 2 < 16 ? 16 : expected * 2);
    slots_.assign(cap, OrderRec{});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
  }

  // Returns a pointer valid until the next insert, or nullptr.
  OrderRec* find(std::uint64_t ref) {
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      OrderRec& s = slots_[i];
      if (s.ref == ref) return &s;
      if (s.ref == 0) return nullptr;
    }
  }

  // Returns false for ref 0 or a duplicate.
  bool insert(const OrderRec& r) {
    if (r.ref == 0) return false;
    if ((size_ + 1) * 2 > slots_.size()) grow();
    for (std::size_t i = home(r.ref);; i = (i + 1) & mask_) {
      OrderRec& s = slots_[i];
      if (s.ref == r.ref) return false;
      if (s.ref == 0) {
        s = r;
        ++size_;
        return true;
      }
    }
  }

  void erase(OrderRec* s) {
    std::size_t hole = static_cast<std::size_t>(s - slots_.data());
    for (std::size_t i = (hole + 1) & mask_;; i = (i + 1) & mask_) {
      if (slots_[i].ref == 0) break;
      const std::size_t h = home(slots_[i].ref);
      // Move slot i into the hole unless its home lies cyclically in (hole, i].
      if (((i - h) & mask_) >= ((i - hole) & mask_)) {
        slots_[hole] = slots_[i];
        hole = i;
      }
    }
    slots_[hole] = OrderRec{};
    --size_;
  }

  // Pulls the slot an upcoming lookup will touch into cache ahead of time.
  void prefetch(std::uint64_t ref) const { __builtin_prefetch(&slots_[home(ref)]); }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return slots_.size(); }

 private:
  std::size_t home(std::uint64_t ref) const { return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ull) >> shift_); }

  void grow() {
    std::vector<OrderRec> old;
    old.swap(slots_);
    slots_.assign(old.size() * 2, OrderRec{});
    mask_ = slots_.size() - 1;
    shift_ = 64 - std::countr_zero(slots_.size());
    size_ = 0;
    for (const OrderRec& r : old)
      if (r.ref != 0) insert(r);
  }

  std::vector<OrderRec> slots_;
  std::size_t mask_ = 0;
  int shift_ = 0;
  std::size_t size_ = 0;
};

}  // namespace itch
