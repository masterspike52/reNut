#pragma once
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <windows.h>

namespace PathConfigStore {

    struct SavedPaths {
        std::filesystem::path game_data_root;
        std::filesystem::path user_data_root;
        std::filesystem::path update_data_root;
    };

    inline std::string Trim(std::string s) {
        auto l = s.find_first_not_of(" \t\r\n");
        auto r = s.find_last_not_of(" \t\r\n");
        return (l == std::string::npos) ? "" : s.substr(l, r - l + 1);
    }

    inline std::string UnquoteTrim(std::string s) {
        s = Trim(s);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            s = s.substr(1, s.size() - 2);
        return s;
    }

    inline std::string Quote(const std::string& s) {
        return "\"" + s + "\"";
    }

    inline std::filesystem::path GetStorePath(const std::string& app_name) {
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exe_path).parent_path();
        return dir / (app_name + ".cfg");
    }

    inline std::optional<SavedPaths> TryLoad(const std::string& app_name) {
        auto path = GetStorePath(app_name);
        if (!std::filesystem::exists(path))
            return std::nullopt;

        std::ifstream f(path);
        if (!f.is_open())
            return std::nullopt;

        SavedPaths cfg;
        bool has_game = false, has_user = false;

        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(line.substr(0, eq));
            std::string val = UnquoteTrim(line.substr(eq + 1));
            if (key == "game_data_root") { cfg.game_data_root = val; has_game = true; }
            else if (key == "user_data_root") { cfg.user_data_root = val; has_user = true; }
            else if (key == "update_data_root") { cfg.update_data_root = val; }
        }

        if (!has_game || !has_user)
            return std::nullopt;

        return cfg;
    }

    inline void Save(const std::string& app_name, const SavedPaths& cfg) {
        auto path = GetStorePath(app_name);
        std::ofstream f(path, std::ios::trunc);
        f << "# " << app_name << " path configuration\n";
        f << "game_data_root   = " << Quote(cfg.game_data_root.string()) << "\n";
        f << "user_data_root   = " << Quote(cfg.user_data_root.string()) << "\n";
        f << "update_data_root = " << Quote(cfg.update_data_root.string()) << "\n";
    }

} // namespace PathConfigStore