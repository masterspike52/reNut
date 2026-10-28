// Signing in, creating a profile, the gamerpic and photo, signing out.

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "file_picker.h"
#include "guide_dialog.h"
#include "live.h"
#include "photo.h"

namespace rexglue_guide {

using namespace ui;
using assets::Sound;

namespace {

// Photos picked in the system file dialog, prepared on the dialog's thread and
// uploaded from the Guide's.
struct PhotoPick {
  std::string png;
  std::string error;
};
std::mutex g_photo_mutex;
std::vector<PhotoPick> g_photo_picks;

void OnPhotoChosen(std::optional<std::filesystem::path> file, std::string error) {
  if (!file && error.empty()) return;  // cancelled
  PhotoPick pick;
  if (file) pick.png = photo::FromFile(*file, &pick.error);
  else pick.error = std::move(error);
  std::lock_guard lock(g_photo_mutex);
  g_photo_picks.push_back(std::move(pick));
}

}  // namespace

void GuideDialog::SignInHomeRows(std::vector<Row>& rows, Page&) {
  const bool in = backend_->signed_in();
  const std::string remembered = backend_->remembered_gamertag();
  if (!remembered.empty()) {
    rows.push_back({.label = remembered, .sub = "Sign in", .gamerpic = backend_->remembered_gamerpic(), .activate = [this] {
                      Busy("Signing in...");
                      backend_->SignInRemembered([this](const Result& r) { SignInDone(r); });
                    }});
  }
  rows.push_back({.label = "Create New Profile", .glyph = Glyph::kAddPerson, .activate = [this] { StartCreate(); }});
  rows.push_back({.label = "Sign In With Existing Profile", .glyph = Glyph::kPeople, .activate = [this] { StartSignIn(); }});
  rows.push_back({.label = "Play Offline", .activate = [this] { Close(); }});
}

void GuideDialog::StartCreate() {
  AskGamertag("Create Profile", [this](std::string gamertag) {
    OpenKeyboard({"Choose a Password", "At least 6 characters. You'll use it to sign in on other PCs.", "", true, 64,
                    [this, gamertag](std::optional<std::string> password) {
                      if (!password) return;
                      if (password->size() < 6) {
                        ShowMessage({"Create Profile", "Passwords need at least 6 characters.", {"OK"}});
                        return;
                      }
                      pending_gamertag_ = gamertag;
                      pending_password_ = *password;
                      Push({PageKind::kGamerpics, "Choose Gamerpic", "signup"});
                    }});
  });
}

void GuideDialog::StartSignIn() {
  AskGamertag("Sign In", [this](std::string gamertag) {
    OpenKeyboard({"Password", "Enter the password for " + gamertag + ".", "", true, 64,
                    [this, gamertag](std::optional<std::string> password) {
                      if (!password) return;
                      Busy("Signing in...");
                      backend_->SignIn(gamertag, *password, [this](const Result& r) { SignInDone(r); });
                    }});
  });
}

void GuideDialog::AskGamertag(const std::string& title, std::function<void(std::string)> next) {
  OpenKeyboard({title, "Enter your gamertag (up to 15 letters, numbers or spaces).", "", false, 15,
                  [this, title, next](std::optional<std::string> text) {
                    if (!text) return;
                    if (auto error = ValidateGamertag(*text); !error.empty()) {
                      ShowMessage({title, error, {"OK"}});
                      return;
                    }
                    next(*text);
                  }});
}

void GuideDialog::ChooseGamerpic(int index) {
  const std::vector<int> choices = backend_->gamerpic_choices();
  if (index < 0 || index >= int(choices.size())) return;
  const int gamerpic = choices[index];
  if (Current().arg == "signup") {
    Busy("Creating your profile...");
    backend_->CreateAccount(pending_gamertag_, pending_password_, gamerpic, [this](const Result& r) {
      pending_password_.clear();
      SignInDone(r);
    });
  } else {
    backend_->SetGamerpic(gamerpic);
    Pop();
  }
}

void GuideDialog::SignInDone(const Result& r) {
  busy_.reset();
  if (!r.ok) {
    ShowMessage({"Sign In", r.error, {"OK"}});
    return;
  }
  // One account per player: the title would see the same XUID twice.
  for (int user = 0; user < live::kMaxUsers; ++user) {
    const Backend* other = backends_[user].get();
    if (user == user_ || !other || !other->signed_in() || other->me().xuid != backend_->me().xuid) continue;
    const std::string gamertag = backend_->me().gamertag;
    backend_->SignOut();
    ShowMessage({"Sign In",
                 gamertag + " is already signed in as Player " + std::to_string(user + 1) + ". Choose another profile.",
                 {"OK"}});
    return;
  }
  if (screen_ == Screen::kSignIn) Close();
  SignedInToast();
}

void GuideDialog::SignedInToast() {
  const Profile& me = backend_->me();
  // Players 2-4 say which player they are.
  Toast t;
  t.gamerpic = me.gamerpic;
  t.line1 = me.gamertag;
  t.line2 = user_ == 0 ? "signed in" : "signed in as " + PlayerLabel();
  t.ring_player = user_;
  ShowToast(std::move(t));
}

void GuideDialog::EditMotto() {
  OpenKeyboard({"Motto", "Tell people a little about yourself.", backend_->me().motto, false, 21,
                  [this](std::optional<std::string> text) {
                    if (text) backend_->SetMotto(*text);
                  }});
}

void GuideDialog::ConfirmSignOut() {
  ShowMessage({"Sign Out", "Sign out of " + backend_->me().gamertag + "?", {"Sign Out", "Cancel"}, 0, [this](int i) {
                 if (i != 0) return;
                 backend_->SignOut();
                 for (int b = 0; b < kBladeCount; ++b) stacks_[b] = {RootPage(b)};
                 blade_ = kHomeBlade;
               }});
}

void GuideDialog::ChoosePhoto() {
  file_picker::ChoosePicture(OnPhotoChosen);
}

void GuideDialog::PumpPhotoPicks() {
  std::vector<PhotoPick> picks;
  {
    std::lock_guard lock(g_photo_mutex);
    picks.swap(g_photo_picks);
  }
  for (PhotoPick& pick : picks) {
    if (pick.png.empty()) {
      ShowMessage({"Upload Photo", pick.error, {"OK"}});
      continue;
    }
    Busy("Uploading photo...");
    backend_->UploadPhoto(std::move(pick.png), [this](const Result& r) {
      busy_.reset();
      if (!r.ok) ShowMessage({"Upload Photo", r.error, {"OK"}});
      else if (!r.note.empty()) ShowMessage({"Upload Photo", r.note, {"OK"}});
      else ShowToast({.gamerpic = backend_->me().gamerpic, .line1 = "Photo updated", .line2 = backend_->me().gamertag});
    });
  }
}

}  // namespace rexglue_guide
