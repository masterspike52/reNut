// The system's Open dialog, for importing a file into the content browser. rexglue builds SDL without its dialogs, so
// this is Windows' own dialog, or zenity or kdialog elsewhere. It runs on a thread of its own, so the game keeps
// drawing while it's up.

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace rexfabric::ui::file_picker {

// Called on the picker's thread: the file chosen, or none, with `error` empty when the player cancelled and set when
// there's no dialog to show.
using Done = std::function<void(std::optional<std::filesystem::path> file, std::string error)>;

// What the dialog offers: pictures for a texture, models for a mesh.
enum class Files { kPictures, kMeshes };

// Shows an Open dialog for files of one kind without blocking the caller.
void Choose(Files files, Done done);
inline void ChoosePicture(Done done) { Choose(Files::kPictures, std::move(done)); }

}  // namespace rexfabric::ui::file_picker
