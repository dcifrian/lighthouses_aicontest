#include "pyval.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace py {

namespace {

const char32_t kNonPrintable[][2] = {
#include "unicode_printable.inc"
};

bool is_printable(char32_t c) {
    size_t lo = 0, hi = sizeof(kNonPrintable) / sizeof(kNonPrintable[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < kNonPrintable[mid][0])
            hi = mid;
        else if (c > kNonPrintable[mid][1])
            lo = mid + 1;
        else
            return false;
    }
    return true;
}

const char* kHex = "0123456789abcdef";

void append_hex(std::string& out, uint32_t v, int digits) {
    for (int k = digits - 1; k >= 0; k--) out += kHex[(v >> (4 * k)) & 0xF];
}

void append_utf8(std::string& out, char32_t c) {
    // Lone surrogates are encoded like Python's "surrogatepass".
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// ---------------------------------------------------------------- JSON --

struct DecodeError {
    std::string repr;
};
struct StopIteration {
    size_t idx;
};

bool is_ws(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char32_t c) { return c >= '0' && c <= '9'; }

class Decoder {
public:
    explicit Decoder(const Str& s) : s_(s), len_(s.size()) {}

    [[noreturn]] void raise(const char* msg, size_t pos) {
        // JSONDecodeError.__init__
        size_t lineno = 1 + std::count(s_.begin(), s_.begin() + std::min(pos, len_), U'\n');
        size_t last_nl = std::string::npos;
        for (size_t k = std::min(pos, len_); k > 0; k--)
            if (s_[k - 1] == U'\n') {
                last_nl = k - 1;
                break;
            }
        long long colno = last_nl == std::string::npos ? (long long)pos + 1 : (long long)(pos - last_nl);
        char buf[96];
        snprintf(buf, sizeof buf, ": line %zu column %lld (char %zu)", lineno, colno, pos);
        throw DecodeError{exc_repr("JSONDecodeError", std::string(msg) + buf)};
    }

    void decode(Value& out) {
        if (len_ > 0 && s_[0] == 0xFEFF) raise("Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
        size_t idx = skip_ws(0);
        size_t end;
        try {
            end = scan_once(idx, out, 0);
        } catch (const StopIteration& e) {
            raise("Expecting value", e.idx);
        }
        end = skip_ws(end);
        if (end != len_) raise("Extra data", end);
    }

private:
    size_t skip_ws(size_t i) const {
        while (i < len_ && is_ws(s_[i])) i++;
        return i;
    }

    bool match(size_t idx, const char* lit) const {
        size_t n = strlen(lit);
        if (idx + n > len_) return false;
        for (size_t k = 0; k < n; k++)
            if (s_[idx + k] != char32_t((unsigned char)lit[k])) return false;
        return true;
    }

    size_t scan_once(size_t idx, Value& out, int depth) {
        if (idx >= len_) throw StopIteration{idx};
        switch (s_[idx]) {
            case '"':
                out.t = Value::T::Str;
                return scanstring(idx + 1, out.s);
            case '{':
                check_depth(depth, "object");
                return parse_object(idx + 1, out, depth + 1);
            case '[':
                check_depth(depth, "array");
                return parse_array(idx + 1, out, depth + 1);
            case 'n':
                if (match(idx, "null")) {
                    out.t = Value::T::None;
                    return idx + 4;
                }
                break;
            case 't':
                if (match(idx, "true")) {
                    out.t = Value::T::Bool;
                    out.b = true;
                    return idx + 4;
                }
                break;
            case 'f':
                if (match(idx, "false")) {
                    out.t = Value::T::Bool;
                    out.b = false;
                    return idx + 5;
                }
                break;
            case 'N':
                if (match(idx, "NaN")) {
                    out.t = Value::T::Float;
                    out.f = std::nan("");
                    return idx + 3;
                }
                break;
            case 'I':
                if (match(idx, "Infinity")) {
                    out.t = Value::T::Float;
                    out.f = HUGE_VAL;
                    return idx + 8;
                }
                break;
            case '-':
                if (match(idx, "-Infinity")) {
                    out.t = Value::T::Float;
                    out.f = -HUGE_VAL;
                    return idx + 9;
                }
                break;
        }
        return match_number(idx, out);
    }

    void check_depth(int depth, const char* what) {
        // CPython raises RecursionError at the interpreter recursion limit;
        // the exact depth depends on the Python stack, so this is approximate.
        if (depth >= 990)
            throw DecodeError{exc_repr("RecursionError",
                                       std::string("maximum recursion depth exceeded while decoding a JSON ") +
                                           what + " from a unicode string")};
    }

    size_t parse_object(size_t idx, Value& out, int depth) {
        out.t = Value::T::Dict;
        size_t end_idx = len_ - 1;  // only used when len_ > 0 (we are after '{')
        idx = skip_ws(idx);
        if (idx > end_idx || s_[idx] != '}') {
            while (true) {
                if (idx > end_idx || s_[idx] != '"')
                    raise("Expecting property name enclosed in double quotes", idx);
                Str key;
                idx = scanstring(idx + 1, key);
                idx = skip_ws(idx);
                if (idx > end_idx || s_[idx] != ':') raise("Expecting ':' delimiter", idx);
                idx = skip_ws(idx + 1);
                Value val;
                idx = scan_once(idx, val, depth);
                auto it = std::find_if(out.dict.begin(), out.dict.end(),
                                       [&](const auto& kv) { return kv.first == key; });
                if (it != out.dict.end())
                    it->second = std::move(val);
                else
                    out.dict.emplace_back(std::move(key), std::move(val));
                idx = skip_ws(idx);
                if (idx <= end_idx && s_[idx] == '}') break;
                if (idx > end_idx || s_[idx] != ',') raise("Expecting ',' delimiter", idx);
                idx = skip_ws(idx + 1);
            }
        }
        return idx + 1;
    }

    size_t parse_array(size_t idx, Value& out, int depth) {
        out.t = Value::T::List;
        size_t end_idx = len_ - 1;
        idx = skip_ws(idx);
        if (idx > end_idx || s_[idx] != ']') {
            while (true) {
                out.list.emplace_back();
                idx = scan_once(idx, out.list.back(), depth);
                idx = skip_ws(idx);
                if (idx <= end_idx && s_[idx] == ']') break;
                if (idx > end_idx || s_[idx] != ',') raise("Expecting ',' delimiter", idx);
                idx = skip_ws(idx + 1);
            }
        }
        if (idx > end_idx || s_[idx] != ']') raise("Expecting value", end_idx);
        return idx + 1;
    }

    size_t match_number(size_t start, Value& out) {
        size_t end_idx = len_ - 1;  // idx < len_ is guaranteed by scan_once
        size_t idx = start;
        bool is_float = false;
        if (s_[idx] == '-') {
            idx++;
            if (idx > end_idx) throw StopIteration{start};
        }
        if (s_[idx] >= '1' && s_[idx] <= '9') {
            idx++;
            while (idx <= end_idx && is_digit(s_[idx])) idx++;
        } else if (s_[idx] == '0') {
            idx++;
        } else {
            throw StopIteration{start};
        }
        if (idx < end_idx && s_[idx] == '.' && is_digit(s_[idx + 1])) {
            is_float = true;
            idx += 2;
            while (idx <= end_idx && is_digit(s_[idx])) idx++;
        }
        if (idx < end_idx && (s_[idx] == 'e' || s_[idx] == 'E')) {
            size_t e_start = idx;
            idx++;
            if (idx < end_idx && (s_[idx] == '-' || s_[idx] == '+')) idx++;
            while (idx <= end_idx && is_digit(s_[idx])) idx++;
            if (is_digit(s_[idx - 1]))
                is_float = true;
            else
                idx = e_start;
        }
        std::string num;
        num.reserve(idx - start);
        for (size_t k = start; k < idx; k++) num += char(s_[k]);
        if (is_float) {
            out.t = Value::T::Float;
            out.f = strtod(num.c_str(), nullptr);
        } else {
            out.t = Value::T::Int;
            bool neg = num[0] == '-';
            std::string mag = num.substr(neg ? 1 : 0);
            if (mag.size() > 4300) {
                char buf[200];
                snprintf(buf, sizeof buf,
                         "Exceeds the limit (4300) for integer string conversion: value has %zu digits; "
                         "use sys.set_int_max_str_digits() to increase the limit",
                         mag.size());
                throw DecodeError{exc_repr("ValueError", buf)};
            }
            out.digits = (neg && mag != "0") ? "-" + mag : mag;
            // Saturate: anything beyond int64 compares like a huge value.
            if (mag.size() > 18) {
                out.i = neg ? INT64_MIN : INT64_MAX;
            } else {
                out.i = std::stoll(mag);
                if (neg) out.i = -out.i;
            }
        }
        return idx;
    }

    int hexval(char32_t d) const {
        if (d >= '0' && d <= '9') return int(d - '0');
        if (d >= 'a' && d <= 'f') return int(d - 'a' + 10);
        if (d >= 'A' && d <= 'F') return int(d - 'A' + 10);
        return -1;
    }

    // _json.c scanstring_unicode, strict mode. `end` is the index after the
    // opening quote; returns the index after the closing quote.
    size_t scanstring(size_t end, Str& out) {
        size_t begin = end - 1;
        size_t next;
        while (true) {
            char32_t c = 0;
            for (next = end; next < len_; next++) {
                c = s_[next];
                if (c == '"' || c == '\\') break;
                if (c <= 0x1f) raise("Invalid control character at", next);
            }
            if (next >= len_ || (c != '"' && c != '\\')) raise("Unterminated string starting at", begin);
            out.append(s_, end, next - end);
            next++;
            if (c == '"') return next;
            if (next == len_) raise("Unterminated string starting at", begin);
            c = s_[next];
            if (c != 'u') {
                end = next + 1;
                switch (c) {
                    case '"': case '\\': case '/': break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    default: raise("Invalid \\escape", end - 2);
                }
            } else {
                c = 0;
                next++;
                end = next + 4;
                if (end >= len_) raise("Invalid \\uXXXX escape", next - 1);
                for (; next < end; next++) {
                    int h = hexval(s_[next]);
                    if (h < 0) raise("Invalid \\uXXXX escape", end - 5);
                    c = (c << 4) | char32_t(h);
                }
                if (c >= 0xD800 && c <= 0xDBFF && end + 6 < len_ && s_[next++] == '\\' && s_[next++] == 'u') {
                    char32_t c2 = 0;
                    end += 6;
                    for (; next < end; next++) {
                        int h = hexval(s_[next]);
                        if (h < 0) raise("Invalid \\uXXXX escape", end - 5);
                        c2 = (c2 << 4) | char32_t(h);
                    }
                    if (c2 >= 0xDC00 && c2 <= 0xDFFF)
                        c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
                    else
                        end -= 6;
                }
            }
            out += c;
        }
    }

    const Str& s_;
    size_t len_;
};

}  // namespace

const Value* Value::get(const char* key) const {
    if (t != T::Dict) return nullptr;
    Str k = from_ascii(key);
    for (const auto& kv : dict)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

bool json_loads(const Str& doc, Value& out, std::string& err) {
    try {
        Decoder(doc).decode(out);
        return true;
    } catch (const DecodeError& e) {
        err = e.repr;
        return false;
    }
}

bool utf8_decode(const std::string& bytes, Str& out, std::string& err) {
    out.clear();
    out.reserve(bytes.size());
    const unsigned char* s = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t n = bytes.size(), i = 0;
    auto cont = [](unsigned char c) { return (c & 0xC0) == 0x80; };
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            out += char32_t(c);
            i++;
            continue;
        }
        size_t avail = n - i;
        const char* reason = nullptr;
        size_t bad_end = 0;
        if (c < 0xC2 || c > 0xF4) {
            reason = "invalid start byte";
            bad_end = i + 1;
        } else if (c < 0xE0) {
            if (avail < 2) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (!cont(s[i + 1])) {
                reason = "invalid continuation byte";
                bad_end = i + 1;
            } else {
                out += char32_t(((c & 0x1F) << 6) | (s[i + 1] & 0x3F));
                i += 2;
                continue;
            }
        } else if (c < 0xF0) {
            auto bad2 = [&](unsigned char c2) {
                return !cont(c2) || (c2 < 0xA0 ? c == 0xE0 : c == 0xED);
            };
            if (avail < 2) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (bad2(s[i + 1])) {
                reason = "invalid continuation byte";
                bad_end = i + 1;
            } else if (avail < 3) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (!cont(s[i + 2])) {
                reason = "invalid continuation byte";
                bad_end = i + 2;
            } else {
                out += char32_t(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F));
                i += 3;
                continue;
            }
        } else {
            auto bad2 = [&](unsigned char c2) {
                return !cont(c2) || (c2 < 0x90 ? c == 0xF0 : c == 0xF4);
            };
            if (avail < 2) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (bad2(s[i + 1])) {
                reason = "invalid continuation byte";
                bad_end = i + 1;
            } else if (avail < 3) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (!cont(s[i + 2])) {
                reason = "invalid continuation byte";
                bad_end = i + 2;
            } else if (avail < 4) {
                reason = "unexpected end of data";
                bad_end = n;
            } else if (!cont(s[i + 3])) {
                reason = "invalid continuation byte";
                bad_end = i + 3;
            } else {
                out += char32_t(((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) |
                                (s[i + 3] & 0x3F));
                i += 4;
                continue;
            }
        }
        err = "UnicodeDecodeError('utf-8', " + repr_bytes(bytes) + ", " + std::to_string(i) + ", " +
              std::to_string(bad_end) + ", '" + reason + "')";
        return false;
    }
    return true;
}

std::string utf8_encode(const Str& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) append_utf8(out, c);
    return out;
}

