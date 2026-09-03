// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sebastian Marrufo

// A deliberately small JSON subset, so the tool has zero dependencies.
//
// Enough to read the backup format and profile files this tool writes, plus
// hand-authored profiles: objects, arrays, strings, numbers, bools, null.
// No streaming, no comments, no fancy number formats.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nvtune::json {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& w) : std::runtime_error(w) {}
};

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

class Value {
public:
    // Enumerators are abbreviated so they do not shadow the Array/Object
    // aliases declared just above.
    enum class Type { Null, Bool, Number, String, Arr, Obj };

    Value() : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n) : type_(Type::Number), num_(n) {}
    Value(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(Array a) : type_(Type::Arr), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Obj), obj_(std::move(o)) {}

    Type type() const noexcept { return type_; }
    bool is_null()   const noexcept { return type_ == Type::Null; }
    bool is_object() const noexcept { return type_ == Type::Obj; }
    bool is_array()  const noexcept { return type_ == Type::Arr; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_number() const noexcept { return type_ == Type::Number; }

    const std::string& as_string() const;
    double             as_number() const;
    bool               as_bool()   const;
    const Array&       as_array()  const;
    const Object&      as_object() const;

    // Object accessors. `at` throws, `get` returns nullptr.
    const Value& at(const std::string& key) const;
    const Value* get(const std::string& key) const noexcept;
    bool contains(const std::string& key) const noexcept;

    // Convenience: string value or a default.
    std::string str_or(const std::string& key, const std::string& def) const;

    Object& object_ref();
    Array&  array_ref();

    std::string dump(int indent = 2) const;

private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type        type_;
    bool        bool_ = false;
    double      num_ = 0;
    std::string str_;
    Array       arr_;
    Object      obj_;
};

Value parse(const std::string& text);
Value parse_file(const std::string& path);
void  write_file_atomic(const std::string& path, const Value& v);

// Parses "0x1234" or "4660" out of a JSON string or number.
std::uint32_t as_u32(const Value& v);

}  // namespace nvtune::json
