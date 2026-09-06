#include "test_utils.h"

#include <misuc/engine/core/Json.h>
#include <misuc/engine/project/Session.h>

using misuc::core::Json;
using misuc::project::Session;

TEST_CASE(json_roundtrip) {
  Json::Object o;
  o.emplace("name", Json("test project"));
  o.emplace("tempo", Json(128.5));
  o.emplace("loop", Json(true));
  o.emplace("null", Json(nullptr));

  Json::Array arr;
  arr.emplace_back(Json("a"));
  arr.emplace_back(Json(2.0));
  o.emplace("tags", Json(std::move(arr)));

  const std::string text = Json(std::move(o)).dump();
  const Json parsed = Json::parse(text);
  TEST_ASSERT(parsed.isObject());
  TEST_ASSERT(parsed.at("name").string() == "test project");
  TEST_ASSERT(parsed.at("tempo").number() == 128.5);
  TEST_ASSERT(parsed.at("loop").boolean());
  TEST_ASSERT(parsed.at("null").isNull());
  TEST_ASSERT(parsed.at("tags").array().size() == 2);
  TEST_ASSERT(parsed.at("tags").array()[1].number() == 2.0);
}

TEST_CASE(json_parse_errors) {
  bool threw = false;
  try {
    Json::parse("{ invalid");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  TEST_ASSERT(threw);
}

TEST_CASE(session_roundtrip) {
  Session s;
  s.setName("My First Track");
  s.setTempo(97.5);
  s.setTimeSignature(3, 4);
  s.setSampleRate(44100);

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.name() == "My First Track");
  TEST_ASSERT(loaded.tempo() == 97.5);
  TEST_ASSERT(loaded.timeSigNumerator() == 3);
  TEST_ASSERT(loaded.timeSigDenominator() == 4);
  TEST_ASSERT(loaded.sampleRate() == 44100);
}

TEST_CASE(session_schema_version) {
  const Session s;
  const Json doc = Json::parse(s.toJson());
  TEST_ASSERT(doc.at("schemaVersion").number() == Json::kSchemaVersion);
  TEST_ASSERT(doc.at("app").string() == "misuc");
}