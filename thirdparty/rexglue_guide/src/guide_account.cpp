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
                      Push({PageKind::kChangeGamerpic, "Choose Gamerpic", "signup"});
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
  if (Current().arg == "signup" && !backend_->signed_in()) {
    Busy("Creating your profile...");
    backend_->CreateAccount(pending_gamertag_, pending_password_, gamerpic, [this](const Result& r) {
      if (r.ok) pending_password_.clear();
      SignInDone(r);
    });
  } else {
    // (Signing up: the profile was already made for another of the options.)
    backend_->SetGamerpic(gamerpic);
    BackToProfile();
  }
}

// Profile > Change Gamerpic, or signing up (arg "signup"): where the picture
// comes from.
void GuideDialog::ChangeGamerpicRows(std::vector<Row>& rows, Page& page) {
  const std::string arg = page.arg == "signup" ? "signup" : "settings";
  rows.push_back({.label = "RexLiveless Gamerpics", .activate = [this, arg] {
                    Push({PageKind::kGamerpics, "RexLiveless Gamerpics", arg});
                  }});
  // Both need the account: signing up makes it first (with the first
  // gamerpic until another is picked).
  rows.push_back({.label = "Gamerpic Database", .sub = "From xboxgamer.pics", .activate = [this] {
                    WithProfile([this] { Push({PageKind::kGamerpicGames, "Gamerpic Database", ""}); });
                  }});
  rows.push_back({.label = "Upload Custom", .detail = "256 x 256", .activate = [this] {
                    WithProfile([this] {
                      ChoosePhoto();
                      BackToProfile();  // the upload finishes on its own (PumpPhotoPicks)
                    });
                  }});
}

// Runs `next` once there's an account: signing up, it's made here first.
void GuideDialog::WithProfile(std::function<void()> next) {
  if (backend_->signed_in()) return next();
  Busy("Creating your profile...");
  backend_->CreateAccount(pending_gamertag_, pending_password_, 0, [this, next](const Result& r) {
    busy_.reset();
    if (!r.ok) {
      ShowMessage({"Create Profile", r.error, {"OK"}});
      return;
    }
    pending_password_.clear();
    next();
  });
}

// After a gamerpic is picked: back past the pages it was picked on. Signing
// up, that finishes it.
void GuideDialog::BackToProfile() {
  while (Stack().size() > 1 && (Current().kind == PageKind::kGamerpics || Current().kind == PageKind::kGamerpicGames ||
                                Current().kind == PageKind::kGamerpicGame || Current().kind == PageKind::kChangeGamerpic)) {
    Pop();
  }
  FinishSignUp();
}

// A profile made while signing up is signed in: off the sign-in screen.
void GuideDialog::FinishSignUp() {
  if (screen_ != Screen::kSignIn || !backend_->signed_in()) return;
  Close();
  SignedInToast();
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
                 ResetStacks();
                 blade_ = BladeOf(kHomeBlade);
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
