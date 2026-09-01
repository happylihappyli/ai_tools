#ifndef AB_JSON_H
#define AB_JSON_H
// SPDX-License-Identifier: MIT
//
// AbJson — 轻量级 JSON 解析 (够 ab 用, 避免 nlohmann/json 依赖)
// 支持 object / array / string / number / bool / null
// API:
//   AbJson v = AbJson::parse(text);
//   if (v.isObject()) { auto& obj = v.toObject(); v["key"].toString(); }
//   v["key"].toString("default");
//   v["arr"].toArray().size();

#include <map>
#include <vector>
#include <string>
#include <variant>
#include <stdexcept>

namespace ab {

class AbJson;

using AbJsonObject = std::map<std::string, AbJson>;
using AbJsonArray  = std::vector<AbJson>;

class AbJson {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    AbJson() : type_(Null) {}
    AbJson(bool b) : type_(Bool), bool_(b) {}
    AbJson(int n) : type_(Number), num_(static_cast<double>(n)) {}
    AbJson(double n) : type_(Number), num_(n) {}
    AbJson(const std::string& s) : type_(String), str_(s) {}
    AbJson(const char* s) : type_(String), str_(s) {}
    AbJson(const AbJsonArray& a) : type_(Array), arr_(a) {}
    AbJson(const AbJsonObject& o) : type_(Object), obj_(o) {}

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Null; }
    bool isBool()   const { return type_ == Bool; }
    bool isNumber() const { return type_ == Number; }
    bool isString() const { return type_ == String; }
    bool isArray()  const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }

    bool toBool(bool def = false) const { return isBool() ? bool_ : def; }
    double toNumber(double def = 0.0) const { return isNumber() ? num_ : def; }
    int toInt(int def = 0) const { return isNumber() ? static_cast<int>(num_) : def; }
    const std::string& toString(const std::string& def = "") const {
        static const std::string empty;
        if (isString()) return str_;
        if (def.empty()) return empty;
        static std::string s_def;
        s_def = def;
        return s_def;
    }
    const AbJsonArray& toArray() const {
        static const AbJsonArray empty_arr;
        return isArray() ? arr_ : empty_arr;
    }
    const AbJsonObject& toObject() const {
        static const AbJsonObject empty_obj;
        return isObject() ? obj_ : empty_obj;
    }

    // 便捷: 访问对象字段 (key 不存在或类型不对返回 Null)
    const AbJson& operator[](const std::string& key) const {
        if (isObject()) {
            auto it = obj_.find(key);
            if (it != obj_.end()) return it->second;
        }
        return null_instance();
    }
    const AbJson& operator[](const char* key) const { return (*this)[std::string(key)]; }
    const AbJson& operator[](int idx) const {
        if (isArray() && idx >= 0 && idx < static_cast<int>(arr_.size())) {
            return arr_[idx];
        }
        return null_instance();
    }
    bool contains(const std::string& key) const {
        return isObject() && obj_.count(key) > 0;
    }

    // 解析 (入口)
    static AbJson parse(const std::string& text);
    static AbJson parseFile(const std::string& path);

private:
    static const AbJson& null_instance() {
        static AbJson n;
        return n;
    }
    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    AbJsonArray arr_;
    AbJsonObject obj_;
};

}  // namespace ab

#endif  // AB_JSON_H
