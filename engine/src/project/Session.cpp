#include <misuc/engine/project/Session.h>

#include <stdexcept>

namespace misuc::project {

using core::Json;

Session::Session() = default;

std::string Session::toJson() const {
  Json::Object root;
  root.emplace("schemaVersion", Json(Json::kSchemaVersion));
  root.emplace("app", Json("misuc"));

  Json::Object meta;
  meta.emplace("name", Json(name_));
  meta.emplace("createdUtc", Json(std::string("")));
  meta.emplace("modifiedUtc", Json(std::string("")));
  root.emplace("metadata", Json(std::move(meta)));

  Json::Object transportObj;
  transportObj.emplace("tempo", Json(tempo_));
  transportObj.emplace("timeSigNumerator", Json(tsNum_));
  transportObj.emplace("timeSigDenominator", Json(tsDen_));
  transportObj.emplace("sampleRate", Json(static_cast<double>(sampleRate_)));
  root.emplace("transport", Json(std::move(transportObj)));

  // Tracks/clips arrive in later milestones.
  root.emplace("tracks", Json(Json::Array{}));

  return Json(std::move(root)).dump();
}

Session Session::fromJson(const std::string& text) {
  const Json doc = Json::parse(text);
  if (!doc.isObject())
    throw std::runtime_error("Project root must be a JSON object");

  Session s;
  const auto& root = doc.object();

  auto it = root.find("transport");
  if (it != root.end() && it->second.isObject()) {
    const auto& t = it->second.object();
    if (auto f = t.find("tempo"); f != t.end() && f->second.isNumber())
      s.setTempo(f->second.number());
    if (auto f = t.find("timeSigNumerator"); f != t.end() && f->second.isNumber())
      s.setTimeSignature(static_cast<int>(f->second.number()), s.timeSigDenominator());
    if (auto f = t.find("timeSigDenominator"); f != t.end() && f->second.isNumber())
      s.setTimeSignature(s.timeSigNumerator(), static_cast<int>(f->second.number()));
    if (auto f = t.find("sampleRate"); f != t.end() && f->second.isNumber())
      s.setSampleRate(static_cast<uint32_t>(f->second.number()));
  }

  if (auto f = root.find("metadata"); f != root.end() && f->second.isObject()) {
    const auto& m = f->second.object();
    if (auto n = m.find("name"); n != m.end() && n->second.isString())
      s.setName(n->second.string());
  }
  return s;
}

} // namespace misuc::project