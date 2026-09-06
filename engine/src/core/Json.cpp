#include <misuc/engine/core/Json.h>

#include <cctype>
#include <cstdio>
#include <sstream>

namespace misuc::core {

namespace {

class Parser {
public:
  explicit Parser(const std::string& text) : s_(text) {}

  Json parse() {
    skipWs();
    Json v = parseValue();
    skipWs();
    if (pos_ != s_.size())
      throw std::runtime_error("Trailing characters in JSON");
    return v;
  }

private:
  void skipWs() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
      ++pos_;
  }

  char peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }

  bool consume(char c) {
    if (peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  Json parseValue() {
    skipWs();
    switch (peek()) {
      case '{':
        return parseObject();
      case '[':
        return parseArray();
      case '"':
        return Json(parseString());
      case 't':
        expect("true");
        return Json(true);
      case 'f':
        expect("false");
        return Json(false);
      case 'n':
        expect("null");
        return Json(nullptr);
      default:
        return Json(parseNumber());
    }
  }

  void expect(const char* word) {
    for (const char* p = word; *p; ++p) {
      if (peek() != *p)
        throw std::runtime_error("Unexpected token in JSON");
      ++pos_;
    }
  }

  Json parseObject() {
    Json::Object obj;
    ++pos_; // {
    skipWs();
    if (consume('}'))
      return Json(std::move(obj));
    while (true) {
      skipWs();
      if (peek() != '"')
        throw std::runtime_error("Expected object key");
      std::string key = parseString();
      skipWs();
      if (!consume(':'))
        throw std::runtime_error("Expected ':'");
      Json v = parseValue();
      obj.emplace(std::move(key), std::move(v));
      skipWs();
      if (consume('}'))
        break;
      if (!consume(','))
        throw std::runtime_error("Expected ',' in object");
    }
    return Json(std::move(obj));
  }

  Json parseArray() {
    Json::Array arr;
    ++pos_; // [
    skipWs();
    if (consume(']'))
      return Json(std::move(arr));
    while (true) {
      arr.push_back(parseValue());
      skipWs();
      if (consume(']'))
        break;
      if (!consume(','))
        throw std::runtime_error("Expected ',' in array");
    }
    return Json(std::move(arr));
  }

  std::string parseString() {
    ++pos_; // opening quote
    std::string out;
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"')
        return out;
      if (c == '\\') {
        if (pos_ >= s_.size())
          break;
        const char e = s_[pos_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          default: out += e; break;
        }
      } else {
        out += c;
      }
    }
    throw std::runtime_error("Unterminated string in JSON");
  }

  double parseNumber() {
    const std::size_t start = pos_;
    if (consume('-')) {}
    while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
           peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-')
      ++pos_;
    if (pos_ == start)
      throw std::runtime_error("Invalid number in JSON");
    return std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
  }

  const std::string& s_;
  std::size_t pos_ = 0;
};

} // namespace

Json Json::parse(const std::string& text) {
  // Support optional UTF-8 BOM.
  std::size_t start = 0;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    start = 3;
  return Parser(text.substr(start)).parse();
}

void Json::write(std::string& out) const {
  if (const auto* b = std::get_if<bool>(&value_)) {
    out += *b ? "true" : "false";
  } else if (const auto* d = std::get_if<double>(&value_)) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9f", *d);
    std::string num(buf);
    while (num.size() > 1 && num.back() == '0')
      num.pop_back();
    if (!num.empty() && num.back() == '.')
      num.pop_back();
    if (num == "-0")
      num = "0";
    out += num;
  } else if (const auto* str = std::get_if<std::string>(&value_)) {
    out += '"';
    for (const char c : *str) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
          } else {
            out += c;
          }
      }
    }
    out += '"';
  } else if (const auto* arr = std::get_if<Array>(&value_)) {
    out += '[';
    bool first = true;
    for (const Json& v : *arr) {
      if (!first) out += ',';
      first = false;
      v.write(out);
    }
    out += ']';
  } else if (const auto* obj = std::get_if<Object>(&value_)) {
    out += '{';
    bool first = true;
    for (const auto& [key, v] : *obj) {
      if (!first) out += ',';
      first = false;
      Json(key).write(out);
      out += ':';
      v.write(out);
    }
    out += '}';
  } else {
    out += "null";
  }
}

} // namespace misuc::core