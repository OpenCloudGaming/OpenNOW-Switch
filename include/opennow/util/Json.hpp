#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace opennow::util {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    JsonValue();
    explicit JsonValue(std::nullptr_t);
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);
    explicit JsonValue(Array value);
    explicit JsonValue(Object value);

    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0) const;
    std::string asString(std::string fallback = {}) const;

    const Array& asArray() const;
    const Object& asObject() const;

    const JsonValue* get(std::string_view key) const;
    const JsonValue* at(std::size_t index) const;

private:
    Storage value_;
};

struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::string error;
};

JsonParseResult parseJson(std::string_view text);

} // namespace opennow::util
