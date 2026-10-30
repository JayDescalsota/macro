#include "json_min.h"
#include <cstdio>
#include <cstring>

namespace {
struct P {
    const char* p;
    const char* end;
    std::string err;
    void skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool expect(char c) {
        skip();
        if (p < end && *p == c) { p++; return true; }
        err = std::string("expected '") + c + "'";
        return false;
    }
    static void encodeUtf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; i++) {
            if (p >= end) { err = "bad \\u escape"; return false; }
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else { err = "bad \\u escape"; return false; }
        }
        return true;
    }
    bool parseString(std::string& out) {
        skip();
        if (p >= end || *p != '"') { err = "expected string"; return false; }
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) { err = "bad escape"; return false; }
                char e = *p++;
                if (e == '"') out.push_back('"');
                else if (e == '\\') out.push_back('\\');
                else if (e == '/') out.push_back('/');
                else if (e == 'b') out.push_back('\b');
                else if (e == 'f') out.push_back('\f');
                else if (e == 'n') out.push_back('\n');
                else if (e == 'r') out.push_back('\r');
                else if (e == 't') out.push_back('\t');
                else if (e == 'u') {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
                        if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            uint32_t lo;
                            if (!hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else { err = "bad low surrogate"; return false; }
                        } else { err = "lone high surrogate"; return false; }
                    }
                    encodeUtf8(cp, out);
                } else { err = "bad escape"; return false; }
            } else out.push_back(c);
        }
        if (p >= end) { err = "unterminated string"; return false; }
        p++;  // closing quote
        return true;
    }
    bool parseValue(JVal& v) {
        skip();
        if (p >= end) { err = "unexpected end"; return false; }
        char c = *p;
        if (c == '{') {
            v.type = JVal::Obj;
            p++;
            skip();
            if (p < end && *p == '}') { p++; return true; }
            while (true) {
                std::string key;
                if (!parseString(key)) return false;
                if (!expect(':')) return false;
                JVal item;
                if (!parseValue(item)) return false;
                v.obj.push_back({key, item});
                skip();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                err = "expected ',' or '}'";
                return false;
            }
        }
        if (c == '[') {
            v.type = JVal::Arr;
            p++;
            skip();
            if (p < end && *p == ']') { p++; return true; }
            while (true) {
                JVal item;
                if (!parseValue(item)) return false;
                v.arr.push_back(item);
                skip();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                err = "expected ',' or ']'";
                return false;
            }
        }
        if (c == '"') { v.type = JVal::Str; return parseString(v.str); }
        if ((c >= '0' && c <= '9') || c == '-') {
            const char* s = p;
            if (*p == '-') p++;
            while (p < end && *p >= '0' && *p <= '9') p++;
            if (p < end && *p == '.') { p++; while (p < end && *p >= '0' && *p <= '9') p++; }
            if (p < end && (*p == 'e' || *p == 'E')) {
                p++;
                if (p < end && (*p == '+' || *p == '-')) p++;
                while (p < end && *p >= '0' && *p <= '9') p++;
            }
            try { v.num = std::stod(std::string(s, p)); }
            catch (...) { err = "bad number"; return false; }
            v.type = JVal::Num;
            return true;
        }
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) { v.type = JVal::Bool; v.b = true; p += 4; return true; }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) { v.type = JVal::Bool; v.b = false; p += 5; return true; }
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) { v.type = JVal::Null; p += 4; return true; }
        err = "unexpected value";
        return false;
    }
};
}  // namespace

bool JParser::parse(const std::string& s, JVal& out, std::string& err) {
    P p{s.c_str(), s.c_str() + s.size()};
    if (!p.parseValue(out)) { err = p.err; return false; }
    p.skip();
    if (p.p != p.end) { err = "trailing data"; return false; }
    return true;
}

std::string JEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else o.push_back((char)c);
    }
    return o;
}
