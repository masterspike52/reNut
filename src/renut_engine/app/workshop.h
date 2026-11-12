#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace renut::workshop {

constexpr const char kHost[] = "raw.githubusercontent.com";
constexpr const char kRoot[] = "/etonedemid/nutstaller-workshop/main/";

struct Entry {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    std::string created;
    std::string blueprint;
    std::string preview;
    int parts = 0;
    std::vector<std::string> needs;
};

std::vector<Entry> ParseIndex(const std::string &json, std::string *error);

std::filesystem::path SaveFolder(const std::filesystem::path &user_root);

bool IsBlueprint(const std::string &data);

bool Install(const std::filesystem::path &save, const Entry &entry, const std::string &blueprint, std::string *error);

void Remove(const std::filesystem::path &save, const std::string &id);


std::map<std::string, std::string> Installed(const std::filesystem::path &save);

std::vector<uint8_t> DecodeImage(const void *data, size_t size, int *width, int *height, int max_width = 0);


constexpr const char kSubmitUrl[] = "https://github.com/etonedemid/nutstaller-workshop/issues/new?template=submit-blueprint.yml";
constexpr size_t kMaxName = 31;
constexpr size_t kMaxDescription = 200;

struct LocalVehicle {
    std::string name;
    std::filesystem::path file;
};

std::vector<LocalVehicle> LocalVehicles(const std::filesystem::path &save);

std::string BuildSubmission(const std::filesystem::path &blueprint_file, const std::string &name, const std::string &description, const std::filesystem::path &screenshot, const std::filesystem::path &mods_folder, std::string *error);
}
