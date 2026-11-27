// Materials (docs/content_and_classes.md, Materials): the material types the game declares (rexfabric/content.h), and
// the enabled mods' materials as they were loaded, each as its editor has it while one is open, for the game to read
// with FindMaterial.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rexfabric/content.h>

#include "../mods.h"
#include "assets.h"
#include "types.h"

namespace rexfabric::content {

// A material type the game declared, its properties as a struct's fields are.
struct MaterialType {
  std::string path;
  std::string help;
  std::vector<Property> properties;
  std::string preview_component;
  AvatarValues preview_values;
  std::string preview_material;
  std::string preview_choice;
  // A graph material type's (content/material_graph.h).
  std::vector<MaterialPinDecl> graph_inputs;
  std::vector<ShadingModelDecl> shading_models;
  std::vector<MaterialNodeDecl> graph_nodes;
  std::string game_textures;
  std::function<std::vector<std::string>(const MaterialProgram& program)> check_program;

  bool HasGraph() const { return !graph_inputs.empty(); }
  const ShadingModelDecl* FindShadingModel(std::string_view name) const;
};

std::shared_ptr<const MaterialType> FindMaterialType(std::string_view path);
// The game's material types' paths, as declared.
std::vector<std::string> MaterialTypePaths();

// Reads the materials of the enabled mods. With the classes, after the types.
void LoadMaterials(const std::vector<ModInfo>& mods);

// An editor's unsaved material, read in place of the loaded one with that path until EndMaterialPreview. Any thread.
void PreviewMaterial(std::string_view path, const MaterialAsset& asset);
void EndMaterialPreview(std::string_view path);

}  // namespace rexfabric::content
