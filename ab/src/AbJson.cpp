// AbJson.cpp — 简化版 JSON 解析实现
// 支持 object / array / string / number / bool / null
// 递归下降, 错误抛 std::runtime_error

#include "AbJson.h"
#include <fstream>
#include <sstream>
#include <cctype>

namespace ab {

namespace {

struct Parser {
    const std::string& s;
    size_t pos = 0;
    explicit Parser(const std::string& str) : s(str) {}

    void skip() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    char peek() {
        skip();
        if (pos >= s.size()) throw std::runtime_error("unexpected EOF");
        return s[pos];
    }
    char get() {
        skip();
        if (pos >= s.size()) throw std::runtime_error("unexpected EOF");
        return s[pos++];
    }
    void expect(char c) {
        if (get() != c) throw std::runtime_error(std::string("expected '") + c + "'");
    }
    bool consume(const char* kw) {
        skip();
        size_t n = std::string(kw).size();
        if (pos + n > s.size()) return false;
        for (size_t i = 0; i < n; ++i) {
            if (s[pos + i] != kw[i]) return false;
        }
        pos += n;
        return true;
    }

    AbJson parseValue() {
        skip();
        if (pos >= s.size()) throw std::runtime_error("EOF");
        char c = s[pos];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return AbJson(parseString());
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        return AbJson(parseNumber());
    }

    AbJsonObject parseObject() {
        AbJsonObject obj;
        expect('{');
        skip();
        if (peek() == '}') { get(); return obj; }
        while (true) {
            skip();
            if (peek() != '"') throw std::runtime_error("expected string key");
            std::string key = parseString();
            expect(':');
            obj[key] = parseValue();
            skip();
            if (peek() == ',') { get(); continue; }
            if (peek() == '}') { get(); break; }
            throw std::runtime_error("expected ',' or '}'");
        }
        return obj;
    }

    AbJsonArray parseArray() {
        AbJsonArray arr;
        expect('[');
        skip();
        if (peek() == ']') { get(); return arr; }
        while (true) {
            arr.push_back(parseValue());
            skip();
            if (peek() == ',') { get(); continue; }
            if (peek() == ']') { get(); break; }
            throw std::runtime_error("expected ',' or ']'");
        }
        return arr;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                char n = s[pos + 1];
                switch (n) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        if (pos + 5 >= s.size()) throw std::runtime_error("bad \\u");
                        unsigned int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            cp <<= 4;
                            char h = s[pos + 2 + i];
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            else throw std::runtime_error("bad hex");
                        }
                        // 简化: 不完整处理 surrogate pair, 直接 utf-8 编码 BMP
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
                        pos += 4;
                        break;
                    }
                    default: out += n; break;
                }
                pos += 2;
            } else {
                out += s[pos++];
            }
        }
        expect('"');
        return out;
    }

    AbJson parseBool() {
        if (consume("true"))  return AbJson(true);
        if (consume("false")) return AbJson(false);
        throw std::runtime_error("expected bool");
    }
    AbJson parseNull() {
        if (consume("null")) return AbJson();
        throw std::runtime_error("expected null");
    }
    AbJson parseNumber() {
        skip();
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos]))
               || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E'
               || s[pos] == '+' || s[pos] == '-')) ++pos;
        std::string sub = s.substr(start, pos - start);
        return AbJson(std::stod(sub));
    }
};

}  // anonymous namespace

AbJson AbJson::parse(const std::string& text) {
    Parser p(text);
    AbJson v = p.parseValue();
    p.skip();
    if (p.pos < p.s.size()) {
        throw std::runtime_error("trailing content after JSON value");
    }
    return v;
}

AbJson AbJson::parseFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open: " + path);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

}  // namespace ab
