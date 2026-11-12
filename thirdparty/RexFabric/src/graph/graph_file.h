// Graphs as JSON, for files and for the clipboard.
//
//   {
//     "version": 1,
//     "variables": [{"name": "Count", "type": "int", "initial": "0", "comment": ""}],
//     "nodes": [
//       {"id": 1, "type": "event.frame", "x": 0, "y": 0},
//       {"id": 2, "type": "var.set", "x": 240, "y": 0, "settings": {"Variable": "Count"}, "values": {"Value": "1"}}
//     ],
//     "links": [{"from": {"node": 1, "pin": "Then"}, "to": {"node": 2, "pin": "Exec"}}],
//     "routines": [{"id": 1, "kind": "function", "name": "Twice", "pure": true, "comment": "",
//                   "inputs": [{"name": "Value", "type": "number"}], "outputs": [{"name": "Result", "type": "number"}]}]
//   }
//
// A node in a function's or a macro's graph says so with "owner", the routine's id.
//
// Unknown node types load as they are, so a graph opened where the game declares less keeps its nodes.

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "graph.h"

namespace rexfabric::graph {

inline constexpr int kFileVersion = 1;

std::string Write(const Graph& graph);
bool Read(std::string_view text, Graph& graph, std::string* error = nullptr);

// Some of a graph's nodes and the links between them, for copy and paste. The clipboard is plain text, so nodes
// go between graphs, and between games.
std::string WriteNodes(const Graph& graph, std::span<const int> nodes);
bool ReadNodes(std::string_view text, Graph& graph);

}  // namespace rexfabric::graph
