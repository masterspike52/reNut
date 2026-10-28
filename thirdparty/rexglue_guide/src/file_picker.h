#pragma once
// The system's "open file" dialog, without SDL's (rexglue builds SDL with its
// dialog support off).

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace rexglue_guide::file_picker {

// Called on the picker's own thread: the chosen file, or nullopt with `error`
// empty when cancelled and set when there's no picker to show.
using Done = std::function<void(std::optional<std::filesystem::path> file, std::string error)>;

// Shows an open dialog for pictures without blocking the caller.
void ChoosePicture(Done done);

}  // namespace rexglue_guide::file_picker
