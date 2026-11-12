#pragma once
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <rex/platform/file.h>

namespace renut::paths {

inline bool IsAppImage() {
#if defined(__linux__)
	const char *image = std::getenv("APPIMAGE");
	return image && *image;
#else
	return false;
#endif
}

inline std::filesystem::path WritableFolder() {
	if (IsAppImage())
		return rex::platform::GetUserFolder() / "renut";
	return rex::platform::GetExecutableFolder();
}

inline void PrepareWritableFolder() {
	if (!IsAppImage())
		return;
	std::error_code ec;
	const std::filesystem::path folder = WritableFolder();
	std::filesystem::create_directories(folder, ec);
	std::filesystem::current_path(folder, ec);
}

}
