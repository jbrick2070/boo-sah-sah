#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace boo {

class JsonValue {
public:
    using Array  = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;

    JsonValue() noexcept : data_(nullptr) {}
    JsonValue(std::nullptr_t) noexcept : data_(nullptr) {}
    JsonValue(bool b) noexcept : data_(b) {}
    JsonValue(double n) noexcept : data_(n) {}
    JsonValue(int64_t n) noexcept : data_(static_cast<double>(n)) {}
    JsonValue(uint64_t n) noexcept : data_(static_cast<double>(n)) {}
    JsonValue(int n) noexcept : data_(static_cast<double>(n)) {}
    JsonValue(std::string s) noexcept : data_(std::move(s)) {}
    JsonValue(const char* s) : data_(std::string(s ? s : "")) {}
    JsonValue(Array a) noexcept : data_(std::move(a)) {}
    JsonValue(Object o) noexcept : data_(std::move(o)) {}

    bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(data_); }
    bool isBool() const noexcept { return std::holds_alternative<bool>(data_); }
    bool isNumber() const noexcept { return std::holds_alternative<double>(data_); }
    bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }
    bool isArray() const noexcept { return std::holds_alternative<Array>(data_); }
    bool isObject() const noexcept { return std::holds_alternative<Object>(data_); }

    bool asBool(bool fallback = false) const noexcept {
        return isBool() ? std::get<bool>(data_) : fallback;
    }
    double asNumber(double fallback = 0.0) const noexcept {
        return isNumber() ? std::get<double>(data_) : fallback;
    }
    int64_t asInt64(int64_t fallback = 0) const noexcept;
    const std::string& asString() const;
    std::string asStringOr(std::string_view fallback) const;
    const Array& asArray() const;
    Array& asArrayMut();
    const Object& asObject() const;
    Object& asObjectMut();

    bool contains(std::string_view key) const noexcept;
    const JsonValue* find(std::string_view key) const noexcept;
    const JsonValue& operator[](std::string_view key) const;
    JsonValue& operator[](std::string key);

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_;
};

struct JsonParseResult {
    bool ok = false;
    JsonValue value;
    std::string error;
    size_t line = 1;
    size_t column = 1;
};

JsonParseResult parseJson(std::string_view text);
JsonParseResult parseJsonFile(const std::filesystem::path& path);
std::string serializeJson(const JsonValue& value, int indentSpaces = 2);

} // namespace boo
