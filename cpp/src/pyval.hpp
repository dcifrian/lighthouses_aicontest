// Python-compatible values, JSON decoding and repr() formatting.
//
// The official engine is written in Python and some of its output depends on
// Python's exact behaviour: json.loads() accepting NaN/Infinity and big ints,
// the exact text of JSONDecodeError/UnicodeDecodeError messages, and repr()
// of strings, floats, lists and exceptions. This module reproduces CPython
// 3.10 for everything a bot can send.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace py {

using Str = std::u32string;

struct Value {
    enum class T { None, Bool, Int, Float, Str, List, Dict };
    T t = T::None;
    bool b = false;
    int64_t i = 0;          // Int: value saturated to int64 range
    std::string digits;     // Int: repr() of the exact value
    double f = 0;           // Float
    Str s;                  // Str
    std::vector<Value> list;                    // List
    std::vector<std::pair<Str, Value>> dict;    // Dict, insertion order

    bool is_int() const { return t == T::Int || t == T::Bool; }  // isinstance(v, int)
    int64_t as_int() const { return t == T::Bool ? int64_t(b) : i; }
    const Value* get(const char* key) const;  // dict[key] or nullptr
};

// bytes.decode("utf-8"). On failure returns false and sets err to
// repr(UnicodeDecodeError(...)).
bool utf8_decode(const std::string& bytes, Str& out, std::string& err);

// json.loads(doc). On failure returns false and sets err to repr() of the
// exception (normally JSONDecodeError(...)).
bool json_loads(const Str& doc, Value& out, std::string& err);

std::string utf8_encode(const Str& s);
Str from_ascii(const std::string& s);  // ASCII/UTF-8 source text to Str

std::string repr(const Str& s);
std::string repr(const Value& v);
std::string repr_float(double d);
std::string repr_bytes(const std::string& b);
// repr() of an exception constructed with a single string argument.
std::string exc_repr(const char* cls, const std::string& msg_utf8);

// Append a JSON string literal as json.dumps(ensure_ascii=True) writes it.
void json_dump_str(std::string& out, const Str& s);

}  // namespace py
