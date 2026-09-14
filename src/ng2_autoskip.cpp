#include "ng2_autoskip.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
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

// --- Live pad file (pad_script.txt) ------------------------------------------
//
// A live control channel that mirrors Fable II's, driven by AI Vision's
// hand_padscript tool. The tool writes commands to <exe dir>/pad_script.txt;
// this driver polls that file (every 100 ms, stat only), reads all lines,
// deletes it, and plays the queued commands one at a time - each held for its
// time, then a 0.1 s gap - so a RUNNING game can be driven with no relaunch. An
// <exe dir>/pad_script.accepts file carrying this pid tells the tool the
// channel is live (the file outlives a hard-exit, so the pid is checked).
// Grammar, one per line, lower-cased, # comments:
//   a b x y start back up down left right lb rb [:hold_s]   (0.2 s default)
//   l:x,y[:secs]  r:x,y[:secs]   sticks in -1..1            (0.5 s default)
//   lt[:secs]  rt[:secs]         triggers
//   wait:secs                    idle
//   release                      clear the queue
struct PadCmd {
  uint16_t buttons = 0;
  float lx = 0, ly = 0, rx = 0, ry = 0;
  float lt = 0, rt = 0;
  double hold = 0.2;
  bool wait_only = false;
};

std::mutex g_padfile_mu;
std::deque<PadCmd> g_padfile_q;
double g_padfile_cmd_start = -1.0;
bool g_padfile_cmd_logged = false;
double g_padfile_last_poll = -1.0;
bool g_padfile_accepts_written = false;

std::filesystem::path PadDir() { return rex::filesystem::GetExecutableFolder(); }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Parse one line. Returns true and fills `out` for a real command; sets `clear`
// for "release"; returns false for blank/comment/unknown.
bool ParsePadLine(const std::string& raw, PadCmd& out, bool& clear) {
  clear = false;
  std::string s = raw;
  const auto hash = s.find('#');
  if (hash != std::string::npos) s = s.substr(0, hash);
  s = Lower(Trim(s));
  if (s.empty()) return false;
  if (s == "release") {
    clear = true;
    return false;
  }
  std::vector<std::string> parts;
  {
    std::stringstream ss(s);
    std::string p;
    while (std::getline(ss, p, ':')) parts.push_back(p);
  }
  if (parts.empty()) return false;
  const std::string& tok = parts[0];
  if (tok == "wait") {
    out.wait_only = true;
    out.hold = parts.size() > 1 ? std::atof(parts[1].c_str()) : 0.5;
    return true;
  }
  if ((tok == "l" || tok == "r") && parts.size() >= 2) {
    float x = 0, y = 0;
    std::sscanf(parts[1].c_str(), "%f,%f", &x, &y);
    if (tok == "l") {
      out.lx = x;
      out.ly = y;
    } else {
      out.rx = x;
      out.ry = y;
    }
    out.hold = parts.size() > 2 ? std::atof(parts[2].c_str()) : 0.5;
    return true;
  }
  if (tok == "lt" || tok == "rt") {
    (tok == "lt" ? out.lt : out.rt) = 1.0f;
    out.hold = parts.size() > 1 ? std::atof(parts[1].c_str()) : 0.5;
    return true;
  }
  if (const uint16_t m = ButtonMask(tok)) {
    out.buttons = m;
    out.hold = parts.size() > 1 ? std::atof(parts[1].c_str()) : 0.2;
    return true;
  }
  return false;
}

void PollPadFile() {
  const double now = Now();
  if (now - g_padfile_last_poll < 0.1)
    return;
  g_padfile_last_poll = now;
  std::error_code ec;
  const auto path = PadDir() / "pad_script.txt";
  if (!std::filesystem::exists(path, ec))
    return;
  std::ifstream f(path);
  if (!f)
    return;
  std::vector<std::string> lines;
  std::string ln;
  while (std::getline(f, ln)) lines.push_back(ln);
  f.close();
  std::filesystem::remove(path, ec);  // consumed
  std::lock_guard<std::mutex> lk(g_padfile_mu);
  int n = 0;
  for (const auto& raw : lines) {
    PadCmd c;
    bool clear = false;
    if (ParsePadLine(raw, c, clear)) {
      g_padfile_q.push_back(c);
      ++n;
    } else if (clear) {
      g_padfile_q.clear();
      g_padfile_cmd_start = -1.0;
    }
  }
  if (n)
    REXLOG_INFO("[padfile] {} command(s) queued", n);
}

