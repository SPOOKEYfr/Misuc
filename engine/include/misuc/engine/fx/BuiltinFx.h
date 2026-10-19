#pragma once

// Built-in FX inserts offered by the engine on every build (independent of
// lilv/LV2 availability). Metadata mirrors the DSP port layout in Eq.h so the
// UI can build sliders and the publish path can instantiate from the same
// parameter vector.

#include <misuc/engine/dsp/Compressor.h>
#include <misuc/engine/dsp/Eq.h>
#include <misuc/engine/fx/Lv2Host.h>

namespace misuc::engine {

inline constexpr const char* kBuiltinEqUri = "builtin:parametric-eq";
inline constexpr const char* kBuiltinCompUri = "builtin:sc-compressor";

// The parametric EQ's catalog entry. Purely static metadata shared by
// discoverFx() and fxInfo(); never mutated after construction.
inline const FxPluginInfo& builtinEqInfo() {
  static const FxPluginInfo info = [] {
    FxPluginInfo i;
    i.uri = kBuiltinEqUri;
    i.name = "Built-in EQ";
    i.author = "Misuc";
    i.audioIns = 1;
    i.audioOuts = 1;
    i.ctrlOuts = 0;

    using dsp::ParametricEq;
    auto addPort = [&i](uint32_t port, const char* label, float min, float max,
                        float def) {
      i.params.push_back(FxParamInfo{port, label, min, max, def});
    };
    addPort(ParametricEq::kHpPort, "High-pass", ParametricEq::kMinHz,
            ParametricEq::kMaxHz, ParametricEq::kMinHz);
    addPort(ParametricEq::kLpPort, "Low-pass", ParametricEq::kMinHz,
            ParametricEq::kMaxHz, ParametricEq::kMaxHz);
    for (uint32_t b = 0; b < ParametricEq::kBandCount; ++b) {
      const uint32_t base =
          ParametricEq::kBandBase + ParametricEq::kPortsPerBand * b;
      const std::string band = "Band " + std::to_string(b + 1);
      addPort(base + ParametricEq::kBandFreq, (band + " freq").c_str(),
              ParametricEq::kMinHz, ParametricEq::kMaxHz,
              ParametricEq::kDefaultBandHz[b]);
      addPort(base + ParametricEq::kBandGainDb, (band + " gain").c_str(),
              ParametricEq::kMinGainDb, ParametricEq::kMaxGainDb, 0.f);
      addPort(base + ParametricEq::kBandQ, (band + " Q").c_str(),
              ParametricEq::kMinQ, ParametricEq::kMaxQ, 1.f);
    }
    return i;
  }();
  return info;
}

// The sidechain compressor's catalog entry (Comp19). Mirrors the Compressor DSP
// port layout so the UI builds sliders and the publish path instantiates from
// the same parameter vector.
inline const FxPluginInfo& builtinCompInfo() {
  static const FxPluginInfo info = [] {
    FxPluginInfo i;
    i.uri = kBuiltinCompUri;
    i.name = "SC Compressor";
    i.author = "Misuc";
    i.audioIns = 1;
    i.audioOuts = 1;
    i.ctrlOuts = 0;

    using dsp::Compressor;
    auto addPort = [&i](uint32_t port, const char* label, float min, float max,
                        float def) {
      i.params.push_back(FxParamInfo{port, label, min, max, def});
    };
    addPort(Compressor::kThresholdDbPort, "Threshold", Compressor::kMinThresholdDb,
            Compressor::kMaxThresholdDb, -24.f);
    addPort(Compressor::kRatioPort, "Ratio", Compressor::kMinRatio,
            Compressor::kMaxRatio, 4.f);
    addPort(Compressor::kAttackMsPort, "Attack", Compressor::kMinAttackMs,
            Compressor::kMaxAttackMs, 5.f);
    addPort(Compressor::kReleaseMsPort, "Release", Compressor::kMinReleaseMs,
            Compressor::kMaxReleaseMs, 200.f);
    addPort(Compressor::kMakeupDbPort, "Makeup", Compressor::kMinMakeupDb,
            Compressor::kMaxMakeupDb, 0.f);
    return i;
  }();
  return info;
}

} // namespace misuc::engine