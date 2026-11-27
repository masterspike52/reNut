#include "gamescript.hpp"

#include "assetdb.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace nut::gamescript {
namespace {

// Field codes: i int, f float, a asset id, x hex word, s<N> string of N bytes; "*k" repeats a field.
struct OpSpec {
    uint32_t op;
    const char* fields;
};

const OpSpec kOps[] = {
    {0x00, ""}, {0x01, "x a x*3 i f*7"}, {0x02, "x"}, {0x03, "i x*2"}, {0x04, "i"}, {0x05, "i"}, {0x0F, "i"},
    {0x10, "a x i*3"}, {0x11, "a i"}, {0x12, "f*2 i*4"}, {0x19, "x f i"}, {0x1A, ""}, {0x1B, ""}, {0x1C, "i*2"},
    {0x22, "s64 i"}, {0x23, "i"}, {0x27, "x*2 f*2 i f x"}, {0x2C, "x i*5"}, {0x2D, "x f"}, {0x32, "s64"}, {0x34, "i*4"},
    {0x48, "s64 i*2"}, {0x4E, "x a"}, {0x50, "s256"}, {0x52, "x"}, {0x53, "i f*6 x f*6 x*2 f"}, {0x54, "f*7"},
    {0x57, "s64 a i"}, {0x58, "s64 i*3"}, {0x59, "f"}, {0x5A, "x i"}, {0x5D, "s64 i"}, {0x5E, ""}, {0x5F, ""},
    {0x60, "s64 a i"}, {0x63, "i"}, {0x64, "i"}, {0x65, "x"}, {0x66, "x"}, {0x67, "s64 i"},
    {0x69, "i*2 a*2 i a f*4 i*2 f*12 i*2 a i*2 f*9"}, {0x6C, "f*5"}, {0x6D, "s64 i"}, {0x6F, "s64*2 a*3 x"}, {0x71, ""},
    {0x72, "a i f*4 i"}, {0x73, "a i f*3"}, {0x75, "i s64 i"}, {0x76, "i"}, {0x77, ""}, {0x78, "i"}, {0x79, "a f*9"},
    {0x7A, "a*2 i f*2"}, {0x7B, "a i"}, {0x7C, ""},
    {0x7D, "f*9 i*2 f*2 x f x f*12 x f*7 i*3 x*2 i*3 a*2 i a*3 i*2 f i f*4 i*3 a*8 i*2"}, {0x7E, "i x f*2 i f"},
    {0x7F, "a f i*2"}, {0x80, "a*6"}, {0x81, "i"}, {0x82, "i"}, {0x84, "i*3"}, {0x85, "i*2 x i x i x i x i x i x"},
    {0x86, "a"}, {0x87, "i f*7"}, {0x88, "s256 i"}, {0x89, "s64 i"}, {0x8A, "i"}, {0x8B, "s64 i"}, {0x8C, "i"},
    {0x8D, "i*2"}, {0x8E, ""}, {0x90, "s64 i"}, {0x92, "a i"}, {0x93, "a"}, {0x94, "i"}, {0x95, "f"},
    {0x96, "i*4 f i*2"}, {0x97, "i"}, {0x98, "s64 f*2"}, {0x99, "i*2 f*11"}, {0x9A, "a*10"}, {0x9C, "s64"},
    {0x9E, "x"}, {0x9F, "s256 i*2"}, {0xA0, "x*2 i f x"}, {0xA1, "s32 f"}, {0xA2, ""}, {0xA3, "s64 i"}, {0xA4, "i"},
    {0xA6, ""}, {0xA7, "a*6 i*5"},
};

struct GoalSpec {
    const char* name;
    const char* fields;
};

const GoalSpec kGoals[] = {
    {"actorMindGoalId_AimAtTarget", "f*3"},
    {"actorMindGoalId_BeginScriptAid", "a i"},
    {"actorMindGoalId_Celebrate", "i"},
    {"actorMindGoalId_CircleTarget", "f*7 i"},
    {"actorMindGoalId_ClearGoalsIfBodyFlag", "s64 i"},
    {"actorMindGoalId_ClearGoalsIfExitStatus", "i*2"},
    {"actorMindGoalId_DiveBombAtTarget", "f*2"},
    {"actorMindGoalId_DriveAtTarget", "f"},
    {"actorMindGoalId_DriveBuggyAroundCourse", "f*4"},
    {"actorMindGoalId_DriveBuggyFaceTarget", "f i f i*3 f i"},
    {"actorMindGoalId_DriveToDestination", "f i*2"},
    {"actorMindGoalId_EnterVehicle", "i*2 f"},
    {"actorMindGoalId_Evade", "f*8 i*3 f*5 i*2 f*3"},
    {"actorMindGoalId_EvadeVehicle", "f*6 i"},
    {"actorMindGoalId_FireWeapon", "i*2 f"},
    {"actorMindGoalId_FlyToDestination", "f*4 i*2 a i"},
    {"actorMindGoalId_FlyToTarget", "f*4 i*3 f i*3 f"},
    {"actorMindGoalId_GenerateEvasionDestination", "f*4"},
    {"actorMindGoalId_GenerateRandomDestination", "f*2 i*6"},
    {"actorMindGoalId_GenerateRandomFlyDestination", "f*2 i*4"},
    {"actorMindGoalId_GenerateRandomFlyDestinationCloseToTarget", "i*2 f i*4"},
    {"actorMindGoalId_GetClosestAvailableVehicle", "f i*8"},
    {"actorMindGoalId_GetInFrontOfTarget", "f i f*2"},
    {"actorMindGoalId_GruntbotFlyEvadeTarget", "f i f*2"},
    {"actorMindGoalId_GruntbotFlyToTarget", "f*4 i*3 f i*4"},
    {"actorMindGoalId_Null", "i"},
    {"actorMindGoalId_ReverseBuggy", "f i*2 f"},
    {"actorMindGoalId_SceneActorSyncRegister", "i"},
    {"actorMindGoalId_SceneActorSyncSetStatus", "i"},
    {"actorMindGoalId_SceneActorSyncWaitNReady", "i"},
    {"actorMindGoalId_SetGoalFlag", "s48"},
    {"actorMindGoalId_SetState", "s64*2 i*2"},
    {"actorMindGoalId_SetStateForWantedBasis", "s64*3"},
    {"actorMindGoalId_SetStateForWantedBasisIfFlag", "s64*4 i*2"},
    {"actorMindGoalId_SetStateIfFlag", "s64*2 i*2"},
    {"actorMindGoalId_SimulateButtonPress", "i"},
    {"actorMindGoalId_TurnToFaceDestination", "f i"},
    {"actorMindGoalId_TurnToFaceInitialRot", "f i"},
    {"actorMindGoalId_TurnToFaceTarget", "f"},
    {"actorMindGoalId_Wait", "f*2"},
    {"actorMindGoalId_WaitForFlag", "s64 i"},
    {"actorMindGoalId_WaitForSafeStateChange", "i"},
    {"actorMindGoalId_WalkAroundCourse", "f*4 i"},
    {"actorMindGoalId_WalkToCourse", "f*3 i f i*3 f*5 i*2 f*3"},
    {"actorMindGoalId_WalkToDestination", "f*5 i*3 f*5 i*2 f*3 i"},
    {"actorMindGoalId_WalkToTarget", "f*5 i*3 f*5 i*2 f*3 i f*3 i*5 s64"},
    {"actorMindGoalId_WalkToVehicle", "f*2 i f*2 i*3 f*5 i*2 f*3"},
    {"actorMindGoalId_WarpToDestination", "i"},
};

struct Field {
    char type;
    uint32_t size;
};

struct Layout {
    std::vector<Field> fields;
    uint32_t size = 0;
    std::string spec;
};

Layout parseLayout(std::string_view spec) {
    Layout l;
    l.spec = spec;
    for (const std::string& tok : split(spec, ' ')) {
        if (tok.empty()) continue;
        size_t star = tok.find('*');
        std::string f = tok.substr(0, star);
        int count = star == std::string::npos ? 1 : std::atoi(tok.c_str() + star + 1);
        Field fd{f[0], f[0] == 's' ? uint32_t(std::atoi(f.c_str() + 1)) : 4u};
        for (int k = 0; k < count; ++k) {
            l.fields.push_back(fd);
            l.size += fd.size;
        }
    }
    return l;
}

const Layout* opLayout(uint32_t op) {
    static const std::map<uint32_t, Layout> table = [] {
        std::map<uint32_t, Layout> m;
        for (auto& o : kOps) m[o.op] = parseLayout(o.fields);
        return m;
    }();
    auto it = table.find(op);
    return it == table.end() ? nullptr : &it->second;
}

const Layout* goalLayout(const std::string& name) {
    static const std::map<std::string, Layout> table = [] {
        std::map<std::string, Layout> m;
        for (auto& g : kGoals) m[g.name] = parseLayout(g.fields);
        return m;
    }();
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

const Layout& tableLayout(Kind k) {
    static const Layout scriptTable = parseLayout("s64 a"), stateTable = parseLayout("s64 a*2 s64");
    return k == Kind::StateTable ? stateTable : scriptTable;
}

[[noreturn]] void lineFail(int line, const std::string& msg) { fail("line " + std::to_string(line) + ": " + msg); }

// ---- writing ----------------------------------------------------------------------

std::string hexWord(uint32_t v) {
    char t[16];
    std::snprintf(t, sizeof t, "0x%08X", v);
    return t;
}

// Shortest text that reads back as the same bits; denormals, infinities and NaNs are written as hex.
std::string floatText(uint32_t bits) {
    uint32_t e = (bits >> 23) & 0xFF;
    if ((e == 0 && (bits & 0x7FFFFFFF)) || e == 0xFF) return hexWord(bits);
    float f;
    std::memcpy(&f, &bits, 4);
    char t[48];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(t, sizeof t, "%.*g", prec, double(f));
        float back = std::strtof(t, nullptr);
        uint32_t bb;
        std::memcpy(&bb, &back, 4);
        if (bb == bits) break;
    }
    std::string s = t;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string quote(std::string_view s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c >= 0x20 && c < 0x7F) {
            o += char(c);
        } else {
            char t[8];
            std::snprintf(t, sizeof t, "\\x%02X", c);
            o += t;
        }
    }
    return o + "\"";
}