Str from_ascii(const std::string& s) {
    Str out;
    std::string err;
    if (!utf8_decode(s, out, err)) out.assign(s.begin(), s.end());
    return out;
}

std::string repr(const Str& s) {
    bool has_sq = s.find(U'\'') != Str::npos, has_dq = s.find(U'"') != Str::npos;
    char32_t quote = (has_sq && !has_dq) ? U'"' : U'\'';
    std::string out;
    out.reserve(s.size() + 2);
    out += char(quote);
    for (char32_t c : s) {
        if (c == quote || c == U'\\') {
            out += '\\';
            out += char(c);
        } else if (c == U'\t') {
            out += "\\t";
        } else if (c == U'\n') {
            out += "\\n";
        } else if (c == U'\r') {
            out += "\\r";
        } else if (c < U' ' || c == 0x7F) {
            out += "\\x";
            append_hex(out, c, 2);
        } else if (c < 0x7F) {
            out += char(c);
        } else if (is_printable(c)) {
            append_utf8(out, c);
        } else if (c <= 0xFF) {
            out += "\\x";
            append_hex(out, c, 2);
        } else if (c <= 0xFFFF) {
            out += "\\u";
            append_hex(out, c, 4);
        } else {
            out += "\\U";
            append_hex(out, c, 8);
        }
    }
    out += char(quote);
    return out;
}

