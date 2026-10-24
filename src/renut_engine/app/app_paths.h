#pragma once
// Where reNut writes its own files (renut.toml, logs, pipeline_list.bin, the
// path setup, shader captures). Next to the executable, as the SDK assumes;
// an AppImage runs from a read-only mount, so there it is the user data
// folder the SDK keeps saves in ($XDG_DATA_HOME/renut, or
// ~/.local/share/renut), and the working directory is moved there so the
// paths reNut writes relative to it land there too. On Android the executable
// is app_process: there it is the app's external files directory, which
// SDL_main makes the user data folder and the working directory
// (android_main.cpp).

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <rex/filesystem.h>

namespace renut::paths {

// The AppImage runtime sets APPIMAGE to the image's own path.
inline bool IsAppImage() {
#if defined(__linux__)
	const char *image = std::getenv("APPIMAGE");
	return image && *image;
#else
	return false;
#endif
}

// Not next to the executable.
inline bool InUserFolder() {
#if defined(__ANDROID__)
	return true;
#else
	return IsAppImage();
#endif
}

inline std::filesystem::path WritableFolder() {
#if defined(__ANDROID__)
	return rex::filesystem::GetUserFolder();
#endif
	if (IsAppImage())
		return rex::filesystem::GetUserFolder() / "renut";
	return rex::filesystem::GetExecutableFolder();
}

// Once, before anything is written: creates the folder and, for an AppImage,
// makes it the working directory.
inline void PrepareWritableFolder() {
	if (!IsAppImage())
		return;
	std::error_code ec;
	const std::filesystem::path folder = WritableFolder();
	std::filesystem::create_directories(folder, ec);
	std::filesystem::current_path(folder, ec);
}

} // namespace renut::paths
