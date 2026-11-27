#include "scriptgraph.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace nut::scriptgraph {

// The nodes, one a line (scriptgraph_nodes.cpp): kind | id | category | title | inputs | outputs | lua | help.
extern const char* const kNodeSpec;

const char* const kLuaMark = "-- Made by NutTool's script graph editor";

namespace {

struct TypeName {
    PinType type;
    const char* name;
};
const TypeName kTypes[] = {{PinType::Exec, "exec"},     {PinType::Any, "any"},   {PinType::Bool, "bool"},
                           {PinType::Number, "num"},    {PinType::String, "str"}, {PinType::Vector, "vec"},
                           {PinType::Part, "part"},     {PinType::Vehicle, "vehicle"}, {PinType::Npc, "npc"},
                           {PinType::Table, "table"},   {PinType::Actor, "actor"}, {PinType::Place, "place"}};

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(uint8_t(s[a]))) ++a;
    while (b > a && std::isspace(uint8_t(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitOn(const std::string& s, char by) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t end = s.find(by, at);
        out.push_back(s.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) return out;
        at = end + 1;
    }
}

// "name:type[a/b]=default" pins, comma separated: "!" before a name makes a field, "?" after it optional,
// and [a/b] lists its dropdown's choices.
std::vector<PinDef> parsePins(const std::string& text) {
    std::vector<PinDef> out;
    for (const std::string& raw : splitOn(text, ',')) {
        std::string s = trimmed(raw);
        if (s.empty()) continue;
        PinDef p;
        if (s[0] == '!') p.field = true, s = s.substr(1);
        const size_t eq = s.find('=');
        if (eq != std::string::npos) p.value = s.substr(eq + 1), s = s.substr(0, eq);
        const size_t open = s.find('['), close = s.find(']');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            for (const std::string& c : splitOn(s.substr(open + 1, close - open - 1), '/'))
                if (!trimmed(c).empty()) p.choices.push_back(trimmed(c));
            s = s.substr(0, open) + s.substr(close + 1);
        }
        const size_t colon = s.find(':');
        if (colon != std::string::npos) {
            std::string type = trimmed(s.substr(colon + 1));
            if (!type.empty() && type.back() == '?') p.optional = true, type.pop_back();
            p.type = typeFromName(type);
            s = s.substr(0, colon);
        }
        s = trimmed(s);
        if (!s.empty() && s.back() == '?') p.optional = true, s.pop_back();
        p.name = s;
        out.push_back(p);
    }
    return out;
}

std::vector<NodeDef> buildCatalogue() {
    std::vector<NodeDef> out;
    for (const std::string& raw : splitOn(kNodeSpec, '\n')) {
        const std::string line = trimmed(raw);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = splitOn(line, '|');
        if (f.size() < 7) fail("scriptgraph: a node line has " + std::to_string(f.size()) + " fields: " + line);
        f.resize(8);
        NodeDef d;
        const char kind = trimmed(f[0]).empty() ? 'A' : trimmed(f[0])[0];
        d.kind = kind == 'E' || kind == 'C' ? Kind::Event : kind == 'P' ? Kind::Pure : kind == 'F' ? Kind::Flow
                 : kind == 'D'                ? Kind::PartDef
                                              : Kind::Action;
        d.callback = kind == 'C';
        d.id = trimmed(f[1]);
        d.category = trimmed(f[2]);
        d.title = trimmed(f[3]);
        d.inputs = parsePins(f[4]);
        d.outputs = parsePins(f[5]);
        for (size_t at = 0; (at = f[6].find("\\n", at)) != std::string::npos;) f[6].replace(at, 2, "\n");
        d.lua = trimmed(f[6]);
        d.help = trimmed(f[7]);
        if (d.kind == Kind::Event) d.outputs.insert(d.outputs.begin(), PinDef{"then", PinType::Exec});
        if (d.kind == Kind::Action) {
            d.inputs.insert(d.inputs.begin(), PinDef{"in", PinType::Exec});
            d.outputs.insert(d.outputs.begin(), PinDef{"then", PinType::Exec});
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::string luaString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out + "\"";
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || std::isdigit(uint8_t(s[0]))) return false;
    for (char c : s)
        if (!std::isalnum(uint8_t(c)) && c != '_') return false;
    static const std::set<std::string> kKeywords = {"and", "break", "do", "else", "elseif", "end", "false", "for",
                                                    "function", "goto", "if", "in", "local", "nil", "not", "or",
                                                    "repeat", "return", "then", "true", "until", "while"};
    return !kKeywords.count(s);
}

// A template's places: {pin} its Lua, {@pin} its text as typed, {,pin} ", " and its Lua when given,
// {&pin} "pin = <Lua>, " when given; {_params}, {_id} and {_body} the generator's own.
std::string expand(const std::string& t, const std::function<std::string(char, const std::string&)>& place) {
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '{') {
            size_t j = i + 1;
            char kind = 0;
            if (j < t.size() && (t[j] == '@' || t[j] == ',' || t[j] == '&')) kind = t[j++];
            const size_t start = j;
            while (j < t.size() && (std::isalnum(uint8_t(t[j])) || t[j] == '_')) ++j;
            if (j < t.size() && t[j] == '}' && j > start) {
                out += place(kind, t.substr(start, j - start));
                i = j;
                continue;
            }
        }
        out += t[i];
    }
    return out;
}

