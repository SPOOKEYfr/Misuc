#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace misuc::core {

// Minimal JSON document model + serializer covering the .misuc project format.
// Not a general-purpose JSON library; supports objects, arrays, strings,
// numbers, booleans and null.
class Json {
public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json, std::less<>>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Json() : value_(nullptr) {}
  Json(std::nullptr_t) : value_(nullptr) {}
  Json(bool b) : value_(b) {}
  Json(double d) : value_(d) {}
  Json(int i) : value_(static_cast<double>(i)) {}
  Json(std::string s) : value_(std::move(s)) {}
  Json(const char* s) : value_(std::string(s)) {}
  Json(Array a) : value_(std::move(a)) {}
  Json(Object o) : value_(std::move(o)) {}

  bool isObject() const { return std::holds_alternative<Object>(value_); }
  bool isArray() const { return std::holds_alternative<Array>(value_); }
  bool isString() const { return std::holds_alternative<std::string>(value_); }
  bool isNumber() const { return std::holds_alternative<double>(value_); }
  bool isBool() const { return std::holds_alternative<bool>(value_); }
  bool isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }

  const Object& object() const { return std::get<Object>(value_); }
  const Array& array() const { return std::get<Array>(value_); }
  const std::string& string() const { return std::get<std::string>(value_); }
  double number() const { return std::get<double>(value_); }
  bool boolean() const { return std::get<bool>(value_); }

  const Json& at(const char* key) const {
    const Object& o = object();
    auto it = o.find(key);
    if (it == o.end())
      throw std::runtime_error(std::string("missing key: ") + key);
    return it->second;
  }

  // Serialize to a compact JSON string.
  std::string dump() const {
    std::string out;
    write(out);
    return out;
  }

  static Json parse(const std::string& text);

  // Current .misuc schema version (v2 adds tracks with MIDI notes; audio
  // clips under each track are a compatible additive extension).
  static constexpr int kSchemaVersion = 2;

private:
  void write(std::string& out) const;
  Value value_;
};

} // namespace misuc::core