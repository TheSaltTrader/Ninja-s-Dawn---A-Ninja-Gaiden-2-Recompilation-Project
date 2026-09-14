#include "ng2_autoskip.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace ng2 {

// The SDK keeps these in rex:: and rex::input::; pulled in here so the driver
// below reads like the SDK's own drivers do.
using rex::X_STATUS;
using rex::X_RESULT;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_VIBRATION;
using rex::input::X_INPUT_KEYSTROKE;

namespace {

// How long a chapter load keeps the skip armed. Long enough to cover an opening
// cinematic, short enough that it cannot still be running once the player has
// control - which in this game would mean holding attack.
constexpr double kArmedSeconds = 25.0;

// A press every ~12 frames at 60Hz. Fast enough to get through a prompt that
// wants several presses, slow enough to look like presses rather than a stuck
// button - some menus ignore a button that never releases.
constexpr double kPressPeriod = 0.20;
constexpr double kPressHold = 0.08;

constexpr uint16_t kButtonA = 0x1000;
constexpr uint16_t kButtonStart = 0x0010;

std::atomic<bool> g_enabled{false};
std::atomic<double> g_armed_until{-1.0};

double Now() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

// --- Scripted playback (NG2_PAD_SCRIPT) --------------------------------------
//
// A launch-time script that drives the guest pad through menus with no human -
// the same in-process approach Fable II uses. Because the SDK ORs every device
// assigned to a player, these presses reach player 1 even when a real
// controller is connected (an idle real pad does not disarm it; RealPadActive
// only yields on genuine stick/button activity). Format, comma-separated:
//   "autoskip:N"  hammer A+Start until N seconds (skips intros/prompts)
//   "T:button"    press <button> at T seconds for a short window
// buttons: a b x y start back up down left right lb rb ls rs.
uint16_t ButtonMask(const std::string& n) {
  if (n == "a") return 0x1000;
  if (n == "b") return 0x2000;
  if (n == "x") return 0x4000;
  if (n == "y") return 0x8000;
  if (n == "start") return 0x0010;
  if (n == "back") return 0x0020;
  if (n == "up") return 0x0001;
  if (n == "down") return 0x0002;
  if (n == "left") return 0x0004;
  if (n == "right") return 0x0008;
  if (n == "lb") return 0x0100;
  if (n == "rb") return 0x0200;
  if (n == "ls") return 0x0040;
  if (n == "rs") return 0x0080;
  return 0;
}

struct ScriptPress {
  double t;
  uint16_t mask;
  bool fired = false;
};
std::vector<ScriptPress> g_script;
double g_autoskip_until = -1.0;
bool g_script_parsed = false;
bool g_has_script = false;
constexpr double kScriptHold = 0.18;  // how long each scripted press is held

void ParsePadScriptOnce() {
  if (g_script_parsed) return;
  g_script_parsed = true;
  const char* s = std::getenv("NG2_PAD_SCRIPT");
  if (!s || !*s) return;
  const std::string in(s);
  size_t i = 0;
  while (i < in.size()) {
    const size_t comma = in.find(',', i);
    const std::string tok =
        in.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    i = (comma == std::string::npos) ? in.size() : comma + 1;
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) continue;
    const std::string a = tok.substr(0, colon), b = tok.substr(colon + 1);
    if (a == "autoskip") {
      g_autoskip_until = std::atof(b.c_str());
    } else {
      const uint16_t m = ButtonMask(b);
      if (m) g_script.push_back({std::atof(a.c_str()), m});
    }
  }
  g_has_script = (g_autoskip_until > 0.0) || !g_script.empty();
  if (g_has_script)
    REXLOG_INFO("Pad script: {} presses, autoskip until {:.0f}s (NG2_PAD_SCRIPT)",
                g_script.size(), g_autoskip_until);
}

uint16_t ScriptButtons(double now) {
  ParsePadScriptOnce();
  if (!g_has_script) return 0;
  uint16_t b = 0;
  if (now < g_autoskip_until) {
    const double phase = std::fmod(now, kPressPeriod);
    if (phase < kPressHold) b |= kButtonA | kButtonStart;
  }
  for (auto& p : g_script) {
    if (now >= p.t && now < p.t + kScriptHold) {
      if (!p.fired) {
        p.fired = true;
        REXLOG_INFO("[padscript] {:.1f} s: buttons 0x{:04X}", now, p.mask);
      }
      b |= p.mask;
    }
  }
  return b;
}

