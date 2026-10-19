#pragma once

// LV2 effect hosting for the real-time engine. This header deliberately does
// NOT include <lilv/lilv.h>: the opaque FxInstance it hands out is only ever
// touched by the audio thread through the static runInstance(), and every
// lilv call stays in Lv2Host.cpp on the control side.

#include <cstdint>
#include <string>
#include <vector>

namespace misuc::engine {

// One adjustable parameter carried between the UI and the host. `index` is the
// plugin port number; `value` is in the plugin's native units.
struct FxParam {
  uint32_t index = 0;
  float value = 1.0f;

  bool operator==(const FxParam&) const = default;
};

// One effect insert on a track's or bus's post-fader chain (what the UI
// publishes). `sidechainBus` names the engine bus index whose post-fader
// output feeds a sidechain-capable insert's key input; -1 keys the insert's own
// signal. Only honored on bus chains (track inserts carry -1).
struct FxInsertParam {
  std::string uri;
  bool enabled = true;
  std::vector<FxParam> params;
  int sidechainBus = -1;

  bool operator==(const FxInsertParam&) const = default;
};

// Metadata of a discovered, hostable effect and the adjustable parameter it
// exposes (label + min/max/default so a slider can be built).
struct FxParamInfo {
  uint32_t index = 0;
  std::string label;
  float min = 0.0f;
  float max = 1.0f;
  float def = 0.0f;
};

struct FxPluginInfo {
  std::string uri;
  std::string name;
  std::string author;
  uint32_t audioIns = 0;
  uint32_t audioOuts = 0;
  uint32_t ctrlOuts = 0;
  std::vector<FxParamInfo> params;
};

// Control-side host wrapper. Discovery, instantiation and teardown only ever
// run here (never on the audio thread). When lilv is not available at build
// time the constructor leaves the host unavailable and every call degrades to
// empty / false.
class Lv2Host {
 public:
  Lv2Host();
  ~Lv2Host();

  Lv2Host(const Lv2Host&) = delete;
  Lv2Host& operator=(const Lv2Host&) = delete;

  // Whether lilv was found at build time and the world could be created.
  bool available() const noexcept;

  // Discover the hostable effect set (cached after the first call). Only
  // float-only effects with at least one audio input and one audio output are
  // offered — the exact class the real-time fold stage can run without any
  // buffer negotiation.
  std::vector<FxPluginInfo> discoverEffects();
  const FxPluginInfo* find(const std::string& uri) const;

  // Create one effect instance sized for `maxFrames` audio staging and return
  // an owning opaque handle (nullptr on failure). `defaults` overrides the
  // plugin's default control values. Called on the control/UI thread.
  void* instantiate(const std::string& uri,
                    const std::vector<FxParam>& defaults, float sampleRate,
                    uint32_t maxFrames);
  void freeInstance(void* handle);

  // Render `frames` of inL/inR through the effect into outL/outR. The instance
  // stages audio in its own buffers, so out may alias in. Safe to call from
  // the audio thread once the handle is published (the handle is immutable
  // while referenced). `enabled` bypasses the plugin (copy only).
  static void run(void* handle, const float* inL, const float* inR,
                  float* outL, float* outR, uint32_t frames, bool enabled);

  // Write `value` (native plugin units) into the instance's control input port
  // `portIndex`. Safe to call from the audio thread between run() calls — the
  // plugin reads its control inputs from these very buffers at the start of the
  // next run, so this is how control-rate (per-block) automation reaches the
  // effect with no lock, allocation or lilv call. No-op when the port is not an
  // automatable control input (unknown / out of range). `enabled` param is not
  // needed here: a disabled insert simply never calls setControl.
  static void setControl(void* handle, uint32_t portIndex, float value);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace misuc::engine