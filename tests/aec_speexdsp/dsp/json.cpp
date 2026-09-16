#include "json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace json {

const Value &Value::at(const std::string &key) const {
    static const Value null_value;
    if (type != Type::Object)
        return null_value;
    auto it = object.find(key);
    return it == object.end() ? null_value : *it->second;
}

const Value &Value::at(size_t index) const {
    static const Value null_value;
    if (type != Type::Array || index >= array.size())
        return null_value;
    return *array[index];
}

namespace {

class Parser {
 public:
    Parser(const std::string &text, std::string *error) : text_(text), error_(error) {}

    std::shared_ptr<Value> run() {
        skip_ws();
        auto value = parse_value();
        if (value && (error_ == nullptr || error_->empty())) {
            skip_ws();
            if (pos_ != text_.size()) {
                fail("trailing characters after JSON document");
                return nullptr;
            }
        }
        return value;
    }

 private:
    const std::string &text_;
    std::string *error_;
    size_t pos_ = 0;

    void fail(const std::string &message) {
        if (error_ != nullptr && error_->empty()) {
            std::ostringstream o;
            o << message << " at offset " << pos_;
            *error_ = o.str();
        }
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                pos_++;
            else
                break;
        }
    }

    bool literal(const char *word) {
        const size_t len = std::strlen(word);
        if (text_.compare(pos_, len, word) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    std::shared_ptr<Value> parse_value() {
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return nullptr;
        }
        const char c = text_[pos_];
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == '"')
            return parse_string();
        if (c == 't') {
            if (!literal("true")) {
                fail("invalid literal");
                return nullptr;
            }
            auto v = std::make_shared<Value>();
            v->type = Value::Type::Bool;
            v->boolean = true;
            return v;
        }
        if (c == 'f') {
            if (!literal("false")) {
                fail("invalid literal");
                return nullptr;
            }
            auto v = std::make_shared<Value>();
            v->type = Value::Type::Bool;
            v->boolean = false;
            return v;
        }
        if (literal("null"))
            return std::make_shared<Value>();
        return parse_number();
    }

    std::shared_ptr<Value> parse_object() {
        auto v = std::make_shared<Value>();
        v->type = Value::Type::Object;
        pos_++;  // {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            pos_++;
            return v;
        }
        while (pos_ < text_.size()) {
            skip_ws();
            if (text_[pos_] != '"') {
                fail("expected object key");
                return nullptr;
            }
            auto key = parse_string();
            if (!key)
                return nullptr;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail("expected ':'");
                return nullptr;
            }
            pos_++;
            skip_ws();
            auto value = parse_value();
            if (!value)
                return nullptr;
            v->object[key->string] = value;
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                pos_++;
                return v;
            }
            fail("expected ',' or '}'");
            return nullptr;
        }
        fail("unterminated object");
        return nullptr;
    }

    std::shared_ptr<Value> parse_array() {
        auto v = std::make_shared<Value>();
        v->type = Value::Type::Array;
        pos_++;  // [
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            pos_++;
            return v;
        }
        while (pos_ < text_.size()) {
            skip_ws();
            auto value = parse_value();
            if (!value)
                return nullptr;
            v->array.push_back(value);
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                pos_++;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                pos_++;
                return v;
            }
            fail("expected ',' or ']'");
            return nullptr;
        }
        fail("unterminated array");
        return nullptr;
    }

    std::shared_ptr<Value> parse_string() {
        auto v = std::make_shared<Value>();
        v->type = Value::Type::String;
        pos_++;  // opening quote
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                pos_++;
                return v;
            }
            if (c == '\\') {
                pos_++;
                if (pos_ >= text_.size())
                    break;
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': v->string += '"'; break;
                    case '\\': v->string += '\\'; break;
                    case '/': v->string += '/'; break;
                    case 'b': v->string += '\b'; break;
                    case 'f': v->string += '\f'; break;
                    case 'n': v->string += '\n'; break;
                    case 'r': v->string += '\r'; break;
                    case 't': v->string += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > text_.size()) {
                            fail("bad \\u escape");
                            return nullptr;
                        }
                        const int code = static_cast<int>(std::strtol(text_.substr(pos_, 4).c_str(), nullptr, 16));
                        pos_ += 4;
                        // UTF-8 encode (BMP only, sufficient for manifests)
                        if (code < 0x80) {
                            v->string += static_cast<char>(code);
                        } else if (code < 0x800) {
                            v->string += static_cast<char>(0xC0 | (code >> 6));
                            v->string += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            v->string += static_cast<char>(0xE0 | (code >> 12));
                            v->string += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            v->string += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default:
                        fail("bad escape");
                        return nullptr;
                }
            } else {
                v->string += c;
                pos_++;
            }
        }
        fail("unterminated string");
        return nullptr;
    }

    std::shared_ptr<Value> parse_number() {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
            pos_++;
        bool digits = false, is_float = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                digits = true;
                pos_++;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                if (c == '.' || c == 'e' || c == 'E')
                    is_float = true;
                pos_++;
            } else {
                break;
            }
        }
        if (!digits) {
            fail("invalid number");
            return nullptr;
        }
        auto v = std::make_shared<Value>();
        v->type = Value::Type::Number;
        v->number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return v;
    }
};

}  // namespace

std::shared_ptr<Value> parse(const std::string &text, std::string *error) {
    std::string local_error;
    Parser parser(text, error ? error : &local_error);
    return parser.run();
}

std::shared_ptr<Value> load(const std::string &path, std::string *error) {
    std::ifstream f(path);
    if (!f) {
        if (error)
            *error = "cannot open " + path;
        return nullptr;
    }
    std::ostringstream o;
    o << f.rdbuf();
    return parse(o.str(), error);
}

}  // namespace json
