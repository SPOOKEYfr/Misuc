#pragma once

#include <cstdint>
#include <string>

#include <misuc/engine/core/Json.h>

namespace misuc::project {

// Versioned project document. The .misuc format is JSON with an explicit
// schema version so future migrations are possible. This is the foundation of
// the session model; tracks/clips arrive in later milestones.
class Session {
public:
  Session();

  std::string name() const { return name_; }
  void setName(std::string name) { name_ = std::move(name); }

  double tempo() const { return tempo_; }
  void setTempo(double tempo) { tempo_ = tempo; }

  int timeSigNumerator() const { return tsNum_; }
  int timeSigDenominator() const { return tsDen_; }
  void setTimeSignature(int num, int den) {
    tsNum_ = num;
    tsDen_ = den;
  }

  uint32_t sampleRate() const { return sampleRate_; }
  void setSampleRate(uint32_t sr) { sampleRate_ = sr; }

  // Serialize to .misuc text.
  std::string toJson() const;

  // Load from .misuc text. Throws std::runtime_error on malformed input.
  static Session fromJson(const std::string& text);

private:
  std::string name_ = "Untitled";
  double tempo_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;
  uint32_t sampleRate_ = 48000;
};

} // namespace misuc::project