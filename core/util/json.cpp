#include "capturekit/util/json.hpp"

#include <cctype>

namespace capturekit::json {
namespace {
struct P {
  std::string_view s;
  size_t i = 0;
  std::string err;
  void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
  bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
  bool string(std::string& out) {
    ws();
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\') {
        if (i >= s.size()) return fail("bad escape");
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
          case '"': case '\\': case '/': out += e; break;
          default: return fail("unsupported escape");
        }
      } else out += c;
    }
    if (i >= s.size()) return fail("unterminated string");
    ++i;
    return true;
  }
};
}  // namespace

std::optional<FlatObject> parse_flat(std::string_view text, std::string* error) {
  P p{text};
  FlatObject obj;
  auto bad = [&]() -> std::optional<FlatObject> { if (error) *error = p.err; return std::nullopt; };
  if (!p.eat('{')) { p.fail("expected {"); return bad(); }
  if (p.eat('}')) return obj;
  do {
    std::string key;
    if (!p.string(key)) return bad();
    if (!p.eat(':')) { p.fail("expected :"); return bad(); }
    p.ws();
    if (p.i >= text.size()) { p.fail("unexpected end"); return bad(); }
    char c = text[p.i];
    if (c == '"') {
      std::string v;
      if (!p.string(v)) return bad();
      obj[key] = v;
    } else if (text.substr(p.i, 4) == "true") { obj[key] = true; p.i += 4; }
    else if (text.substr(p.i, 5) == "false") { obj[key] = false; p.i += 5; }
    else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      double v = 0;
      auto r = std::from_chars(text.data() + p.i, text.data() + text.size(), v);
      if (r.ec != std::errc()) { p.fail("bad number"); return bad(); }
      p.i = size_t(r.ptr - text.data());
      obj[key] = v;
    } else { p.fail("unsupported value (flat objects only)"); return bad(); }
  } while (p.eat(','));
  if (!p.eat('}')) { p.fail("expected }"); return bad(); }
  p.ws();
  if (p.i != text.size()) { p.fail("trailing data"); return bad(); }
  return obj;
}

}  // namespace capturekit::json
