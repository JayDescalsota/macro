#pragma once
// Minimal JSON parser/serializer for the macro config schema.
// Supports: objects, arrays, strings (with \" \\ \/ \b \f \n \r \t \uXXXX
// incl. surrogate pairs), numbers, true/false/null. UTF-8 in/out.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;                       // UTF-8
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;  // insertion order

    const JVal* find(const std::string& key) const {
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string sval(const std::string& key, const std::string& def = "") const {
        const JVal* v = find(key);
        if (!v || v->type != Str) return def;
        return v->str;
    }
    bool bval(const std::string& key, bool def = false) const {
        const JVal* v = find(key);
        if (!v || v->type != Bool) return def;
        return v->b;
    }
    double nval(const std::string& key, double def = 0) const {
        const JVal* v = find(key);
        if (!v || v->type != Num) return def;
        return v->num;
    }
};

struct JParser {
    static bool parse(const std::string& s, JVal& out, std::string& err);
};

std::string JEscape(const std::string& s);
