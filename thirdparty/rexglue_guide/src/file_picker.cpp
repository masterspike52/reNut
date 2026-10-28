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

namespace rexglue_guide::file_picker {
namespace {

#if defined(_WIN32)

// The Open dialog, over `owner` (the game's window): modal to it, so it stays
// in front.
void Run(HWND owner, const Done& done) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  wchar_t file[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"Pictures (PNG, JPEG, BMP, GIF, TGA)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = DWORD(std::size(file));
  ofn.lpstrTitle = L"Choose a Photo";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
  const bool chosen = GetOpenFileNameW(&ofn);
  const DWORD error = chosen ? 0 : CommDlgExtendedError();
  if (SUCCEEDED(com)) CoUninitialize();
  if (chosen) return done(std::filesystem::path(file), "");
  done(std::nullopt, error ? "The file picker couldn't open (error " + std::to_string(error) + ")." : "");
}

#else

// The first of zenity (GNOME and most desktops) and kdialog (KDE) that's
// installed. Each prints the chosen path; exit 1 is cancelled.
void Run(const Done& done) {
  static const char* const kPickers[] = {
      "zenity --file-selection --title='Choose a Photo' "
      "--file-filter='Pictures | *.png *.jpg *.jpeg *.bmp *.gif *.tga' 2>/dev/null",
      "kdialog --title 'Choose a Photo' --getopenfilename \"$HOME\" "
      "'*.png *.jpg *.jpeg *.bmp *.gif *.tga|Pictures' 2>/dev/null",
  };
  for (const char* command : kPickers) {
    FILE* pipe = popen(command, "r");
    if (!pipe) continue;
    std::string out;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) out.append(buf, n);
    const int status = pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) continue;  // not installed
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (WEXITSTATUS(status) != 0 || out.empty()) return done(std::nullopt, "");
    return done(std::filesystem::path(out), "");
  }
  done(std::nullopt, "Choosing a photo needs zenity or kdialog installed.");
}

#endif

}  // namespace

void ChoosePicture(Done done) {
#if defined(_WIN32)
  // Picked now: the game's window is the one in front while the Guide is up.
  HWND owner = GetForegroundWindow();
  std::thread([owner, done = std::move(done)] { Run(owner, done); }).detach();
#else
  std::thread([done = std::move(done)] { Run(done); }).detach();
#endif
}

}  // namespace rexglue_guide::file_picker
