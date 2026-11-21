#pragma once
// Minimal JSON helpers: a streaming writer (deterministic formatting) and a flat config
// parser for {"key": number|string|bool} objects. The core needs no general JSON reader.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace capturekit::json {

inline std::string escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += c;
        }
    }
  }
  return o;
}

/// Shortest round-trip representation; NaN/Inf become null (JSON has no NaN).
inline std::string num(double v) {
  if (!std::isfinite(v)) return "null";
  char b[32];
  auto r = std::to_chars(b, b + sizeof b, v);
  std::string s(b, r.ptr);
  return s;
}
inline std::string num(int64_t v) { return std::to_string(v); }
inline std::string num(uint64_t v) { return std::to_string(v); }
inline std::string num(int v) { return std::to_string(v); }
inline std::string str(std::string_view s) { return "\"" + escape(s) + "\""; }

/// Builds a JSON object with keys emitted in sorted order (deterministic reports).
class Object {
 public:
  Object& raw(const std::string& k, std::string v) { kv_[k] = std::move(v); return *this; }
  Object& set(const std::string& k, double v) { return raw(k, num(v)); }
  Object& set(const std::string& k, int64_t v) { return raw(k, num(v)); }
  Object& set(const std::string& k, uint64_t v) { return raw(k, num(v)); }
  Object& set(const std::string& k, int v) { return raw(k, num(v)); }
  Object& set(const std::string& k, bool v) { return raw(k, v ? "true" : "false"); }
  Object& set(const std::string& k, std::string_view v) { return raw(k, str(v)); }
  Object& set(const std::string& k, const char* v) { return raw(k, str(v)); }
  Object& set(const std::string& k, const Object& v) { return raw(k, v.dump()); }
  std::string dump() const {
    std::string o = "{";
    bool first = true;
    for (auto& [k, v] : kv_) {
      if (!first) o += ",";
      first = false;
      o += str(k) + ":" + v;
    }
    return o + "}";
  }

 private:
  std::map<std::string, std::string, std::less<>> kv_;
};

/// JSON array from already-serialised elements.
template <class It>
std::string array(It begin, It end) {
  std::string o = "[";
  for (auto it = begin; it != end; ++it) o += (it == begin ? "" : ",") + *it;
  return o + "]";
}

using FlatValue = std::variant<double, std::string, bool>;
using FlatObject = std::map<std::string, FlatValue, std::less<>>;

/// Parses a flat JSON object. Nested objects/arrays are rejected (returns nullopt).
std::optional<FlatObject> parse_flat(std::string_view text, std::string* error = nullptr);

}  // namespace capturekit::json
