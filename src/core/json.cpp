#include "boo/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace boo {

namespace {
const std::string kEmptyString{};
const JsonValue::Array kEmptyArray{};
const JsonValue::Object kEmptyObject{};
const JsonValue kNullValue{};

void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src) {
        // Strip UTF-8 BOM if present
        if (src_.size() >= 3 &&
            static_cast<unsigned char>(src_[0]) == 0xEF &&
            static_cast<unsigned char>(src_[1]) == 0xBB &&
            static_cast<unsigned char>(src_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    JsonParseResult parse() {
        skipWhitespace();
        if (eof()) {
            return fail("Empty JSON input");
        }
        JsonValue root;
        if (!parseValue(root, 0)) {
            return result_;
        }
        skipWhitespace();
        if (!eof()) {
            return fail("Unexpected trailing characters after root JSON value");
        }
        result_.ok = true;
        result_.value = std::move(root);
        return result_;
    }

private:
    static constexpr size_t kMaxDepth = 64;
    std::string_view src_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t col_ = 1;
    JsonParseResult result_{};

    bool eof() const noexcept { return pos_ >= src_.size(); }
    char peek() const noexcept { return pos_ < src_.size() ? src_[pos_] : '\0'; }

    char advance() noexcept {
        char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    JsonParseResult fail(std::string msg) {
        result_.ok = false;
        result_.error = std::move(msg);
        result_.line = line_;
        result_.column = col_;
        return result_;
    }

    bool setError(std::string msg) {
        fail(std::move(msg));
        return false;
    }

    void skipWhitespace() noexcept {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else {
                break;
            }
        }
    }

    bool parseValue(JsonValue& out, size_t depth) {
        if (depth > kMaxDepth) {
            return setError("Maximum JSON nesting depth exceeded");
        }
        skipWhitespace();
        if (eof()) {
            return setError("Unexpected end of input");
        }
        char c = peek();
        if (c == '{') return parseObject(out, depth + 1);
        if (c == '[') return parseArray(out, depth + 1);
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out = JsonValue(std::move(s));
            return true;
        }
        if (c == 't') return parseLiteral("true", JsonValue(true), out);
        if (c == 'f') return parseLiteral("false", JsonValue(false), out);
        if (c == 'n') return parseLiteral("null", JsonValue(nullptr), out);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return setError(std::string("Unexpected character '") + c + "'");
    }

    bool parseLiteral(std::string_view lit, JsonValue val, JsonValue& out) {
        for (char expected : lit) {
            if (eof() || peek() != expected) {
                return setError(std::string("Invalid literal; expected '") + std::string(lit) + "'");
            }
            advance();
        }
        out = std::move(val);
        return true;
    }

    bool parseHex4(uint32_t& outVal) {
        outVal = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) return setError("Incomplete \\uXXXX escape");
            char c = advance();
            outVal <<= 4;
            if (c >= '0' && c <= '9') outVal |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') outVal |= static_cast<uint32_t>(10 + c - 'a');
            else if (c >= 'A' && c <= 'F') outVal |= static_cast<uint32_t>(10 + c - 'A');
            else return setError("Invalid hex digit in \\uXXXX escape");
        }
        return true;
    }

    bool parseString(std::string& out) {
        advance(); // consume opening '"'
        out.clear();
        while (!eof()) {
            char c = advance();
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                return setError("Unescaped control character in JSON string");
            }
            if (c == '\\') {
                if (eof()) return setError("Unterminated escape sequence");
                char esc = advance();
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800u && cp <= 0xDBFFu) {
                            if (pos_ + 1 < src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                                advance();
                                advance();
                                uint32_t low = 0;
                                if (!parseHex4(low)) return false;
                                if (low < 0xDC00u || low > 0xDFFFu) {
                                    return setError("Invalid UTF-16 low surrogate");
                                }
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                            } else {
                                return setError("Unpaired UTF-16 high surrogate");
                            }
                        }
                        appendUtf8Codepoint(out, cp);
                        break;
                    }
                    default:
                        return setError(std::string("Invalid escape character '\\") + esc + "'");
                }
            } else {
                out.push_back(c);
            }
        }
        return setError("Unterminated JSON string");
    }

    bool parseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (peek() == '-') advance();
        if (eof()) return setError("Incomplete JSON number");
        if (peek() == '0') {
            advance();
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') advance();
        } else {
            return setError("Invalid JSON number");
        }
        if (!eof() && peek() == '.') {
            advance();
            if (eof() || peek() < '0' || peek() > '9') {
                return setError("Expected fractional digits after '.'");
            }
            while (!eof() && peek() >= '0' && peek() <= '9') advance();
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (!eof() && (peek() == '+' || peek() == '-')) advance();
            if (eof() || peek() < '0' || peek() > '9') {
                return setError("Expected exponent digits");
            }
            while (!eof() && peek() >= '0' && peek() <= '9') advance();
        }

        double val = 0.0;
        const auto res = std::from_chars(src_.data() + start, src_.data() + pos_, val);
        if (res.ec != std::errc{} || !std::isfinite(val)) {
            return setError("JSON number out of finite double range");
        }
        out = JsonValue(val);
        return true;
    }

    bool parseArray(JsonValue& out, size_t depth) {
        advance(); // '['
        JsonValue::Array arr;
        skipWhitespace();
        if (!eof() && peek() == ']') {
            advance();
            out = JsonValue(std::move(arr));
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parseValue(elem, depth)) return false;
            arr.push_back(std::move(elem));
            skipWhitespace();
            if (eof()) return setError("Unterminated JSON array");
            char c = advance();
            if (c == ']') break;
            if (c != ',') return setError("Expected ',' or ']' in JSON array");
        }
        out = JsonValue(std::move(arr));
        return true;
    }

    bool parseObject(JsonValue& out, size_t depth) {
        advance(); // '{'
        JsonValue::Object obj;
        skipWhitespace();
        if (!eof() && peek() == '}') {
            advance();
            out = JsonValue(std::move(obj));
            return true;
        }
        while (true) {
            skipWhitespace();
            if (eof() || peek() != '"') {
                return setError("Expected string key in JSON object");
            }
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (eof() || advance() != ':') {
                return setError("Expected ':' after JSON object key");
            }
            JsonValue val;
            if (!parseValue(val, depth)) return false;
            obj.insert_or_assign(std::move(key), std::move(val));
            skipWhitespace();
            if (eof()) return setError("Unterminated JSON object");
            char c = advance();
            if (c == '}') break;
            if (c != ',') return setError("Expected ',' or '}' in JSON object");
        }
        out = JsonValue(std::move(obj));
        return true;
    }
};

