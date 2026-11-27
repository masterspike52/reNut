// Minimal JSON value + parser + pretty writer, used for unpack manifests.
#pragma once

#include "common.hpp"

#include <map>
#include <memory>

namespace nut {

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Bool), bool_(b) {}
    Json(int v) : type_(Number), num_(v) {}
    Json(unsigned v) : type_(Number), num_(v) {}
    Json(int64_t v) : type_(Number), num_(double(v)) {}
    Json(uint64_t v) : type_(Number), num_(double(v)) {}
    Json(double v) : type_(Number), num_(v) {}
    Json(const char* s) : type_(String), str_(s) {}
    Json(std::string s) : type_(String), str_(std::move(s)) {}

    static Json array() { Json j; j.type_ = Array; return j; }
    static Json object() { Json j; j.type_ = Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }

    // Accessors throw on type mismatch so malformed manifests fail loudly.
    bool asBool() const;
    double asNumber() const;
    uint32_t asU32() const;
    int64_t asI64() const;
    const std::string& asString() const;
    const std::vector<Json>& items() const;
    std::vector<Json>& items();

    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);
    const Json& operator[](size_t i) const { return items().at(i); }
    size_t size() const;

    void push(Json v);
    // Object keys keep insertion order so written manifests stay readable.
    const std::vector<std::pair<std::string, Json>>& members() const;

    std::string dump(int indent = 2) const;
    static Json parse(std::string_view text);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace nut