std::string rawToken(const uint8_t* p, size_t n) {
    std::string s = "<";
    for (size_t i = 0; i < n; ++i) s += hex8(p[i]);
    return s + ">";
}

// A fixed-size string field; one with bytes after its terminator is written as raw bytes to stay exact.
std::string stringField(const uint8_t* p, uint32_t n, bool withSize) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    if (len == n) return rawToken(p, n);
    for (size_t k = len; k < n; ++k)
        if (p[k]) return rawToken(p, n);
    std::string s = quote(std::string_view(reinterpret_cast<const char*>(p), len));
    if (withSize) s += ":" + std::to_string(n);
    return s;
}

std::string wordText(char type, uint32_t v, const NameLookup& names) {
    std::string name = names && v != 0 && v != 0xFFFFFFFF ? names(v) : std::string();
    switch (type) {
        case 'i': return std::to_string(int32_t(v));
        case 'f': return floatText(v);
        case 'a':
            if (!name.empty()) return "@" + name;
            return v == 0 || v == 0xFFFFFFFF ? std::to_string(int32_t(v)) : hexWord(v);
        default: return name.empty() ? hexWord(v) : "@" + name;
    }
}

void layoutFields(const Layout& l, const uint8_t* p, const NameLookup& names, std::string& out) {
    size_t off = 0;
    for (auto& f : l.fields) {
        out += ' ';
        out += f.type == 's' ? stringField(p + off, f.size, false) : wordText(f.type, be32(p + off), names);
        off += f.size;
    }
}