void writeEscapedString(std::ostringstream& os, std::string_view s) {
    os.put('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    os << buf;
                } else {
                    os.put(static_cast<char>(c));
                }
                break;
        }
    }
    os.put('"');
}

void serializeImpl(std::ostringstream& os, const JsonValue& v, int indentSpaces, int depth) {
    const auto writeIndent = [&](int d) {
        if (indentSpaces > 0) {
            os << std::string(static_cast<size_t>(d * indentSpaces), ' ');
        }
    };

    if (v.isNull()) {
        os << "null";
    } else if (v.isBool()) {
        os << (v.asBool() ? "true" : "false");
    } else if (v.isNumber()) {
        const double num = v.asNumber();
        if (!std::isfinite(num)) {
            os << "0";
        } else if (std::floor(num) == num && std::abs(num) < 9007199254740992.0) {
            os << static_cast<int64_t>(num);
        } else {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%.6g", num);
            os << buf;
        }
    } else if (v.isString()) {
        writeEscapedString(os, v.asString());
    } else if (v.isArray()) {
        const auto& arr = v.asArray();
        if (arr.empty()) {
            os << "[]";
            return;
        }
        os << '[';
        if (indentSpaces > 0) os << '\n';
        for (size_t i = 0; i < arr.size(); ++i) {
            writeIndent(depth + 1);
            serializeImpl(os, arr[i], indentSpaces, depth + 1);
            if (i + 1 < arr.size()) os << ',';
            if (indentSpaces > 0) os << '\n';
        }
        writeIndent(depth);
        os << ']';
    } else if (v.isObject()) {
        const auto& obj = v.asObject();
        if (obj.empty()) {
            os << "{}";
            return;
        }
        os << '{';
        if (indentSpaces > 0) os << '\n';
        size_t idx = 0;
        for (const auto& [k, val] : obj) {
            writeIndent(depth + 1);
            writeEscapedString(os, k);
            os << (indentSpaces > 0 ? ": " : ":");
            serializeImpl(os, val, indentSpaces, depth + 1);
            if (++idx < obj.size()) os << ',';
            if (indentSpaces > 0) os << '\n';
        }
        writeIndent(depth);
        os << '}';
    }
}

} // namespace

int64_t JsonValue::asInt64(int64_t fallback) const noexcept {
    if (!isNumber()) return fallback;
    const double d = std::get<double>(data_);
    if (!std::isfinite(d)) return fallback;
    return static_cast<int64_t>(std::llround(d));
}

const std::string& JsonValue::asString() const {
    return isString() ? std::get<std::string>(data_) : kEmptyString;
}

std::string JsonValue::asStringOr(std::string_view fallback) const {
    return isString() ? std::get<std::string>(data_) : std::string(fallback);
}

const JsonValue::Array& JsonValue::asArray() const {
    return isArray() ? std::get<Array>(data_) : kEmptyArray;
}

JsonValue::Array& JsonValue::asArrayMut() {
    if (!isArray()) data_ = Array{};
    return std::get<Array>(data_);
}

const JsonValue::Object& JsonValue::asObject() const {
    return isObject() ? std::get<Object>(data_) : kEmptyObject;
}

JsonValue::Object& JsonValue::asObjectMut() {
    if (!isObject()) data_ = Object{};
    return std::get<Object>(data_);
}

bool JsonValue::contains(std::string_view key) const noexcept {
    if (!isObject()) return false;
    const auto& obj = std::get<Object>(data_);
    return obj.find(key) != obj.end();
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (!isObject()) return nullptr;
    const auto& obj = std::get<Object>(data_);
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    const JsonValue* found = find(key);
    return found ? *found : kNullValue;
}

JsonValue& JsonValue::operator[](std::string key) {
    return asObjectMut()[std::move(key)];
}

JsonParseResult parseJson(std::string_view text) {
    Parser p(text);
    return p.parse();
}

JsonParseResult parseJsonFile(const std::filesystem::path& path) {
    JsonParseResult res;
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        res.ok = false;
        res.error = "Unable to open file: " + path.string();
        return res;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return parseJson(ss.str());
}

std::string serializeJson(const JsonValue& value, int indentSpaces) {
    std::ostringstream os;
    serializeImpl(os, value, indentSpaces, 0);
    if (indentSpaces > 0) os << '\n';
    return os.str();
}

} // namespace boo
