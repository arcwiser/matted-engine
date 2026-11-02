// Matted Engine - zero-dependency JSON value tree, parser, writer.
//
// TASK-18 (editor-lite) foundation. Used by scene save/load and any tool that
// needs structured files (perf histories, editor layouts). Design notes:
//  - Objects preserve insertion order so saved files are deterministic and
//    diff-friendly (no hash-map ordering noise).
//  - Load/save are one-off operations (never per-frame), so std::string /
//    std::vector storage is fine per engine conventions (no hot-path churn).
//  - No exceptions: parse returns bool + fills an error string with line info.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mtt::json {

class Value {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Value() = default; // Null

    // ------------------------------------------------------------ factories
    static Value null()                        { return Value(); }
    static Value boolean(bool b)               { Value v(Type::Bool); v.num_ = b ? 1.0 : 0.0; return v; }
    static Value number(double d)              { Value v(Type::Number); v.num_ = d; return v; }
    static Value string(std::string s)         { Value v(Type::String); v.str_ = std::move(s); return v; }
    static Value array()                       { return Value(Type::Array); }
    static Value object()                      { return Value(Type::Object); }

    Type        type() const { return type_; }
    bool        is_null() const   { return type_ == Type::Null; }
    bool        is_bool() const   { return type_ == Type::Bool; }
    bool        is_number() const { return type_ == Type::Number; }
    bool        is_string() const { return type_ == Type::String; }
    bool        is_array() const  { return type_ == Type::Array; }
    bool        is_object() const { return type_ == Type::Object; }

    // ------------------------------------------------------------ accessors
    bool        as_bool(bool def = false) const         { return is_bool() ? num_ != 0.0 : def; }
    double      as_number(double def = 0.0) const       { return is_number() ? num_ : def; }
    float       as_float(float def = 0.0f) const        { return (float)as_number((double)def); }
    int         as_int(int def = 0) const               { return (int)as_number((double)def); }
    uint32_t    as_u32(uint32_t def = 0) const          { return (uint32_t)as_number((double)def); }
    const std::string& as_string() const                { return str_; }
    const char* as_cstr() const                          { return str_.c_str(); }

    const std::vector<Value>&  items() const { return arr_; }
    std::vector<Value>&        items()       { return arr_; }
    // Object members in insertion order (deterministic save format).
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

    // Object lookup; returns Value::null() when missing (safe to chain).
    const Value& get(const char* key) const {
        if (type_ == Type::Object)
            for (const auto& [k, v] : obj_)
                if (k == key) return v;
        static const Value empty = null();
        return empty;
    }
    bool has(const char* key) const { return !get(key).is_null(); } // note: explicit nulls read as missing

    // ------------------------------------------------------------ mutation
    void set_bool(bool b)         { type_ = Type::Bool;   num_ = b ? 1.0 : 0.0; }
    void set_number(double d)     { type_ = Type::Number; num_ = d; }
    void set_string(std::string s){ type_ = Type::String; str_ = std::move(s); }

    Value& push(Value v) { type_ = Type::Array; arr_.push_back(std::move(v)); return arr_.back(); }
    // Insert or overwrite by key; keeps original position on overwrite.
    Value& set(std::string key, Value v) {
        type_ = Type::Object;
        for (auto& [k, existing] : obj_)
            if (k == key) { existing = std::move(v); return existing; }
        obj_.emplace_back(std::move(key), std::move(v));
        return obj_.back().second;
    }

private:
    explicit Value(Type t) : type_(t) {}
    Type        type_ = Type::Null;
    double      num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;                                 // Array
    std::vector<std::pair<std::string, Value>> obj_;         // Object, ordered
};

// Parse text[0..len). Returns nullptr-value on failure and fills `err`
// with "line L: message". Accepts trailing whitespace, rejects trailing junk.
bool parse(const char* text, size_t len, Value& out, std::string* err = nullptr);
inline bool parse(const std::string& text, Value& out, std::string* err = nullptr) {
    return parse(text.data(), text.size(), out, err);
}

// Pretty-print with 2-space indent (matches the rest of the repo's file style).
// `compact` emits no whitespace for machine-written history files.
void write(const Value& v, std::string& out, bool compact = false);

} // namespace mtt::json
