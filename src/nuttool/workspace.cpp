#include "nuttool/workspace.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <stdexcept>

#include <rex/string/text.h>

#include "nbpatch.hpp"
#include "nuttool/studio.h"
#include "renut_engine/mods/nbpatch.h"
#include "renut_engine/mods/zip_reader.h"

namespace renut::nuttool {
namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
fs::path g_location;  // the open mod's folder or loose package
fs::path g_work;      // where its edits go

std::string Utf8(const fs::path &p) { return rex::string::PathToUTF8(p); }

fs::path Scratch(const fs::path &) { return NutToolFolder() / "scratch"; }

// A folder mod's package: its only .nbpatch, else one named after the folder.
fs::path FolderPackage(const fs::path &folder) {
  std::error_code ec;
  std::vector<fs::path> found;
  for (const auto &entry : fs::directory_iterator(folder, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".nbpatch")
      found.push_back(entry.path());
  if (found.size() == 1)
    return found.front();
  return folder / (nut::nbpatch::slug(Utf8(folder.filename())) + ".nbpatch");
}

fs::path PackageOf(const fs::path &location) {
  return location.extension() == ".nbpatch" ? location : FolderPackage(location);
}

fs::path WorkOf(const fs::path &, const fs::path &location) {
  if (location.extension() == ".nbpatch")
    return NutToolFolder() / "work" / location.stem();
  return location / "nuttool";
}

bool HasFiles(const fs::path &dir) {
  std::error_code ec;
  for (const auto &entry : fs::recursive_directory_iterator(dir, ec))
    if (entry.is_regular_file(ec))
      return true;
  return false;
}

bool WriteBytes(const fs::path &file, const std::vector<uint8_t> &data) {
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
  return bool(out);
}

// A game file as it is on disk, whatever workspace NutTool has (UnpackMod runs while one is open).
nut::Bytes ReadOriginal(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    throw std::runtime_error("can't read " + Utf8(file));
  return nut::Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The package's changed files, rebuilt into `work` as NutTool reads them (expanded): a delta against the game's file.
bool Expand(const fs::path &game, const fs::path &package, const fs::path &work, std::string *error) {
  renut::mods::ZipReader zip;
  if (!zip.Open(package, error))
    return false;
  const auto manifest = renut::mods::ReadManifest(zip, error);
  if (!manifest)
    return false;
  for (const renut::mods::PatchFile &f : manifest->files) {
    if (f.kind == "xexmods")
      continue;
    const auto stored = zip.Read(f.entry, error);
    if (!stored)
      return false;
    std::vector<uint8_t> target;
    if (f.kind == "new") {
      target = *stored;
    } else {
      nut::Bytes original;
      try {
        original = nut::nbpatch::expand(ReadOriginal(game / rex::string::to_path(f.path)));
      } catch (const std::exception &e) {
        *error = f.path + ": " + e.what();
        return false;
      }
      auto rebuilt = renut::mods::ApplyDelta(original, *stored, error);
      if (!rebuilt)
        return false;
      target = std::move(*rebuilt);
    }
    if (!WriteBytes(work / rex::string::to_path(f.path), target)) {
      *error = "can't write " + Utf8(work / rex::string::to_path(f.path));
      return false;
    }
  }
  return true;
}

// Moves the scratch workspace's files into `work`, keeping what's already there.
void TakeScratch(const fs::path &scratch, const fs::path &work) {
  std::error_code ec;
  if (!fs::is_directory(scratch, ec))
    return;
  for (const auto &entry : fs::recursive_directory_iterator(scratch, ec)) {
    if (!entry.is_regular_file(ec))
      continue;
    const fs::path to = work / entry.path().lexically_relative(scratch);
    if (fs::exists(to, ec))
      continue;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(entry.path(), to, ec);
    if (ec) {
      ec.clear();
      fs::copy_file(entry.path(), to, ec);
    }
  }
  fs::remove_all(scratch, ec);
}

} // namespace

std::vector<ModEntry> ListMods(const fs::path &mods) {
  std::vector<ModEntry> list;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(mods, ec)) {
    const std::string name = Utf8(entry.path().filename());
    if (name.empty() || name[0] == '.')
      continue;
    if (entry.is_directory(ec))
      list.push_back({name, entry.path(), false});
    else if (entry.is_regular_file(ec) && entry.path().extension() == ".nbpatch")
      list.push_back({name, entry.path(), true});
  }
  std::sort(list.begin(), list.end(), [](const ModEntry &a, const ModEntry &b) { return a.name < b.name; });
  return list;
}

bool OpenMod(const fs::path &game, const fs::path &mods, const fs::path &location, std::string *error) {
  const fs::path work = WorkOf(mods, location);
  const fs::path package = PackageOf(location);
  std::error_code ec;
  if (location.extension() != ".nbpatch")
    fs::create_directories(location, ec);
  // Its package's files, the first time it's opened (later its workspace holds them, with any newer edits).
  if (!HasFiles(work) && fs::is_regular_file(package, ec)) {
    nut::setWorkspace({}, {});
    const bool expanded = Expand(game, package, work, error);
    if (!expanded) {
      fs::remove_all(work, ec);
      nut::setWorkspace(game, Scratch(mods));
      return false;
    }
  }
  fs::create_directories(work, ec);
  TakeScratch(Scratch(mods), work);
  std::lock_guard lock(g_mutex);
  g_location = location;
  g_work = work;
  nut::setWorkspace(game, work);
  return true;
}

bool UnpackMod(const fs::path &game, const fs::path &location, const fs::path &out, std::string *error) {
  const fs::path package = PackageOf(location);
  std::error_code ec;
  if (!fs::is_regular_file(package, ec)) {
    *error = "it has no .nbpatch";
    return false;
  }
  const std::string stamp = std::to_string(fs::file_size(package, ec)) + ":" +
                            std::to_string(fs::last_write_time(package, ec).time_since_epoch().count());
  const fs::path stampFile = out / ".unpacked";
  {
    std::ifstream in(stampFile);
    std::string had;
    if (std::getline(in, had) && had == stamp)
      return true;
  }
  fs::remove_all(out, ec);
  if (!Expand(game, package, out, error)) {
    fs::remove_all(out, ec);
    return false;
  }
  fs::create_directories(out, ec);
  std::ofstream(stampFile) << stamp << '\n';
  return true;
}

void CloseMod(const fs::path &game, const fs::path &mods) {
  std::lock_guard lock(g_mutex);
  g_location.clear();
  g_work.clear();
  nut::setWorkspace(game, Scratch(mods));
}

fs::path OpenModLocation() {
  std::lock_guard lock(g_mutex);
  return g_location;
}

std::string OpenModName() {
  std::lock_guard lock(g_mutex);
  return g_location.empty() ? std::string() : Utf8(g_location.extension() == ".nbpatch" ? g_location.stem() : g_location.filename());
}

bool SaveMod(const fs::path &game, const std::string &author, std::vector<std::string> *log) {
  fs::path location, work;
  {
    std::lock_guard lock(g_mutex);
    location = g_location;
    work = g_work;
  }
  if (location.empty()) {
    log->push_back("open a mod first (Mod > Open or New)");
    return false;
  }
  const fs::path package = PackageOf(location);
  const std::string name = OpenModName();
  // Its files against the game's own: the workspace is off while they're read, so the game's are the originals.
  nut::setWorkspace({}, {});
  bool written = false;
  try {
    std::vector<nut::nbpatch::Change> changes;
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(work, ec)) {
      if (!entry.is_regular_file(ec))
        continue;
      const fs::path rel = entry.path().lexically_relative(work);
      const fs::path original = game / rel;
      nut::nbpatch::Change change{rel.generic_string(), fs::is_regular_file(original, ec) ? original : fs::path(),
                                  entry.path()};
      if (!change.original.empty() &&
          nut::nbpatch::expand(nut::readFile(change.original)) == nut::readFile(change.modified))
        continue;  // put back as it was
      changes.push_back(std::move(change));
    }
    nut::nbpatch::Info info;
    info.name = name;
    info.author = author;
    nut::ops::Progress progress;
    progress.log = [log](const std::string &line) { log->push_back(line); };
    if (changes.empty()) {
      fs::remove(package, ec);
      log->push_back(name + ": nothing differs from the game, so it has no package");
      written = true;
    } else {
      written = nut::nbpatch::write(package, info, changes, progress);
      if (written)
        log->push_back("wrote " + Utf8(package) + " (" + std::to_string(changes.size()) + " file(s))");
    }
    if (written && location.extension() != ".nbpatch" && !fs::exists(location / "mod.toml", ec)) {
      std::ofstream toml(location / "mod.toml", std::ios::trunc);
      toml << "name = \"" << name << "\"\nversion = \"1\"\nauthor = \"" << author << "\"\n";
    }
  } catch (const std::exception &e) {
    log->push_back(std::string("couldn't save ") + name + ": " + e.what());
    written = false;
  }
  nut::setWorkspace(game, work);
  return written;
}

} // namespace renut::nuttool
