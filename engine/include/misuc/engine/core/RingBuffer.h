#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace misuc::core {

// Vyukov lock-free SPSC ring buffer for trivially-copyable element types.
// Single producer, single consumer. Non-blocking under contention-free use.
template <typename T, std::size_t Capacity>
class SPSCRing {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static constexpr std::size_t kMask = Capacity - 1;

public:
  SPSCRing()
    : slots_(new Slot[Capacity])
  {
    for (std::size_t i = 0; i < Capacity; ++i)
      slots_[i].seq.store(i, std::memory_order_relaxed);
  }

  // Producer only.
  bool push(const T& value) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    Slot& s = slots_[h & kMask];
    if (s.seq.load(std::memory_order_acquire) != h)
      return false; // full
    s.value = value;
    s.seq.store(h + 1, std::memory_order_release);
    head_.store(h + 1, std::memory_order_relaxed);
    return true;
  }

  // Consumer only.
  bool pop(T& value) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    Slot& s = slots_[t & kMask];
    if (s.seq.load(std::memory_order_acquire) != t + 1)
      return false; // empty
    value = s.value;
    s.seq.store(t + kMask + 1, std::memory_order_release);
    tail_.store(t + 1, std::memory_order_relaxed);
    return true;
  }

private:
  struct alignas(64) Slot {
    std::atomic<std::size_t> seq{0};
    T value{};
  };
  std::unique_ptr<Slot[]> slots_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

// Small shared snapshot published from the audio thread and polled by the UI.
// All fields are atomics; the audio thread stores, the UI thread loads.
struct MeterSnapshot {
  std::atomic<float> peakL{0.f};
  std::atomic<float> peakR{0.f};
  std::atomic<float> rmsL{0.f};
  std::atomic<float> rmsR{0.f};
  std::atomic<bool>  clipped{false};
  std::atomic<float> inPeak{0.f};
  std::atomic<float> inRms{0.f};
  std::atomic<bool>  inClipped{false};
  std::atomic<uint64_t> playheadSamples{0};
  std::atomic<bool>  playing{false};
  std::atomic<uint64_t> xruns{0};
  std::atomic<float> cpuPercent{0.f};
  std::atomic<float> masterDb{0.f};

  void clear() {
    peakL.store(0.f, std::memory_order_relaxed);
    peakR.store(0.f, std::memory_order_relaxed);
    rmsL.store(0.f, std::memory_order_relaxed);
    rmsR.store(0.f, std::memory_order_relaxed);
    clipped.store(false, std::memory_order_relaxed);
    inPeak.store(0.f, std::memory_order_relaxed);
    inRms.store(0.f, std::memory_order_relaxed);
    inClipped.store(false, std::memory_order_relaxed);
  }
};

// One-pole smoothing used to ramp gains and avoid zipper noise in the
// real-time path. No allocations, no locks.
class Smoother {
public:
  // Configure the smoothing coefficient once.
  void setCoefficient(float coef) noexcept { coef_ = coef; }
  void setTarget(float target) noexcept { target_ = target; }
  void reset(float value) noexcept { target_ = value; current_ = value; }

  // Advance one sample toward the target. Called on the RT thread.
  float tick() noexcept {
    current_ += coef_ * (target_ - current_);
    return current_;
  }

  float value() const noexcept { return current_; }

private:
  float coef_ = 0.001f;   // one-pole lambda coefficient
  float target_ = 0.f;
  float current_ = 0.f;
};

} // namespace misuc::core