// The current pad-file contribution, advancing the queue as time passes.
void PadFileState(uint16_t& buttons, float& lx, float& ly, float& rx, float& ry,
                  float& lt, float& rt) {
  std::lock_guard<std::mutex> lk(g_padfile_mu);
  if (g_padfile_q.empty()) {
    g_padfile_cmd_start = -1.0;
    return;
  }
  const double now = Now();
  if (g_padfile_cmd_start < 0.0) {
    g_padfile_cmd_start = now;
    g_padfile_cmd_logged = false;
  }
  const PadCmd& c = g_padfile_q.front();
  const double elapsed = now - g_padfile_cmd_start;
  if (elapsed < c.hold) {
    if (!g_padfile_cmd_logged) {
      g_padfile_cmd_logged = true;
      REXLOG_INFO("[padfile] buttons 0x{:04X} l({:.1f},{:.1f}) for {:.2f} s",
                  c.buttons, c.lx, c.ly, c.hold);
    }
    if (!c.wait_only) {
      buttons |= c.buttons;
      lx += c.lx;
      ly += c.ly;
      rx += c.rx;
      ry += c.ry;
      lt = std::max(lt, c.lt);
      rt = std::max(rt, c.rt);
    }
  } else if (elapsed < c.hold + 0.1) {
    // the 0.1 s gap between commands - report nothing
  } else {
    g_padfile_q.pop_front();
    g_padfile_cmd_start = -1.0;
  }
}

void ClearPadFileQueue() {
  std::lock_guard<std::mutex> lk(g_padfile_mu);
  g_padfile_q.clear();
  g_padfile_cmd_start = -1.0;
}

void WritePadAcceptsOnce() {
  if (g_padfile_accepts_written)
    return;
  g_padfile_accepts_written = true;
  std::error_code ec;
  const auto path = PadDir() / "pad_script.accepts";
  std::ofstream f(path, std::ios::trunc);
  if (f) {
    f << "ng2recomp pad script v1 pid=" << GetCurrentProcessId() << "\n";
    f.close();
    REXLOG_INFO("Pad file: live channel ready ({})", path.string());
  }
}

// A single device, so its handle is a constant. Distinct from the SDK's own
// driver ids.
constexpr rex::input::DeviceId kSkipDevice =
    static_cast<rex::input::DeviceId>(0x4E473253);  // 'NG2S'

class AutoSkipDriver final : public rex::input::InputDriver {
 public:
  AutoSkipDriver() : InputDriver(nullptr, 0) {}
  ~AutoSkipDriver() override = default;

  X_STATUS Setup() override {
    WritePadAcceptsOnce();
    return X_STATUS_SUCCESS;
  }

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
    // Announce the live channel here rather than in Setup(): AddDriver does not
    // call Setup() in this SDK, but the guest always polls this device.
    WritePadAcceptsOnce();
    // Checked here, at the guest's own polling rate, so a real press stops the
    // synthetic ones within a frame rather than within a sampling interval.
    const bool real = RealPadActive();
    if (real) {
      NoteRealInput();
      ClearPadFileQueue();  // a real press abandons any queued live commands
    } else {
      PollPadFile();  // live channel (pad_script.txt), throttled to 100 ms
    }
    // A launch-time pad script (NG2_PAD_SCRIPT) plays independently of the
    // chapter arm, so a scripted run can navigate menus with no human.
    uint16_t buttons = ScriptButtons(Now());
    if (AutoSkipActive()) {
      const double phase = std::fmod(Now(), kPressPeriod);
      if (phase < kPressHold) buttons |= kButtonA | kButtonStart;
    }
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    if (!real)
      PadFileState(buttons, lx, ly, rx, ry, lt, rt);
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.thumb_lx = int16_t(std::clamp(lx, -1.0f, 1.0f) * 32767.0f);
    out_state->gamepad.thumb_ly = int16_t(std::clamp(ly, -1.0f, 1.0f) * 32767.0f);
    out_state->gamepad.thumb_rx = int16_t(std::clamp(rx, -1.0f, 1.0f) * 32767.0f);
    out_state->gamepad.thumb_ry = int16_t(std::clamp(ry, -1.0f, 1.0f) * 32767.0f);
    out_state->gamepad.left_trigger = uint8_t(std::clamp(lt, 0.0f, 1.0f) * 255.0f);
    out_state->gamepad.right_trigger = uint8_t(std::clamp(rt, 0.0f, 1.0f) * 255.0f);
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
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
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