std::string repr_bytes(const std::string& b) {
    bool has_sq = b.find('\'') != std::string::npos, has_dq = b.find('"') != std::string::npos;
    char quote = (has_sq && !has_dq) ? '"' : '\'';
    std::string out = "b";
    out += quote;
    for (unsigned char c : b) {
        if (c == quote || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < ' ' || c >= 0x7F) {
            out += "\\x";
            append_hex(out, c, 2);
        } else {
            out += char(c);
        }
    }
    out += quote;
    return out;
}

std::string repr_float(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    // sci looks like "-1.2345e+17": split into sign, digits and exponent.
    std::string sign;
    size_t p = 0;
    if (sci[0] == '-') {
        sign = "-";
        p = 1;
    }
    size_t epos = sci.find('e');
    std::string mant = sci.substr(p, epos - p);
    int exp10 = std::stoi(sci.substr(epos + 1));
    std::string digits;
    for (char c : mant)
        if (c != '.') digits += c;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int decpt = exp10 + 1;  // value = 0.DIGITS * 10^decpt
    std::string out = sign;
    if (decpt <= -4 || decpt > 16) {
        out += digits[0];
        if (digits.size() > 1) {
            out += '.';
            out += digits.substr(1);
        }
        int e = decpt - 1;
        out += 'e';
        out += e < 0 ? '-' : '+';
        std::string es = std::to_string(std::abs(e));
        if (es.size() < 2) es = "0" + es;
        out += es;
    } else if (decpt <= 0) {
        out += "0.";
        out += std::string(size_t(-decpt), '0');
        out += digits;
    } else if (size_t(decpt) >= digits.size()) {
        out += digits;
        out += std::string(size_t(decpt) - digits.size(), '0');
        out += ".0";
    } else {
        out += digits.substr(0, size_t(decpt));
        out += '.';
        out += digits.substr(size_t(decpt));
    }
    return out;
}

