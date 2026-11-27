// The editors of a mod's meshes, worlds and packets (docs/content_and_classes.md, Meshes, Worlds and Packets): a mesh's
// help and the model beside it, which Replace takes another in place of; a world's base and help; a packet's base,
// texts, icon, look and unlock.

#pragma once

#include <memory>
#include <string>

#include "asset_editors.h"

namespace rexfabric::ui {

std::unique_ptr<AssetEditor> OpenMeshEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenWorldEditor(EditorHost& host, const std::string& path);
std::unique_ptr<AssetEditor> OpenPacketEditor(EditorHost& host, const std::string& path);

}  // namespace rexfabric::ui
