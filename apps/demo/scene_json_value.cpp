// Matted Engine - JSON parser + writer implementation (see scene_json_value.h).
#include "scene_json_value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mtt::json {
namespace {

// ------------------------------------------------------------------ parsing
struct Cursor {
    const char* p;
    const char* end;
    int line = 1;
};

void skip_ws(Cursor& c) {
    while (c.p < c.end) {
        const char ch = *c.p;
        if (ch == '\n') { ++c.line; ++c.p; }
        else if (ch == ' ' || ch == '\t' || ch == '\r') ++c.p;
        else break;
    }
}

bool fail(Cursor& c, std::string* err, const char* msg) {
    if (err) *err = "line " + std::to_string(c.line) + ": " + msg;
    return false;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

int hex4(Cursor& c) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        if (c.p >= c.end) return -1;
        char ch = *c.p++;
        v <<= 4;
        if (ch >= '0' && ch <= '9')      v |= ch - '0';
        else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
        else return -1;
    }
    return v;
}

bool parse_string(Cursor& c, Value& out, std::string* err) {
    if (c.p >= c.end || *c.p != '"') return fail(c, err, "expected string");
    ++c.p;
    std::string s;
    while (c.p < c.end) {
        const char ch = *c.p++;
        if (ch == '"') { out = Value::string(std::move(s)); return true; }
        if (ch != '\\') {
            if ((uint8_t)ch < 0x20) return fail(c, err, "raw control char in string");
            s.push_back(ch);
            continue;
        }
        if (c.p >= c.end) break;
        const char esc = *c.p++;
        switch (esc) {
            case '"':  s.push_back('"');  break;
            case '\\': s.push_back('\\'); break;
            case '/':  s.push_back('/');  break;
            case 'b':  s.push_back('\b'); break;
            case 'f':  s.push_back('\f'); break;
            case 'n':  s.push_back('\n'); break;
            case 'r':  s.push_back('\r'); break;
            case 't':  s.push_back('\t'); break;
            case 'u': {
                const int cp = hex4(c);
                if (cp < 0) return fail(c, err, "bad \\u escape");
                uint32_t code = (uint32_t)cp;
                // surrogate pair
                if (code >= 0xD800 && code <= 0xDBFF && c.end - c.p >= 6 &&
                    c.p[0] == '\\' && c.p[1] == 'u') {
                    c.p += 2;
                    const int lo = hex4(c);
                    if (lo < 0) return fail(c, err, "bad \\u escape");
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        code = 0x10000 + ((code - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                    else // lone high surrogate: encode replacement char
                        code = 0xFFFD;
                } else if (code >= 0xD800 && code <= 0xDFFF) {
                    code = 0xFFFD; // lone surrogate
                }
                append_utf8(s, code);
                break;
            }
            default: return fail(c, err, "unknown escape");
        }
    }
    return fail(c, err, "unterminated string");
}

bool parse_value(Cursor& c, Value& out, std::string* err);

bool parse_number(Cursor& c, Value& out, std::string* err) {
    const char* start = c.p;
    if (c.p < c.end && *c.p == '-') ++c.p;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    if (c.p < c.end && *c.p == '.') {
        ++c.p;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) {
        ++c.p;
        if (c.p < c.end && (*c.p == '+' || *c.p == '-')) ++c.p;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    if (c.p == start) return fail(c, err, "expected number");
    char buf[64];
    const size_t n = (size_t)(c.p - start);
    if (n >= sizeof(buf)) return fail(c, err, "number too long");
    std::memcpy(buf, start, n);
    buf[n] = '\0';
    char* endp = nullptr;
    const double d = std::strtod(buf, &endp);
    if (endp != buf + n) return fail(c, err, "malformed number");
    out = Value::number(d);
    return true;
}

bool parse_value(Cursor& c, Value& out, std::string* err) {
    skip_ws(c);
    if (c.p >= c.end) return fail(c, err, "unexpected end of input");
    const char ch = *c.p;
    if (ch == '{') {
        ++c.p;
        out = Value::object();
        skip_ws(c);
        if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
        while (true) {
            skip_ws(c);
            Value key;
            if (!parse_string(c, key, err)) return false;
            skip_ws(c);
            if (c.p >= c.end || *c.p != ':') return fail(c, err, "expected ':'");
            ++c.p;
            Value v;
            if (!parse_value(c, v, err)) return false;
            out.set(key.as_string(), std::move(v));
            skip_ws(c);
            if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
            if (c.p < c.end && *c.p == '}') { ++c.p; return true; }
            return fail(c, err, "expected ',' or '}'");
        }
    }
    if (ch == '[') {
        ++c.p;
        out = Value::array();
        skip_ws(c);
        if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
        while (true) {
            Value v;
            if (!parse_value(c, v, err)) return false;
            out.push(std::move(v));
            skip_ws(c);
            if (c.p < c.end && *c.p == ',') { ++c.p; continue; }
            if (c.p < c.end && *c.p == ']') { ++c.p; return true; }
            return fail(c, err, "expected ',' or ']'");
        }
    }
    if (ch == '"') return parse_string(c, out, err);
    if (ch == 't' && c.end - c.p >= 4 && std::memcmp(c.p, "true", 4) == 0) {
        c.p += 4; out = Value::boolean(true); return true;
    }
    if (ch == 'f' && c.end - c.p >= 5 && std::memcmp(c.p, "false", 5) == 0) {
        c.p += 5; out = Value::boolean(false); return true;
    }
    if (ch == 'n' && c.end - c.p >= 4 && std::memcmp(c.p, "null", 4) == 0) {
        c.p += 4; out = Value::null(); return true;
    }
    return parse_number(c, out, err);
}

// ------------------------------------------------------------------ writing
void write_indent(std::string& out, int depth) {
    for (int i = 0; i < depth; ++i) out += "  ";
}

void write_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (uint32_t)(uint8_t)ch);
                    out += buf;
                } else {
                    out.push_back(ch); // UTF-8 bytes pass through
                }
        }
    }
    out.push_back('"');
}