// Is the player actually touching a controller?
//
// Polled through XInput directly rather than through the input system, and that
// is the point: the synthetic pad is NOT an XInput device, so this sees only
// real hardware and cannot feed back on itself. Loaded dynamically because the
// version present differs across Windows installs and a missing DLL should cost
// the feature, not the process.
bool RealPadActive() {
  struct XInputGamepad {
    uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry;
  };
  struct XInputState { uint32_t packet; XInputGamepad pad; };
  using GetStateFn = uint32_t(WINAPI*)(uint32_t, XInputState*);

  static GetStateFn get_state = [] () -> GetStateFn {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryW(dll))
        return reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
    }
    return nullptr;
  }();
  if (!get_state)
    return false;

  constexpr int16_t kStickDeadzone = 12000;   // well past drift
  for (uint32_t user = 0; user < 4; ++user) {
    XInputState st{};
    if (get_state(user, &st) != ERROR_SUCCESS)
      continue;
    const XInputGamepad& g = st.pad;
    if (g.buttons || g.lt > 40 || g.rt > 40 ||
        std::abs(int(g.lx)) > kStickDeadzone || std::abs(int(g.ly)) > kStickDeadzone ||
        std::abs(int(g.rx)) > kStickDeadzone || std::abs(int(g.ry)) > kStickDeadzone) {
      return true;
    }
  }
  return false;
}

// A single device, so its handle is a constant. Distinct from the SDK's own
// driver ids.
constexpr rex::input::DeviceId kSkipDevice =
    static_cast<rex::input::DeviceId>(0x4E473253);  // 'NG2S'

class AutoSkipDriver final : public rex::input::InputDriver {
 public:
  AutoSkipDriver() : InputDriver(nullptr, 0) {}
  ~AutoSkipDriver() override = default;

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kSkipDevice;
    info.name = "Auto-skip";
    // Synthetic, so the assignment logic knows it is not a physical pad and
    // does not count it as "a controller is plugged in".
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kSkipDevice)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out_state)
      return X_ERROR_BAD_ARGUMENTS;
    std::memset(out_state, 0, sizeof(*out_state));
    // The packet number has to move or the guest may treat the state as stale.
    out_state->packet_number = ++packet_;
    // Checked here, at the guest's own polling rate, so a real press stops the
    // synthetic ones within a frame rather than within a sampling interval.
    if (RealPadActive())
      NoteRealInput();
    // A launch-time pad script (NG2_PAD_SCRIPT) plays independently of the
    // chapter arm, so a scripted run can navigate menus with no human.
    uint16_t buttons = ScriptButtons(Now());
    if (AutoSkipActive()) {
      const double phase = std::fmod(Now(), kPressPeriod);
      if (phase < kPressHold) buttons |= kButtonA | kButtonStart;
    }
    out_state->gamepad.buttons = buttons;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kSkipDevice)
      return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xF3FF;  // all buttons, so a script can use any
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId, X_INPUT_VIBRATION*) override {
    return X_ERROR_SUCCESS;  // nothing to rumble
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                              X_INPUT_KEYSTROKE*) override {
    return X_ERROR_EMPTY;
  }

 private:
  uint32_t packet_ = 0;
};

}  // namespace

void SetAutoSkipEnabled(bool enabled) {
  g_enabled.store(enabled, std::memory_order_release);
  if (!enabled)
    g_armed_until.store(-1.0, std::memory_order_release);
}

void ArmAutoSkip() {
  if (!g_enabled.load(std::memory_order_acquire))
    return;
  g_armed_until.store(Now() + kArmedSeconds, std::memory_order_release);
  REXLOG_INFO("Auto-skip: armed for {:.0f}s (chapter loading)", kArmedSeconds);
}

void NoteRealInput() {
  // Disarm rather than pause. Someone who reached for the pad during a
  // cinematic wants to watch it, and the next chapter will arm it again.
  if (g_armed_until.load(std::memory_order_acquire) > 0.0) {
    g_armed_until.store(-1.0, std::memory_order_release);
    REXLOG_INFO("Auto-skip: disarmed - the player pressed something");
  }
}

bool AutoSkipActive() {
  if (!g_enabled.load(std::memory_order_acquire))
    return false;
  const double until = g_armed_until.load(std::memory_order_acquire);
  return until > 0.0 && Now() < until;
}

std::unique_ptr<rex::input::InputDriver> MakeAutoSkipDriver() {
  return std::make_unique<AutoSkipDriver>();
}

}  // namespace ng2
