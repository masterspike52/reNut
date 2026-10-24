#pragma once
// Android: custom Vulkan drivers (Turnip and friends, in the AdrenoTools
// zip format: meta.json + the driver .so) loaded through libadrenotools, plus
// the few calls into the Java activity the manager needs.
//
// Installed drivers live in the app's internal files folder (dlopen refuses
// shared storage): <files>/drivers/<id>/. The selection is in
// <files>/driver.txt; empty or missing means the system driver.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace renut::android {

struct Driver {
    std::string id;           // folder name under drivers/
    std::string name;         // meta.json name (+ version)
    std::string description;
    std::string library;      // meta.json libraryName
};

std::filesystem::path DriversFolder();
std::vector<Driver> InstalledDrivers();
// AdrenoTools driver zips waiting in /sdcard/reNut/drivers.
std::vector<std::filesystem::path> DriverZips();
// Unpacks a driver zip into DriversFolder(); the new id, or empty with *error.
std::string InstallDriverZip(const std::filesystem::path &zip, std::string *error);
void RemoveDriver(const std::string &id);

std::string SelectedDriver();
void SelectDriver(const std::string &id);

// Before the Vulkan interface is created: loads the selected driver and points
// volk at it. A driver that took the last run down (no MarkDriverWorking) is
// dropped for the system driver.
void LoadVulkanDriver();
// Once the device is up with the loaded driver.
void MarkDriverWorking();
// What LoadVulkanDriver did, for the manager.
const std::string &DriverStatus();
// The id LoadVulkanDriver loaded ("" = system driver).
const std::string &LoadedDriver();

// Java side (RenutActivity).
bool HasStorageAccess();
void RequestStorageAccess();
// Starts a new process: a driver is loaded once per process.
void RestartApp();

// RenutActivity's static helpers, callable from any thread.
std::optional<std::vector<uint8_t>> HttpGet(const std::string &url);
// A PNG or JPEG as RGBA, scaled down to at most max_width; empty on failure.
std::vector<uint8_t> DecodeImage(const std::vector<uint8_t> &data, int max_width, int &width,
                                 int &height);
std::string Sha256(const std::vector<uint8_t> &data);  // lowercase hex
// Both return "" on success, else the error.
std::string ZipFolder(const std::filesystem::path &folder, const std::filesystem::path &zip);
std::string Unzip(const std::filesystem::path &zip, const std::filesystem::path &folder);

} // namespace renut::android
