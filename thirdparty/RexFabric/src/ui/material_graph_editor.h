// The material graph editor (docs/materials.md, The editor): a graph material's nodes on a canvas, after Unreal's
// material editor. On the left the game's preview of the material (the Avatar editor's viewport) above the details of
// the material or of the node selected; in the middle the canvas, with the code the graph makes and its problems
// below it. Every change shows in the preview and wherever the material is drawn at once, saved or not.
//
// On the canvas a right click adds a node, dragging a link into empty space adds one connected to it, and unlinked
// inputs are typed into on the node. Delete removes the selection (not the Material node); Ctrl+C, Ctrl+X, Ctrl+V and
// Ctrl+D copy, cut, paste and duplicate. Ctrl+S saves, Ctrl+Z and Ctrl+Y undo and redo.

#pragma once

#include <memory>
#include <string>

#include "../content/assets.h"
#include "asset_editors.h"

namespace rexfabric::ui {

// The editor of a material of a graph material type.
std::unique_ptr<AssetEditor> OpenMaterialGraphEditor(EditorHost& host, const std::string& path, content::MaterialAsset asset);

}  // namespace rexfabric::ui