std::string repr(const Value& v) {
    switch (v.t) {
        case Value::T::None: return "None";
        case Value::T::Bool: return v.b ? "True" : "False";
        case Value::T::Int: return v.digits;
        case Value::T::Float: return repr_float(v.f);
        case Value::T::Str: return repr(v.s);
        case Value::T::List: {
            std::string out = "[";
            for (size_t k = 0; k < v.list.size(); k++) {
                if (k) out += ", ";
                out += repr(v.list[k]);
            }
            return out + "]";
        }
        case Value::T::Dict: {
            std::string out = "{";
            for (size_t k = 0; k < v.dict.size(); k++) {
                if (k) out += ", ";
                out += repr(v.dict[k].first);
                out += ": ";
                out += repr(v.dict[k].second);
            }
            return out + "}";
        }
    }
    return "";
}

std::string exc_repr(const char* cls, const std::string& msg_utf8) {
    return std::string(cls) + "(" + repr(from_ascii(msg_utf8)) + ")";
}

void json_dump_str(std::string& out, const Str& s) {
    out += '"';
    for (char32_t c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c >= ' ' && c <= '~') {
                    out += char(c);
                } else if (c >= 0x10000) {
                    char32_t v = c - 0x10000;
                    out += "\\u";
                    append_hex(out, 0xD800 | ((v >> 10) & 0x3FF), 4);
                    out += "\\u";
                    append_hex(out, 0xDC00 | (v & 0x3FF), 4);
                } else {
                    out += "\\u";
                    append_hex(out, c, 4);
                }
        }
    }
    out += '"';
}

}  // namespace py