// Arguments of an opcode or goal without a known layout: strings get explicit sizes.
void guessedFields(const uint8_t* p, size_t n, const NameLookup& names, std::string& out) {
    size_t off = 0;
    while (off < n) {
        if (n - off < 4) {
            out += ' ' + rawToken(p + off, n - off);
            break;
        }
        size_t len = 0;
        while (off + len < n && p[off + len] >= 0x20 && p[off + len] < 0x7F) ++len;
        if (len >= 4 && off + len < n && p[off + len] == 0) {
            size_t z = off + len + 1;
            while (z < n && !p[z]) ++z;
            size_t w = (z - off) / 4 * 4;
            if (w >= len + 1) {
                out += ' ' + stringField(p + off, uint32_t(w), true);
                off += w;
                continue;
            }
        }
        uint32_t v = be32(p + off);
        char t = 'x';
        if (names && v != 0 && v != 0xFFFFFFFF && !names(v).empty()) t = 'a';
        else if (v < 0x100000 || v >= 0xFFF00000) t = 'i';
        else if (uint32_t e = (v >> 23) & 0xFF; e >= 100 && e <= 150) t = 'f';
        out += ' ' + wordText(t, v, names);
        off += 4;
    }
}

void argsText(const Layout* l, const uint8_t* p, size_t n, const NameLookup& names, std::string& out) {
    if (l && l->size == n) layoutFields(*l, p, names, out);
    else guessedFields(p, n, names, out);
}

