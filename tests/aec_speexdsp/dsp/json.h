/* Minimal JSON reader (objects, arrays, strings, numbers, bools, null).
 * Enough for the scenario manifests produced by tools/generate_signals.py. */
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json {

struct Value;
using Object = std::map<std::string, std::shared_ptr<Value>>;
using Array = std::vector<std::shared_ptr<Value>>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    Array array;
    Object object;

    bool is_null() const { return type == Type::Null; }
    double num(double fallback = 0.0) const { return type == Type::Number ? number : fallback; }
    const std::string &str() const { return string; }
    bool has(const std::string &key) const { return type == Type::Object && object.count(key) != 0; }
    const Value &at(const std::string &key) const;
    const Value &at(size_t index) const;
};

// Parses `text`; returns nullptr on error (message in *error).
std::shared_ptr<Value> parse(const std::string &text, std::string *error = nullptr);

// Loads a file and parses it.
std::shared_ptr<Value> load(const std::string &path, std::string *error = nullptr);

}  // namespace json