std::string pad(int indent) { return std::string(size_t(indent) * 2, ' '); }

class Gen {
public:
    Gen(const Graph& g, std::vector<std::string>& problems) : g_(g), problems_(problems) {
        for (const Link& l : g.links) {
            into_[{l.toNode, l.toPin}] = &l;
            const Node* from = g.node(l.fromNode);
            const NodeDef* d = from ? findDef(from->def) : nullptr;
            if (!d) continue;
            for (const PinDef& p : d->outputs)
                if (p.name == l.fromPin && p.type == PinType::Exec) execFrom_[{l.fromNode, l.fromPin}] = &l;
        }
    }

    std::string run(const std::string& name) {
        std::vector<std::string> blocks;
        std::vector<const Node*> order;
        for (const Node& n : g_.nodes) order.push_back(&n);
        std::sort(order.begin(), order.end(), [](const Node* a, const Node* b) { return a->id < b->id; });

        // Part definitions, each with the callbacks naming its id.
        std::map<std::string, const Node*> parts;
        for (const Node* n : order) {
            const NodeDef* d = findDef(n->def);
            if (!d || d->kind != Kind::PartDef) continue;
            const std::string id = lower(trimmed(text(*n, "id")));
            if (id.empty()) problem(*n, "has no id");
            else if (parts.count(id)) problem(*n, "defines the part '" + id + "' a second time");
            else parts[id] = n;
        }
        for (const Node* n : order) {
            const NodeDef* d = findDef(n->def);
            if (!d) {
                problem(*n, "is a node this NutTool doesn't know (" + n->def + ")");
                continue;
            }
            if (d->callback && !parts.count(lower(trimmed(text(*n, "part")))))
                problem(*n, "names the part '" + trimmed(text(*n, "part")) + "', which no Define part node in this graph defines");
            if (d->kind == Kind::Event && !d->callback) blocks.push_back(eventBlock(*n, *d, 0));
            if (d->kind == Kind::PartDef) blocks.push_back(partBlock(*n, *d, order));
        }
        for (const Node* n : order) {
            const NodeDef* d = findDef(n->def);
            if (d && (d->kind == Kind::Action || d->kind == Kind::Flow) && !reached_.count(n->id))
                problem(*n, "isn't on a white wire from an event, so it never runs");
        }

        std::string out = std::string(kLuaMark) + " from graphs/" + name + ".json: edit the graph, not this file.\n";
        if (!g_.variables.empty()) {
            out += "\n-- The graph's variables.\n";
            for (const Variable& v : g_.variables) {
                if (!isIdentifier(v.name)) {
                    problems_.push_back("the variable '" + v.name + "' needs a name of letters, digits and _ (not a Lua word)");
                    continue;
                }
                PinDef p;
                p.type = v.type;
                out += "local " + v.name + " = " + literal(p, v.value) + "\n";
            }
        }
        for (int id : onces_) out += "local __once" + std::to_string(id) + " = false\n";
        if (after_)
            out += "\n-- Delay nodes: what runs later, from the frame (outside the renut events).\n"
                   "local __later = {}\n"
                   "local function __after(seconds, fn) __later[#__later + 1] = {seconds, fn} end\n"
                   "rex.on(\"frame\", function(dt)\n"
                   "  for i = #__later, 1, -1 do\n"
                   "    local t = __later[i]\n"
                   "    t[1] = t[1] - dt\n"
                   "    if t[1] <= 0 then table.remove(__later, i); t[2]() end\n"
                   "  end\n"
                   "end)\n";
        for (const std::string& b : blocks) out += "\n" + b;
        return out;
    }

private:
    const Graph& g_;
    std::vector<std::string>& problems_;
    std::map<std::pair<int, std::string>, const Link*> into_, execFrom_;
    std::set<int> reached_;
    std::set<std::string> reported_;
    std::vector<int> onces_;
    bool after_ = false;