void write_number(std::string& out, double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
        out += buf;
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", d); // enough round-trip for f32/f64 scene data
    out += buf;
}

void write_value(const Value& v, std::string& out, bool compact, int depth) {
    switch (v.type()) {
        case Value::Type::Null:   out += "null"; break;
        case Value::Type::Bool:   out += v.as_bool() ? "true" : "false"; break;
        case Value::Type::Number: write_number(out, v.as_number()); break;
        case Value::Type::String: write_string(out, v.as_string()); break;
        case Value::Type::Array: {
            const auto& items = v.items();
            if (items.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out.push_back(',');
                if (!compact) { out.push_back('\n'); write_indent(out, depth + 1); }
                write_value(items[i], out, compact, depth + 1);
            }
            if (!compact) { out.push_back('\n'); write_indent(out, depth); }
            out.push_back(']');
            break;
        }
        case Value::Type::Object: {
            const auto& members = v.members();
            if (members.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (size_t i = 0; i < members.size(); ++i) {
                if (i) out.push_back(',');
                if (!compact) { out.push_back('\n'); write_indent(out, depth + 1); }
                write_string(out, members[i].first);
                out.push_back(':');
                if (!compact) out.push_back(' ');
                write_value(members[i].second, out, compact, depth + 1);
            }
            if (!compact) { out.push_back('\n'); write_indent(out, depth); }
            out.push_back('}');
            break;
        }
    }
}

} // namespace

// ------------------------------------------------------------------ public
bool parse(const char* text, size_t len, Value& out, std::string* err) {
    Cursor c{text, text + len, 1};
    if (!parse_value(c, out, err)) { out = Value::null(); return false; }
    skip_ws(c);
    if (c.p != c.end) return fail(c, err, "trailing characters after JSON value");
    if (err) err->clear();
    return true;
}

void write(const Value& v, std::string& out, bool compact) {
    write_value(v, out, compact, 0);
}

} // namespace mtt::json
