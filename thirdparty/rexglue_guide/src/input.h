#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace rex::ui {
class WindowedAppContext;
}

namespace rexglue_guide {

// One frame of menu input from the pad and the keyboard. Directions repeat
// while held; everything else fires once per press.
struct NavInput {
  bool up = false, down = false, left = false, right = false;
  bool accept = false;  // A / Enter
  bool back = false;    // B / Escape
  bool x = false;       // X
  bool y = false;       // Y
  bool lb = false, rb = false;
  bool lt = false;  // pressed this frame
  bool start = false;
  bool ls = false;            // left stick click
  // The controller whose Guide button was pressed this frame: its user index
  // as the title reads it (0-3), kGuideFromSdl for the Guide's own SDL pad
  // (when the title's reads don't carry the button), or -1.
  int guide_from = -1;
  bool lb_held = false, rb_held = false;
  bool pad_used = false;      // the pad was the last device touched
  bool pad_held = false;      // any pad button or direction is down
};

constexpr int kGuideFromSdl = 4;
constexpr int kGuideFromKeyboard = 5;  // the app's keyboard pad (SetKeyboardPad)

class Input {
 public:
  // The app's UI thread, where the pads are read when the Guide draws on
  // another thread: SDL's Raw Input and Windows.Gaming.Input pads (a
  // DualSense over Bluetooth, for one) only update on a thread that pumps
  // window messages.
  void SetUiThread(rex::ui::WindowedAppContext* context) { ui_ = context; }

  // text_entry: the keyboard is typing, so letter keys aren't navigation.
  // controller: the one driving the Guide (as in NavInput::guide_from), whose
  // pad alone navigates; -1 or kGuideFromSdl: every pad.
  NavInput Poll(float dt, bool text_entry, int controller = -1);
  // The player the app's keyboard pad is for, or -1 when there is none.
  static int KeyboardPlayer();

 private:
  struct Pad {
    bool buttons[32] = {};
    float lx = 0.0f, ly = 0.0f, lt = 0.0f;
  };
  static Pad Sample();
  // The title's last read of one controller (-1: all of them together);
  // *fresh: it read that controller lately.
  static Pad GamePad(int controller, bool* fresh = nullptr);
  static Pad KeyboardPad(bool* attached = nullptr);
  static Pad Merge(const Pad& a, const Pad& b);
  Pad SdlPad() const;
  int GuideFrom(const Pad& sdl);

  struct Repeat {
    float held = -1.0f;
    float next = 0.0f;
    bool Tick(bool down, float dt);
  };
  // The last pad read on the UI thread; shared with the read queued there, so
  // it can outlive the Guide.
  struct Sampled {
    std::mutex mutex;
    Pad pad;
    std::atomic<bool> queued{false};
  };
  rex::ui::WindowedAppContext* ui_ = nullptr;
  std::shared_ptr<Sampled> sampled_ = std::make_shared<Sampled>();
  bool prev_[32] = {};
  bool prev_guide_[4] = {};  // each controller's Guide button, as the title read it
  bool prev_sdl_guide_ = false;
  bool prev_keyboard_guide_ = false;
  double last_game_guide_ = -1.0;  // seconds (Poll's clock) of a title-read Guide press
  double clock_ = 0.0;
  float prev_lt_ = 0.0f;
  Repeat up_, down_, left_, right_;
  bool pad_used_ = false;
};

}  // namespace rexglue_guide
