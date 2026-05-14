#include "opennow/util/Json.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace opennow::util {

namespace {

const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonParseResult parse() {
        skipWhitespace();
        auto value = parseValue();
        if (!error_.empty()) {
            return {false, {}, error_};
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return fail("unexpected trailing input");
        }
        return {true, std::move(value), {}};
    }

private:
    JsonParseResult fail(std::string message) {
        std::ostringstream out;
        out << message << " at byte " << pos_;
        return {false, {}, out.str()};
    }

    JsonValue parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            setError("unexpected end of input");
            return {};
        }

        const char c = text_[pos_];
        if (c == 'n') return parseLiteral("null", JsonValue(nullptr));
        if (c == 't') return parseLiteral("true", JsonValue(true));
        if (c == 'f') return parseLiteral("false", JsonValue(false));
        if (c == '"') return JsonValue(parseString());
        if (c == '[') return JsonValue(parseArray());
        if (c == '{') return JsonValue(parseObject());
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return JsonValue(parseNumber());

        setError("unexpected character");
        return {};
    }

    JsonValue parseLiteral(std::string_view literal, JsonValue value) {
        if (text_.substr(pos_, literal.size()) != literal) {
            setError("invalid literal");
            return {};
        }
        pos_ += literal.size();
        return value;
    }

    std::string parseString() {
        std::string out;
        if (!consume('"')) {
            setError("expected string");
            return out;
        }

        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                setError("unterminated escape");
                return out;
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                appendUnicodeEscape(out);
                break;
            default:
                setError("invalid escape");
                return out;
            }
        }

        setError("unterminated string");
        return out;
    }

    void appendUnicodeEscape(std::string& out) {
        if (pos_ + 4 > text_.size()) {
            setError("short unicode escape");
            return;
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code += static_cast<unsigned>(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') code += static_cast<unsigned>(10 + c - 'A');
            else {
                setError("invalid unicode escape");
                return;
            }
        }

        if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | ((code >> 6) & 0x1f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | ((code >> 12) & 0x0f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }

    double parseNumber() {
        const auto start = pos_;
        if (text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }

        const auto raw = std::string(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const auto value = std::strtod(raw.c_str(), &end);
        if (!end || *end != '\0') {
            setError("invalid number");
            return 0;
        }
        return value;
    }

    JsonValue::Array parseArray() {
        JsonValue::Array array;
        consume('[');
        skipWhitespace();
        if (consume(']')) {
            return array;
        }

        while (error_.empty()) {
            array.push_back(parseValue());
            skipWhitespace();
            if (consume(']')) {
                return array;
            }
            if (!consume(',')) {
                setError("expected comma or array end");
                return array;
            }
        }
        return array;
    }

    JsonValue::Object parseObject() {
        JsonValue::Object object;
        consume('{');
        skipWhitespace();
        if (consume('}')) {
            return object;
        }

        while (error_.empty()) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                setError("expected object key");
                return object;
            }
            auto key = parseString();
            skipWhitespace();
            if (!consume(':')) {
                setError("expected object colon");
                return object;
            }
            object.emplace(std::move(key), parseValue());
            skipWhitespace();
            if (consume('}')) {
                return object;
            }
            if (!consume(',')) {
                setError("expected comma or object end");
                return object;
            }
        }
        return object;
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
                break;
            }
            ++pos_;
        }
    }

    void setError(std::string message) {
        if (!error_.empty()) {
            return;
        }
        std::ostringstream out;
        out << message << " at byte " << pos_;
        error_ = out.str();
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

JsonValue::JsonValue() : value_(nullptr) {}
JsonValue::JsonValue(std::nullptr_t) : value_(nullptr) {}
JsonValue::JsonValue(bool value) : value_(value) {}
JsonValue::JsonValue(double value) : value_(value) {}
JsonValue::JsonValue(std::string value) : value_(std::move(value)) {}
JsonValue::JsonValue(Array value) : value_(std::move(value)) {}
JsonValue::JsonValue(Object value) : value_(std::move(value)) {}

bool JsonValue::isNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool JsonValue::isBool() const { return std::holds_alternative<bool>(value_); }
bool JsonValue::isNumber() const { return std::holds_alternative<double>(value_); }
bool JsonValue::isString() const { return std::holds_alternative<std::string>(value_); }
bool JsonValue::isArray() const { return std::holds_alternative<Array>(value_); }
bool JsonValue::isObject() const { return std::holds_alternative<Object>(value_); }

bool JsonValue::asBool(bool fallback) const {
    return isBool() ? std::get<bool>(value_) : fallback;
}

double JsonValue::asNumber(double fallback) const {
    return isNumber() ? std::get<double>(value_) : fallback;
}

std::string JsonValue::asString(std::string fallback) const {
    return isString() ? std::get<std::string>(value_) : std::move(fallback);
}

const JsonValue::Array& JsonValue::asArray() const {
    return isArray() ? std::get<Array>(value_) : kEmptyArray;
}

const JsonValue::Object& JsonValue::asObject() const {
    return isObject() ? std::get<Object>(value_) : kEmptyObject;
}

const JsonValue* JsonValue::get(std::string_view key) const {
    if (!isObject()) {
        return nullptr;
    }
    const auto& object = std::get<Object>(value_);
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &it->second;
}

const JsonValue* JsonValue::at(std::size_t index) const {
    if (!isArray()) {
        return nullptr;
    }
    const auto& array = std::get<Array>(value_);
    return index < array.size() ? &array[index] : nullptr;
}

JsonParseResult parseJson(std::string_view text) {
    return Parser(text).parse();
}

} // namespace opennow::util