    std::string title(const Node& n) const {
        const NodeDef* d = findDef(n.def);
        return std::string(d ? d->title : n.def) + " (node " + std::to_string(n.id) + ")";
    }

    void problem(const Node& n, const std::string& what) {
        const std::string p = title(n) + " " + what;
        if (reported_.insert(p).second) problems_.push_back(p);
    }

    const PinDef* inputDef(const NodeDef& d, const std::string& name) const {
        for (const PinDef& p : d.inputs)
            if (p.name == name) return &p;
        return nullptr;
    }

    // What is typed into an input, else its default.
    std::string text(const Node& n, const std::string& pin) const {
        auto it = n.values.find(pin);
        if (it != n.values.end()) return it->second;
        const NodeDef* d = findDef(n.def);
        const PinDef* p = d ? inputDef(*d, pin) : nullptr;
        return p ? p->value : std::string();
    }

    std::string literal(const PinDef& p, const std::string& raw) const {
        const std::string t = trimmed(raw);
        switch (p.type) {
            case PinType::Number: return t.empty() ? "0" : t;
            case PinType::Bool: return t == "true" || t == "1" || t == "yes" ? "true" : "false";
            case PinType::String: return luaString(raw);
            case PinType::Actor:  // an object's number, or who as text
                if (t.empty()) return "nil";
                return std::isdigit(uint8_t(t[0])) ? t : luaString(t);
            case PinType::Place:
                if (t.empty()) return "nil";
                if (lower(t) == "front" || lower(t) == "banjo") return luaString(lower(t));
                [[fallthrough]];
            case PinType::Vector: {
                std::string spaced = t;
                std::replace(spaced.begin(), spaced.end(), ',', ' ');
                std::vector<std::string> parts;
                for (const std::string& s : splitOn(spaced, ' '))
                    if (!s.empty()) parts.push_back(s);
                if (parts.size() > 3) return t;
                while (parts.size() < 3) parts.push_back("0");
                return "renut.vec(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ")";
            }
            default: return t.empty() ? "nil" : t;
        }
    }

    bool given(const Node& n, const PinDef& p) const {
        return into_.count({n.id, p.name}) || !trimmed(text(n, p.name)).empty();
    }

    // An input's Lua: what is wired in, else what is typed, else its default.
    std::string input(const Node& n, const PinDef& p, std::vector<int>& path) {
        auto it = into_.find({n.id, p.name});
        if (it != into_.end()) return output(it->second->fromNode, it->second->fromPin, path);
        const std::string t = text(n, p.name);
        const bool object = p.type == PinType::Part || p.type == PinType::Vehicle || p.type == PinType::Npc;
        if (object && trimmed(t).empty() && !p.optional) problem(n, "has nothing wired into its '" + p.name + "' input");
        return literal(p, t);
    }

    std::string params(const Node& n, const NodeDef& d) const {
        std::string out;
        for (const PinDef& p : d.outputs)
            if (p.type != PinType::Exec) out += (out.empty() ? "" : ", ") + local('e', n, p.name);
        return out;
    }

    static std::string local(char kind, const Node& n, const std::string& pin) {
        return std::string(1, kind) + std::to_string(n.id) + "_" + pin;
    }

    std::string fill(const Node& n, const NodeDef& d, const std::string& t, std::vector<int>& path) {
        return expand(t, [&](char kind, const std::string& name) -> std::string {
            if (kind == 0 && name == "_params") return params(n, d);
            if (kind == 0 && name == "_id") return std::to_string(n.id);
            const PinDef* p = inputDef(d, name);
            if (!p) return "{" + std::string(kind ? std::string(1, kind) : "") + name + "}";
            if (kind == '@') return trimmed(text(n, name));
            if (kind == ',') return given(n, *p) ? ", " + input(n, *p, path) : "";
            if (kind == '&') return given(n, *p) ? name + " = " + input(n, *p, path) + ", " : "";
            if (p->optional && !given(n, *p)) return "nil";
            return input(n, *p, path);
        });
    }

    // The code of a Lua node, its {a}-{d} the inputs wired in.
    std::string code(const Node& n, const NodeDef& d, std::vector<int>& path) {
        return fill(n, d, text(n, "code"), path);
    }

    void checkVariable(const Node& n) {
        const std::string name = trimmed(text(n, "name"));
        for (const Variable& v : g_.variables)
            if (v.name == name) return;
        problem(n, "names the variable '" + name + "', which the graph doesn't have (add it under Variables)");
    }

    // The Lua giving an output's value.
    std::string output(int nodeId, const std::string& pin, std::vector<int>& path) {
        const Node* n = g_.node(nodeId);
        const NodeDef* d = n ? findDef(n->def) : nullptr;
        if (!d) return "nil";
        switch (d->kind) {
            case Kind::Event: return local('e', *n, pin);
            case Kind::Action: return local('a', *n, pin);
            case Kind::Flow: return local('f', *n, pin);
            case Kind::PartDef: return "nil";
            case Kind::Pure: break;
        }
        if (std::find(path.begin(), path.end(), n->id) != path.end()) {
            problem(*n, "feeds its own input (a loop in the wires)");
            return "nil";
        }
        path.push_back(n->id);
        if (d->id == "var.get") checkVariable(*n);
        const std::string e = d->id == "lua.expr" ? code(*n, *d, path) : fill(*n, *d, d->lua, path);
        path.pop_back();
        size_t index = 0, count = 0;
        for (const PinDef& p : d->outputs)
            if (p.type != PinType::Exec) {
                ++count;
                if (p.name == pin) index = count;
            }
        if (count > 1) return "(select(" + std::to_string(std::max<size_t>(index, 1)) + ", " + e + "))";
        return "(" + e + ")";
    }

    // The code that runs from an exec output on, `indent` deep.
    void chain(int nodeId, const std::string& pin, int indent, std::vector<std::string>& lines, std::set<std::string>& hoist,
               std::vector<int>& onPath) {
        auto it = execFrom_.find({nodeId, pin});
        if (it == execFrom_.end()) return;
        const Node* t = g_.node(it->second->toNode);
        const NodeDef* d = t ? findDef(t->def) : nullptr;
        if (!t || !d) return;
        if (std::find(onPath.begin(), onPath.end(), t->id) != onPath.end()) {
            problem(*t, "is reached again along its own white wires (a loop): use a For or While node");
            return;
        }
        onPath.push_back(t->id);
        reached_.insert(t->id);
        std::vector<int> path;
        if (d->kind == Kind::Action) {
            if (d->id == "var.set") checkVariable(*t);
            const std::string s = d->id == "lua.stmt" ? code(*t, *d, path) : fill(*t, *d, d->lua, path);
            std::string names;
            for (const PinDef& p : d->outputs)
                if (p.type != PinType::Exec) {
                    const std::string name = local('a', *t, p.name);
                    hoist.insert(name);
                    names += (names.empty() ? "" : ", ") + name;
                }
            for (const std::string& line : splitOn(names.empty() ? s : names + " = " + s, '\n')) lines.push_back(pad(indent) + line);
            chain(t->id, "then", indent, lines, hoist, onPath);
        } else if (d->kind == Kind::Flow) {
            flow(*t, *d, indent, lines, hoist, onPath);
        }
        onPath.pop_back();
    }

    void flow(const Node& n, const NodeDef& d, int indent, std::vector<std::string>& lines, std::set<std::string>& hoist,
              std::vector<int>& onPath) {
        std::vector<int> path;
        auto in = [&](const std::string& name) {
            const PinDef* p = inputDef(d, name);
            return p ? input(n, *p, path) : std::string("nil");
        };
        const std::string me = pad(indent);
        if (d.id == "flow.branch") {
            lines.push_back(me + "if " + in("condition") + " then");
            chain(n.id, "true", indent + 1, lines, hoist, onPath);
            std::vector<std::string> otherwise;
            chain(n.id, "false", indent + 1, otherwise, hoist, onPath);
            if (!otherwise.empty()) {
                lines.push_back(me + "else");
                lines.insert(lines.end(), otherwise.begin(), otherwise.end());
            }
            lines.push_back(me + "end");
        } else if (d.id == "flow.sequence") {
            for (const char* pin : {"then1", "then2", "then3", "then4"}) chain(n.id, pin, indent, lines, hoist, onPath);
        } else if (d.id == "flow.foreach") {
            lines.push_back(me + "for " + local('f', n, "index") + ", " + local('f', n, "item") + " in ipairs(" + in("list") + ") do");
            chain(n.id, "loop", indent + 1, lines, hoist, onPath);
            lines.push_back(me + "end");
            chain(n.id, "done", indent, lines, hoist, onPath);
        } else if (d.id == "flow.for") {
            lines.push_back(me + "for " + local('f', n, "index") + " = " + in("from") + ", " + in("to") + ", " + in("step") + " do");
            chain(n.id, "loop", indent + 1, lines, hoist, onPath);
            lines.push_back(me + "end");
            chain(n.id, "done", indent, lines, hoist, onPath);
        } else if (d.id == "flow.while") {
            lines.push_back(me + "while " + in("condition") + " do");
            chain(n.id, "loop", indent + 1, lines, hoist, onPath);
            lines.push_back(me + "end");
            chain(n.id, "done", indent, lines, hoist, onPath);
        } else if (d.id == "flow.delay") {
            after_ = true;
            lines.push_back(me + "__after(" + in("seconds") + ", function()");
            chain(n.id, "then", indent + 1, lines, hoist, onPath);
            lines.push_back(me + "end)");
        } else if (d.id == "flow.once") {
            const std::string flag = "__once" + std::to_string(n.id);
            if (std::find(onces_.begin(), onces_.end(), n.id) == onces_.end()) onces_.push_back(n.id);
            lines.push_back(me + "if not " + flag + " then");
            lines.push_back(pad(indent + 1) + flag + " = true");
            chain(n.id, "then", indent + 1, lines, hoist, onPath);
            lines.push_back(me + "end");
        } else if (d.id == "flow.return") {
            lines.push_back(me + "do return end");
        } else if (d.id == "hud.choice") {
            // The game's box with Confirm and Cancel: each wire runs when the player answers.
            const std::string yes = "__yes" + std::to_string(n.id);
            lines.push_back(me + "renut.hud.choice(" + in("text") + ", function(" + yes + ")");
            lines.push_back(pad(indent + 1) + "if " + yes + " then");
            chain(n.id, "confirm", indent + 2, lines, hoist, onPath);
            std::vector<std::string> cancel;
            chain(n.id, "cancel", indent + 2, cancel, hoist, onPath);
            if (!cancel.empty()) {
                lines.push_back(pad(indent + 1) + "else");
                lines.insert(lines.end(), cancel.begin(), cancel.end());
            }
            lines.push_back(pad(indent + 1) + "end");
            lines.push_back(me + "end)");
        }
    }

    // An event's body: the code on its "then" wire, its actions' values declared first.
    std::vector<std::string> body(const Node& n) {
        std::vector<std::string> lines;
        std::set<std::string> hoist;
        std::vector<int> onPath{n.id};
        chain(n.id, "then", 0, lines, hoist, onPath);
        if (!hoist.empty()) {
            std::string decl = "local ";
            bool first = true;
            for (const std::string& h : hoist) decl += (first ? "" : ", ") + h, first = false;
            lines.insert(lines.begin(), decl);
        }
        return lines;
    }

    // An event's template with its body where {_body} is, at that line's indentation, `indent` deeper.
    std::string eventBlock(const Node& n, const NodeDef& d, int indent) {
        const std::vector<std::string> b = body(n);
        std::vector<int> path;
        std::string out;
        for (const std::string& line : splitOn(d.lua, '\n')) {
            const size_t at = line.find("{_body}");
            if (at != std::string::npos) {
                const std::string lead = line.substr(0, at);
                for (const std::string& bl : b) out += pad(indent) + lead + bl + "\n";
                continue;
            }
            out += pad(indent) + fill(n, d, line, path) + "\n";
        }
        return out;
    }

    std::string partBlock(const Node& n, const NodeDef& d, const std::vector<const Node*>& order) {
        std::vector<int> path;
        std::string out = "renut.parts.add{\n";
        for (const PinDef& p : d.inputs) {
            if (!given(n, p)) continue;
            const std::string t = trimmed(text(n, p.name));
            if (p.name == "base_action" && t != "true") continue;
            std::string value = input(n, p, path);
            if (p.name == "button" && (lower(t) == "none" || lower(t) == "false")) value = "false";
            out += "  " + p.name + " = " + value + ",\n";
        }
        const std::string id = lower(trimmed(text(n, "id")));
        for (const Node* c : order) {
            const NodeDef* cd = findDef(c->def);
            if (cd && cd->callback && lower(trimmed(text(*c, "part"))) == id) out += eventBlock(*c, *cd, 1);
        }
        return out + "}\n";
    }
};

} // namespace