bool isIdentifier(std::string_view s) {
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    for (char c : s)
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

const char* headerText(Kind k) {
    switch (k) {
        case Kind::Script: return "# Scene script: one command per line, op_XX (the opcode in hex) then its arguments. It must end with op_00.\n";
        case Kind::ActorGoals: return "# Actor goals: one goal per line, the goal's name then its arguments.\n";
        case Kind::ScriptTable: return "# Script table: one entry per line: its name, then the asset it runs.\n";
        case Kind::StateTable: return "# State table: one entry per line: the body state, two assets, then a name.\n";
    }
    return "";
}

// ---- reading ----------------------------------------------------------------------

using text::Token;

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<Token> tokenize(std::string_view s, int line) {
    std::vector<Token> out;
    size_t i = 0;
    auto bareEnd = [&](size_t from) {
        size_t e = from;
        while (e < s.size() && s[e] != ' ' && s[e] != '\t' && s[e] != '#') ++e;
        return e;
    };
    while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '#') break;
        Token t;
        if (c == '"') {
            t.type = Token::String;
            ++i;
            for (;;) {
                if (i >= s.size()) lineFail(line, "unterminated string");
                char d = s[i++];
                if (d == '"') break;
                if (d != '\\') {
                    t.text += d;
                    continue;
                }
                if (i >= s.size()) lineFail(line, "unterminated string");
                char e = s[i++];
                if (e == 'x') {
                    int h = i + 1 < s.size() ? hexDigit(s[i]) : -1, l = i + 1 < s.size() ? hexDigit(s[i + 1]) : -1;
                    if (h < 0 || l < 0) lineFail(line, "\\x needs two hex digits");
                    t.text += char(h * 16 + l);
                    i += 2;
                } else if (e == 'n') {
                    t.text += '\n';
                } else if (e == 't') {
                    t.text += '\t';
                } else {
                    t.text += e;
                }
            }
            if (i < s.size() && s[i] == ':') {
                size_t e = bareEnd(++i);
                std::string n(s.substr(i, e - i));
                char* end = nullptr;
                unsigned long v = std::strtoul(n.c_str(), &end, 10);
                if (n.empty() || *end || v == 0 || v > 0xFFFF) lineFail(line, "bad string size ':" + n + "'");
                t.size = uint32_t(v);
                i = e;
            }
        } else if (c == '<') {
            t.type = Token::Raw;
            size_t e = s.find('>', i);
            if (e == std::string_view::npos) lineFail(line, "unterminated <bytes>");
            std::string_view h = s.substr(i + 1, e - i - 1);
            if (h.size() % 2) lineFail(line, "<bytes> needs an even number of hex digits");
            for (size_t k = 0; k < h.size(); k += 2) {
                int a = hexDigit(h[k]), b = hexDigit(h[k + 1]);
                if (a < 0 || b < 0) lineFail(line, "bad hex digit in <bytes>");
                t.raw.push_back(uint8_t(a * 16 + b));
            }
            i = e + 1;
        } else {
            size_t e = bareEnd(i);
            std::string w(s.substr(i, e - i));
            i = e;
            if (w[0] == '@') {
                auto id = assetdb::idFromName(w.substr(1));
                if (!id) lineFail(line, "'" + w + "' is not an asset name (aid_<type>_...)");
                t.word = *id;
            } else if (startsWith(w, "flags=")) {
                char* end = nullptr;
                unsigned long v = std::strtoul(w.c_str() + 6, &end, 0);
                if (w.size() == 6 || *end || v > 0xFFFF) lineFail(line, "bad '" + w + "'");
                t.type = Token::Flags;
                t.word = uint32_t(v);
            } else if (startsWith(w, "0x") || startsWith(w, "0X")) {
                char* end = nullptr;
                unsigned long long v = std::strtoull(w.c_str() + 2, &end, 16);
                if (w.size() == 2 || *end || v > 0xFFFFFFFFull) lineFail(line, "bad hex value '" + w + "'");
                t.word = uint32_t(v);
            } else if ((w[0] >= '0' && w[0] <= '9') || w[0] == '-' || w[0] == '+' || w[0] == '.' || lower(w) == "inf" || lower(w) == "nan") {
                char* end = nullptr;
                if (w.find_first_of(".eEnN") != std::string::npos) {
                    float f = std::strtof(w.c_str(), &end);
                    if (*end) lineFail(line, "bad number '" + w + "'");
                    std::memcpy(&t.word, &f, 4);
                } else {
                    long long v = std::strtoll(w.c_str(), &end, 10);
                    if (*end || v < -2147483648ll || v > 0xFFFFFFFFll) lineFail(line, "bad number '" + w + "'");
                    t.word = uint32_t(v);
                }
            } else {
                t.type = Token::Head;
                t.text = w;
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

void encodeArgs(const std::vector<Token>& toks, size_t from, const Layout* l, Bytes& out, int line, const std::string& what) {
    const size_t start = out.size();
    for (size_t i = from; i < toks.size(); ++i) {
        const Token& t = toks[i];
        const size_t off = out.size() - start;
        switch (t.type) {
            case Token::Word: {
                uint8_t b[4];
                putBe32(b, t.word);
                out.insert(out.end(), b, b + 4);
                break;
            }
            case Token::Raw: out.insert(out.end(), t.raw.begin(), t.raw.end()); break;
            case Token::String: {
                uint32_t n = t.size;
                if (!n && l) {
                    size_t at = 0;
                    for (auto& f : l->fields) {
                        if (at == off && f.type == 's') n = f.size;
                        if (at >= off) break;
                        at += f.size;
                    }
                }
                if (!n) lineFail(line, "no string field here in " + what + "; give its size, as in \"text\":64");
                if (t.text.size() >= n) lineFail(line, quote(t.text) + " is too long: the field holds " + std::to_string(n - 1) + " characters");
                out.insert(out.end(), t.text.begin(), t.text.end());
                out.insert(out.end(), n - t.text.size(), 0);
                break;
            }
            case Token::Head:
            case Token::Flags: lineFail(line, "unexpected '" + (t.type == Token::Flags ? "flags=" + std::to_string(t.word) : t.text) + "'");
        }
    }
    size_t got = out.size() - start;
    if (l && got != l->size)
        lineFail(line, what + " takes " + std::to_string(l->size) + " bytes of arguments (" + (l->spec.empty() ? "none" : l->spec) + "), not " +
                           std::to_string(got));
}

} // namespace

std::optional<Kind> kindOf(std::string_view n) {
    if (startsWith(n, "aid_script_")) return Kind::Script;
    if (startsWith(n, "aid_actorgoals_")) return Kind::ActorGoals;
    if (startsWith(n, "aid_scripttable_")) return Kind::ScriptTable;
    if (startsWith(n, "aid_statetable_")) return Kind::StateTable;
    return std::nullopt;
}

std::optional<Kind> kindOfId(uint32_t id) { return kindOf("aid_" + assetdb::typeName(uint8_t(id >> 24)) + "_"); }

const char* kindLabel(Kind k) {
    switch (k) {
        case Kind::Script: return "script";
        case Kind::ActorGoals: return "actor goals";
        case Kind::ScriptTable: return "script table";
        case Kind::StateTable: return "state table";
    }
    return "?";
}

std::string toText(Kind k, const Bytes& d, const NameLookup& names) {
    std::string out = headerText(k);
    out += "# Values: 12 or -3 (int), 1.5 (float), 0x1A2B3C4D (hex), @aid_name (asset id), \"text\" (string), <0a0b> (raw bytes).\n";
    size_t p = 0;
    if (k == Kind::Script) {
        while (p < d.size()) {
            if (d.size() - p < 8) fail("script: truncated command at offset " + std::to_string(p));
            uint32_t size = be32(&d[p]), op = be32(&d[p + 4]);
            if (size < 8 || size > d.size() - p) fail("script: bad command size at offset " + std::to_string(p));
            char t[24];
            std::snprintf(t, sizeof t, "op_%02X", op);
            out += t;
            argsText(opLayout(op), &d[p + 8], size - 8, names, out);
            out += '\n';
            p += size;
        }
    } else if (k == Kind::ActorGoals) {
        while (p < d.size()) {
            if (d.size() - p < 68) fail("actor goals: truncated goal at offset " + std::to_string(p));
            uint32_t size = be16(&d[p]), flags = be16(&d[p + 2]);
            if (size < 68 || size > d.size() - p) fail("actor goals: bad goal size at offset " + std::to_string(p));
            const uint8_t* nm = &d[p + 4];
            size_t len = 0;
            while (len < 64 && nm[len]) ++len;
            std::string name(reinterpret_cast<const char*>(nm), len);
            bool clean = len < 64 && isIdentifier(name) && !startsWith(name, "flags=");
            for (size_t i = len; clean && i < 64; ++i) clean = nm[i] == 0;
            out += clean ? name : stringField(nm, 64, false);
            if (flags) out += " flags=" + std::to_string(flags);
            argsText(clean ? goalLayout(name) : nullptr, &d[p + 68], size - 68, names, out);
            out += '\n';
            p += size;
        }
    } else {
        const Layout& l = tableLayout(k);
        if (d.size() % l.size) fail(std::string(kindLabel(k)) + ": the size is not a multiple of " + std::to_string(l.size));
        for (; p < d.size(); p += l.size) {
            std::string line;
            layoutFields(l, &d[p], names, line);
            out += line.substr(1) + '\n';
        }
    }
    return out;
}

Bytes fromText(Kind k, std::string_view text) {
    Bytes out;
    int lineNo = 0;
    bool ended = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::vector<Token> toks = tokenize(line, lineNo);
        if (toks.empty()) continue;
        if (k == Kind::Script) {
            const Token& h = toks[0];
            if (h.type != Token::Head || !startsWith(h.text, "op_") || h.text.size() < 4 || h.text.size() > 11)
                lineFail(lineNo, "a command starts with op_XX (its opcode in hex)");
            char* end = nullptr;
            unsigned long op = std::strtoul(h.text.c_str() + 3, &end, 16);
            if (*end) lineFail(lineNo, "bad opcode '" + h.text + "'");
            if (ended) lineFail(lineNo, "commands after op_00 never run: op_00 must be the last command");
            size_t at = out.size();
            out.resize(at + 8);
            encodeArgs(toks, 1, opLayout(uint32_t(op)), out, lineNo, h.text);
            putBe32(&out[at], uint32_t(out.size() - at));
            putBe32(&out[at + 4], uint32_t(op));
            if (op == 0) ended = true;
        } else if (k == Kind::ActorGoals) {
            const Token& h = toks[0];
            Bytes name;
            std::string goal;
            if (h.type == Token::Head || h.type == Token::String) {
                goal = h.text;
                if (goal.empty() || goal.size() >= 64) lineFail(lineNo, "a goal name has 1 to 63 characters");
                name.assign(goal.begin(), goal.end());
                name.resize(64, 0);
            } else if (h.type == Token::Raw && h.raw.size() == 64) {
                name = h.raw;
            } else {
                lineFail(lineNo, "a goal starts with its name (actorMindGoalId_...)");
            }
            size_t first = 1;
            uint32_t flags = 0;
            if (toks.size() > 1 && toks[1].type == Token::Flags) {
                flags = toks[1].word;
                first = 2;
            }
            size_t at = out.size();
            out.resize(at + 4);
            out.insert(out.end(), name.begin(), name.end());
            encodeArgs(toks, first, goal.empty() ? nullptr : goalLayout(goal), out, lineNo, goal.empty() ? std::string("the goal") : goal);
            size_t size = out.size() - at;
            if (size > 0xFFFF) lineFail(lineNo, "the goal is too large");
            putBe16(&out[at], uint16_t(size));
            putBe16(&out[at + 2], uint16_t(flags));
        } else {
            encodeArgs(toks, 0, &tableLayout(k), out, lineNo, "an entry");
        }
    }
    if (k == Kind::Script && !ended) fail("script: the last command must be op_00");
    return out;
}

namespace text {

std::vector<Token> parseLine(std::string_view line, int lineNo) { return tokenize(line, lineNo); }
std::string formatWord(char type, uint32_t v, const NameLookup& names) { return wordText(type, v, names); }
std::string formatString(const uint8_t* p, uint32_t n, bool withSize) { return stringField(p, n, withSize); }
std::string formatRaw(const uint8_t* p, size_t n) { return rawToken(p, n); }

} // namespace text

std::vector<uint32_t> referencedIds(const Bytes& d, const NameLookup& names, uint32_t self) {
    std::vector<uint32_t> out;
    if (!names) return out;
    std::set<uint32_t> seen;
    for (size_t i = 0; i + 4 <= d.size(); i += 4) {
        uint32_t v = be32(&d[i]);
        if (v == 0 || v == self || (v >> 24) >= assetdb::typeCount() || seen.count(v)) continue;
        seen.insert(v);
        if (!names(v).empty()) out.push_back(v);
    }
    return out;
}

} // namespace nut::gamescript
