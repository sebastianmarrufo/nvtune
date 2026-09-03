// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

#include "nvtune/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace nvtune::json {

const std::string& Value::as_string() const {
    if (type_ != Type::String) throw ParseError("value is not a string");
    return str_;
}
double Value::as_number() const {
    if (type_ != Type::Number) throw ParseError("value is not a number");
    return num_;
}
bool Value::as_bool() const {
    if (type_ != Type::Bool) throw ParseError("value is not a bool");
    return bool_;
}
const Array& Value::as_array() const {
    if (type_ != Type::Arr) throw ParseError("value is not an array");
    return arr_;
}
const Object& Value::as_object() const {
    if (type_ != Type::Obj) throw ParseError("value is not an object");
    return obj_;
}
Object& Value::object_ref() {
    if (type_ != Type::Obj) { type_ = Type::Obj; }
    return obj_;
}
Array& Value::array_ref() {
    if (type_ != Type::Arr) { type_ = Type::Arr; }
    return arr_;
}
const Value& Value::at(const std::string& key) const {
    const Value* v = get(key);
    if (v == nullptr) throw ParseError("missing key '" + key + "'");
    return *v;
}
const Value* Value::get(const std::string& key) const noexcept {
    if (type_ != Type::Obj) return nullptr;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &it->second;
}
bool Value::contains(const std::string& key) const noexcept {
    return get(key) != nullptr;
}
std::string Value::str_or(const std::string& key,
                          const std::string& def) const {
    const Value* v = get(key);
    if (v == nullptr || !v->is_string()) return def;
    return v->as_string();
}

namespace {

void escape(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void pad(std::string& out, int indent, int depth) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

}  // namespace

void Value::dump_to(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                out += std::to_string(static_cast<long long>(num_));
            } else {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", num_);
                out += buf;
            }
            break;
        }
        case Type::String: escape(str_, out); break;
        case Type::Arr: {
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            bool first = true;
            for (const Value& v : arr_) {
                if (!first) out += ',';
                first = false;
                pad(out, indent, depth + 1);
                v.dump_to(out, indent, depth + 1);
            }
            pad(out, indent, depth);
            out += ']';
            break;
        }
        case Type::Obj: {
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) out += ',';
                first = false;
                pad(out, indent, depth + 1);
                escape(kv.first, out);
                out += ": ";
                kv.second.dump_to(out, indent, depth + 1);
            }
            pad(out, indent, depth);
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- parser

namespace {

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Value parse() {
        skip();
        Value v = value();
        skip();
        if (i_ != s_.size()) fail("trailing data");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) {
        throw ParseError("JSON at offset " + std::to_string(i_) + ": " + msg);
    }
    void skip() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' ||
                s_[i_] == '\r')) {
            ++i_;
        }
    }
    char peek() {
        if (i_ >= s_.size()) fail("unexpected end of input");
        return s_[i_];
    }
    bool literal(const char* lit) {
        std::size_t n = std::char_traits<char>::length(lit);
        if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
        return false;
    }

    Value value() {
        switch (peek()) {
            case '{': return object();
            case '[': return array();
            case '"': return Value(string());
            case 't': if (literal("true"))  return Value(true);  fail("bad literal");
            case 'f': if (literal("false")) return Value(false); fail("bad literal");
            case 'n': if (literal("null"))  return Value();      fail("bad literal");
            default:  return number();
        }
    }

    Value object() {
        ++i_;  // {
        Object o;
        skip();
        if (peek() == '}') { ++i_; return Value(std::move(o)); }
        for (;;) {
            skip();
            if (peek() != '"') fail("expected object key");
            std::string k = string();
            skip();
            if (peek() != ':') fail("expected ':'");
            ++i_;
            skip();
            o.emplace(std::move(k), value());
            skip();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == '}') { ++i_; break; }
            fail("expected ',' or '}'");
        }
        return Value(std::move(o));
    }

    Value array() {
        ++i_;  // [
        Array a;
        skip();
        if (peek() == ']') { ++i_; return Value(std::move(a)); }
        for (;;) {
            skip();
            a.push_back(value());
            skip();
            char c = peek();
            if (c == ',') { ++i_; continue; }
            if (c == ']') { ++i_; break; }
            fail("expected ',' or ']'");
        }
        return Value(std::move(a));
    }

    std::string string() {
        ++i_;  // opening quote
        std::string out;
        for (;;) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail("unterminated escape");
            char e = s_[i_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) fail("short \\u escape");
                    unsigned cp = static_cast<unsigned>(
                        std::stoul(s_.substr(i_, 4), nullptr, 16));
                    i_ += 4;
                    // Minimal UTF-8 encode; surrogate pairs are not needed for
                    // anything this tool reads or writes.
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
            }
        }
        return out;
    }

    Value number() {
        std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[i_])) ||
                s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' ||
                s_[i_] == '+' || s_[i_] == '-')) {
            ++i_;
        }
        if (i_ == start) fail("expected a value");
        try {
            return Value(std::stod(s_.substr(start, i_ - start)));
        } catch (...) {
            fail("bad number");
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
};

}  // namespace

Value parse(const std::string& text) { return Parser(text).parse(); }

Value parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ParseError("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

void write_file_atomic(const std::string& path, const Value& v) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw ParseError("cannot write " + tmp);
        f << v.dump(2) << "\n";
        if (!f) throw ParseError("write failed: " + tmp);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw ParseError("cannot replace " + path);
    }
}

std::uint32_t as_u32(const Value& v) {
    if (v.is_number()) {
        double d = v.as_number();
        if (d < 0 || d > 4294967295.0) throw ParseError("value out of u32 range");
        return static_cast<std::uint32_t>(d);
    }
    if (v.is_string()) {
        const std::string& s = v.as_string();
        try {
            return static_cast<std::uint32_t>(std::stoul(s, nullptr, 0));
        } catch (...) {
            throw ParseError("'" + s + "' is not an integer");
        }
    }
    throw ParseError("expected a number or numeric string");
}

}  // namespace nvtune::json
