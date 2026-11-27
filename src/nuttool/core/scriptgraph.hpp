// Script graphs for reNut mods: nodes wired together as in a visual scripting editor, made into the
// Lua reNut runs. The GUI edits them (gui/script_graph.cpp); scriptgraph_nodes.cpp holds the nodes.
#pragma once

#include "common.hpp"

#include <map>

namespace nut::scriptgraph {

// What a pin carries. Exec pins are the white wires: what runs after what. An actor is any character (an
// object, or who as text: nearest, last, banjo, a name); a place is a vector, "front" or "banjo".
enum class PinType { Exec, Any, Bool, Number, String, Vector, Part, Vehicle, Npc, Table, Actor, Place };

const char* typeName(PinType t);
PinType typeFromName(const std::string& name);  // Any for an unknown one
// Whether an output of type `from` may be wired into an input of type `to`.
bool compatible(PinType from, PinType to);

struct PinDef {
    std::string name;
    PinType type = PinType::Any;
    std::string value;      // its default, as typed in the node
    bool field = false;     // a setting in the node, never wired
    bool optional = false;  // left out of the Lua when not wired and empty
    std::vector<std::string> choices;  // what its dropdown offers (it can still be typed)
};

// Events start the white wires; actions run in them; pure nodes only give values; flow nodes steer
// them; a part definition gathers the part callbacks naming its id.
enum class Kind { Event, Action, Pure, Flow, PartDef };

struct NodeDef {
    std::string id, category, title, help;
    Kind kind = Kind::Action;
    std::vector<PinDef> inputs, outputs;  // exec pins included
    std::string lua;                      // the Lua it becomes, with {pin} places (scriptgraph.cpp)
    bool callback = false;                // a part callback (inside its part's renut.parts.add)
};

// Every node there is, by category in the order the menus show them.
const std::vector<NodeDef>& catalogue();
const NodeDef* findDef(const std::string& id);

struct Node {
    int id = 0;
    std::string def;
    float x = 0, y = 0;
    std::map<std::string, std::string> values;  // what is typed into its inputs, by name
};

struct Link {
    int id = 0;
    int fromNode = 0, toNode = 0;
    std::string fromPin, toPin;  // an output, an input
};

// A graph variable: a local of the script, kept between events.
struct Variable {
    std::string name;
    PinType type = PinType::Number;
    std::string value;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Variable> variables;
    int nextId = 1;

    Node* node(int id);
    const Node* node(int id) const;
};

Graph load(const fs::path& json);
std::string toJson(const Graph& g);

// The script's Lua. `problems` gets what keeps it from working as drawn (a part object input with
// nothing wired in, a callback without its part, a loop in the white wires...).
std::string toLua(const Graph& g, const std::string& name, std::vector<std::string>& problems);

// The first line of every script a graph makes, so it is known not to be hand-written.
extern const char* const kLuaMark;

} // namespace nut::scriptgraph