const char* typeName(PinType t) {
    for (const TypeName& n : kTypes)
        if (n.type == t) return n.name;
    return "any";
}

PinType typeFromName(const std::string& name) {
    for (const TypeName& n : kTypes)
        if (name == n.name) return n.type;
    return PinType::Any;
}

bool compatible(PinType from, PinType to) {
    if (from == PinType::Exec || to == PinType::Exec) return from == to;
    // A place takes a vector or text; a character is a number (its object) or text (who).
    if (to == PinType::Place && (from == PinType::Vector || from == PinType::String)) return true;
    if (to == PinType::Actor && (from == PinType::Number || from == PinType::String)) return true;
    if (from == PinType::Actor && to == PinType::Number) return true;
    return from == to || from == PinType::Any || to == PinType::Any;
}

const std::vector<NodeDef>& catalogue() {
    static const std::vector<NodeDef> all = buildCatalogue();
    return all;
}

const NodeDef* findDef(const std::string& id) {
    static const std::map<std::string, const NodeDef*> byId = [] {
        std::map<std::string, const NodeDef*> m;
        for (const NodeDef& d : catalogue()) m[d.id] = &d;
        return m;
    }();
    auto it = byId.find(id);
    return it == byId.end() ? nullptr : it->second;
}

Node* Graph::node(int id) {
    for (Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

const Node* Graph::node(int id) const {
    for (const Node& n : nodes)
        if (n.id == id) return &n;
    return nullptr;
}

Graph load(const fs::path& json) {
    const Json j = Json::parse(readText(json));
    Graph g;
    if (j.has("nextId")) g.nextId = int(j["nextId"].asNumber());
    if (j.has("variables"))
        for (const Json& v : j["variables"].items())
            g.variables.push_back({v["name"].asString(), typeFromName(v["type"].asString()), v.has("value") ? v["value"].asString() : ""});
    if (j.has("nodes"))
        for (const Json& o : j["nodes"].items()) {
            Node n;
            n.id = int(o["id"].asNumber());
            n.def = o["def"].asString();
            n.x = float(o["x"].asNumber());
            n.y = float(o["y"].asNumber());
            if (o.has("values"))
                for (const auto& [k, v] : o["values"].members()) n.values[k] = v.asString();
            g.nextId = std::max(g.nextId, n.id + 1);
            g.nodes.push_back(std::move(n));
        }
    if (j.has("links"))
        for (const Json& o : j["links"].items()) {
            Link l;
            l.id = int(o["id"].asNumber());
            l.fromNode = int(o["from"][size_t(0)].asNumber());
            l.fromPin = o["from"][size_t(1)].asString();
            l.toNode = int(o["to"][size_t(0)].asNumber());
            l.toPin = o["to"][size_t(1)].asString();
            g.nextId = std::max(g.nextId, l.id + 1);
            g.links.push_back(std::move(l));
        }
    return g;
}

std::string toJson(const Graph& g) {
    Json root = Json::object();
    root["graph"] = 1;
    root["nextId"] = g.nextId;
    Json vars = Json::array();
    for (const Variable& v : g.variables) {
        Json o = Json::object();
        o["name"] = v.name;
        o["type"] = typeName(v.type);
        o["value"] = v.value;
        vars.push(o);
    }
    root["variables"] = vars;
    Json nodes = Json::array();
    for (const Node& n : g.nodes) {
        Json o = Json::object();
        o["id"] = n.id;
        o["def"] = n.def;
        o["x"] = double(n.x);
        o["y"] = double(n.y);
        Json values = Json::object();
        for (const auto& [k, v] : n.values) values[k] = v;
        o["values"] = values;
        nodes.push(o);
    }
    root["nodes"] = nodes;
    Json links = Json::array();
    for (const Link& l : g.links) {
        Json o = Json::object(), from = Json::array(), to = Json::array();
        o["id"] = l.id;
        from.push(l.fromNode), from.push(l.fromPin);
        to.push(l.toNode), to.push(l.toPin);
        o["from"] = from;
        o["to"] = to;
        links.push(o);
    }
    root["links"] = links;
    return root.dump() + "\n";
}

std::string toLua(const Graph& g, const std::string& name, std::vector<std::string>& problems) {
    return Gen(g, problems).run(name);
}

} // namespace nut::scriptgraph
