#include "file_picker.h"

#include <iterator>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#else
#include <cstdio>
#include <sys/wait.h>
#endif

namespace rexfabric::ui::file_picker {
namespace {

// How each dialog asks for its kind of file: its title, and the filter in each picker's own form.
struct Ask {
  const wchar_t* title;
  const wchar_t* windows_filter;  // pairs of description and patterns, ending in an empty one
  const char* zenity;
  const char* kdialog;
  const char* missing;  // what's said when there's no picker installed
};

const Ask& AskFor(Files files) {
  static const Ask kPictures{
      L"Import a Picture",
      L"Pictures (PNG, JPEG, BMP, TGA, GIF)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0\0",
      "zenity --file-selection --title='Import a Picture' --file-filter='Pictures | *.png *.jpg *.jpeg *.bmp *.tga *.gif' 2>/dev/null",
      "kdialog --title 'Import a Picture' --getopenfilename \"$HOME\" '*.png *.jpg *.jpeg *.bmp *.tga *.gif|Pictures' 2>/dev/null",
      "Importing a picture needs zenity or kdialog installed",
  };
  static const Ask kMeshes{
      L"Import a Mesh",
      L"Models (binary glTF)\0*.glb\0\0",
      "zenity --file-selection --title='Import a Mesh' --file-filter='Models | *.glb' 2>/dev/null",
      "kdialog --title 'Import a Mesh' --getopenfilename \"$HOME\" '*.glb|Models' 2>/dev/null",
      "Importing a mesh needs zenity or kdialog installed",
  };
  return files == Files::kMeshes ? kMeshes : kPictures;
}

#if defined(_WIN32)

// The Open dialog over `owner` (the game's window), modal to it so it stays in front.
void Run(HWND owner, const Ask& ask, const Done& done) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  wchar_t file[MAX_PATH * 4] = {};
  OPENFILENAMEW dialog = {};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = ask.windows_filter;
  dialog.lpstrFile = file;
  dialog.nMaxFile = DWORD(std::size(file));
  dialog.lpstrTitle = ask.title;
  dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
  const bool chosen = GetOpenFileNameW(&dialog);
  const DWORD error = chosen ? 0 : CommDlgExtendedError();
  if (SUCCEEDED(com)) CoUninitialize();
  if (chosen) return done(std::filesystem::path(file), "");
  done(std::nullopt, error ? "The Open dialog couldn't open (error " + std::to_string(error) + ")" : "");
}

#else

// The first of zenity (GNOME and most desktops) and kdialog (KDE) that's installed. Each prints the path chosen; exit
// 1 is cancelled.
void Run(const Ask& ask, const Done& done) {
  for (const char* command : {ask.zenity, ask.kdialog}) {
    FILE* pipe = popen(command, "r");
    if (!pipe) continue;
    std::string out;
    char buffer[512];
    while (size_t read = fread(buffer, 1, sizeof(buffer), pipe)) out.append(buffer, read);
    const int status = pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) continue;  // not installed
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (WEXITSTATUS(status) != 0 || out.empty()) return done(std::nullopt, "");
    return done(std::filesystem::path(out), "");
  }
  done(std::nullopt, ask.missing);
}

#endif

}  // namespace

void Choose(Files files, Done done) {
  const Ask& ask = AskFor(files);
#if defined(_WIN32)
  // Taken now: the game's window is the one in front while the browser is up.
  HWND owner = GetForegroundWindow();
  std::thread([owner, &ask, done = std::move(done)] { Run(owner, ask, done); }).detach();
#else
  std::thread([&ask, done = std::move(done)] { Run(ask, done); }).detach();
#endif
}

}  // namespace rexfabric::ui::file_picker
