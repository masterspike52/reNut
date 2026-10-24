#pragma once
// Saves renut.toml. Unlike rex::cvar::SaveConfig, the file is written even
// with nothing modified (a setting put back to its default must leave it, or
// the next start loads the old value again), and through a temporary file, so
// a process killed mid-write (Android) leaves the old file rather
// than a truncated one.

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace renut::config {

inline void Save(const std::filesystem::path &path) {
	std::filesystem::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream file(tmp, std::ios::trunc);
		file << "# Auto-generated cvar configuration\n" << rex::cvar::SerializeToTOML();
		file.flush();
		if (!file) {
			REXLOG_ERROR("Saving {} failed", tmp.string());
			return;
		}
	}
	std::error_code ec;
	std::filesystem::rename(tmp, path, ec);
	if (ec) { // filesystems that do not rename over an existing file
		std::filesystem::remove(path, ec);
		std::filesystem::rename(tmp, path, ec);
	}
	if (ec)
		REXLOG_ERROR("Saving {}: {}", path.string(), ec.message());
}

} // namespace renut::config
