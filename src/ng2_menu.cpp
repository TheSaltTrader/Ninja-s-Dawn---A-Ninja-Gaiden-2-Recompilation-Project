#include "ng2_menu.h"

#include "ng2_diagnostics.h"

#include "ng2_hwdetect.h"
#include "ng2_autoskip.h"
#include "ng2_perf.h"
#include "ng2_texnotify.h"

#include <filesystem>
#include <thread>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include "ng2_platform.h"
#include "ng2_saveimport.h"
#include "ng2_update.h"

namespace fs = std::filesystem;

namespace ng2 {
namespace {

// --- colours -------------------------------------------------------------
//
// Steel and blood, which is the game's own palette. The accent is the red of
// the II in the logo.
const ImVec4 kGood(0.42f, 0.80f, 0.47f, 1.0f);
const ImVec4 kBad(0.88f, 0.32f, 0.30f, 1.0f);
const ImVec4 kDim(0.58f, 0.58f, 0.62f, 1.0f);
const ImVec4 kAccent(0.78f, 0.16f, 0.18f, 1.0f);
const ImVec4 kHeading(0.86f, 0.86f, 0.88f, 1.0f);

// Wrapped, not clipped. Several of these lines are two sentences long and the
// overlay is only 620px wide; ImGui::Text would just run them off the edge,
// which is how the footer lost its last three words.
void Muted(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("%s", buf);
  ImGui::PopStyleColor();
}

// Pushes the menu font if there is one. ImGui 1.92 takes an explicit size, and
// a null font means "keep the current one", so the fallback is free.
void PushMenuFont(ImFont* font, float size) {
  ImGui::PushFont(font, font != nullptr ? size : 0.0f);
}

// A label with its explanation behind a hover marker.
//
// The explanations were paragraphs under every row, which pushed the settings
// themselves below the fold - the one thing a settings screen must not do.
// They are worth keeping (a setting like "internal render size" is just a
// number otherwise), so they moved into the marker, and only a genuine
// surprise stays on the page as text.
void HelpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextUnformatted("(?)");
  ImGui::PopStyleColor();
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void RowLabel(const char* label, const char* help) {
  ImGui::TextUnformatted(label);
  if (help && *help)
    HelpMarker(help);
}

void SectionHeader(const char* text, const char* help = nullptr) {
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  if (help && *help)
    HelpMarker(help);
  // A short accent rule under the heading rather than a full-width separator:
  // the pages are one column of settings, and a line all the way across reads
  // as a page break instead of a group label.
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
      ImVec2(p.x, p.y + 2.0f), ImVec2(p.x + 46.0f, p.y + 4.0f),
      ImGui::GetColorU32(kAccent));
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
}

// A path shown in a read-only field: long paths have to be visible and
// selectable, and an ImGui::Text would just clip them.
void PathField(const char* id, const std::string& value) {
  std::string buffer = value;
  buffer.resize(std::max<size_t>(buffer.size() + 1, 512));
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputText(id, buffer.data(), buffer.size(),
                   ImGuiInputTextFlags_ReadOnly);
}

// --- cvar helpers --------------------------------------------------------

bool CvarExists(const char* name) {
  return rex::cvar::GetFlagInfo(name) != nullptr;
}

void SetCvar(const char* name, const std::string& value) {
  if (!CvarExists(name)) {
    // Not a silent no-op: a setting whose cvar this build does not have is a
    // real finding, and the menu greys the row for the same reason.
    REXLOG_WARN("Settings: cvar '{}' is not registered; '{}' not applied", name,
                value);
    return;
  }
  if (!rex::cvar::SetFlagByName(name, value))
    REXLOG_WARN("Settings: cvar '{}' rejected value '{}'", name, value);
}

// The values the build itself declares for a string cvar, so a combo can never
// offer an option the presenter does not implement.
//
// `declared` separates the two ways a one-entry list happens: a build that
// really implements one filter, and a cvar that constrains nothing and left us
// falling back to the current value. They deserve different words on screen.
std::vector<std::string> AllowedValues(const char* name,
                                       const std::string& current,
                                       bool* declared) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  if (info != nullptr && !info->constraints.allowed_values.empty()) {
    if (declared) *declared = true;
    return info->constraints.allowed_values;
  }
  if (declared) *declared = false;
  return {current};
}

// --- shared pages --------------------------------------------------------

struct PageOptions {
  // False in the in-game overlay: the window and the guest video mode were
  // created during startup and cannot be rebuilt underneath a running game.
  bool restart_bound_editable = true;
};

// Marks a row that will not take effect until the next launch. Drawn after the
// control so it reads as a footnote on the value, not on the label. Red, and
// worded plainly: the value is saved the moment it changes and applied on the
// next launch, so the player is told the change is real but deferred rather
// than being blocked from making it.
void RestartTag() {
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.28f, 0.28f, 1.0f));
  ImGui::TextUnformatted("restart required");
  ImGui::PopStyleColor();
}

// Settings are drawn as a two-column grid, label beside control. Stacking the
// control under its label doubled the height of every row and pushed half the
// list off a 720p window.
constexpr float kLabelWidth = 190.0f;
constexpr float kControlWidth = 250.0f;
constexpr ImGuiTableFlags kRowTableFlags = ImGuiTableFlags_SizingFixedFit;

// Settings rows are tighter than the rest of the UI. At the global spacing the
// list ran two rows past the bottom of a 720p window, and the choice was
// between dropping settings and reclaiming ~10px a row - which is invisible in
// a table of aligned controls, and costs nothing.
struct TightRows {
  TightRows() {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(st.ItemSpacing.x, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(st.FramePadding.x, 4.0f));
  }
  ~TightRows() { ImGui::PopStyleVar(2); }
};

// Opens the next label/control row. The control that follows fills the second
// column, so every widget lines up without any of them naming a width.
void RowStart(const char* label, const char* help) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && *help)
    HelpMarker(help);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

struct Resolution {
  int w, h;
};
// The guest is told the display is this size, so these are real render
// resolutions rather than upscales.
//
// The ultrawide entries are offered because the window really can be that
// shape, but Ninja Gaiden II is a 2008 console title with a 16:9 HUD and no
// wider field of view to give: at 21:9 or 32:9 the picture either stretches or
// gets pillarboxed, and which one is the "Keep aspect ratio" setting below.
// Pillarboxed is the honest choice - it renders the game's own framing at the
// full height of the display - and it is why that setting defaults to on.
constexpr Resolution kResolutions[] = {
    {1280, 720},
    {1920, 1080},
    {2560, 1440},
    {3840, 2160},
    {2560, 1080},
    {3440, 1440},
    {3840, 1600},
    {5120, 1440},
};
constexpr int kResolutionCount = IM_ARRAYSIZE(kResolutions);
// The trailing "Custom" entry is only ever *shown*; it cannot be picked. It
// exists so a hand-edited config with an unusual size is reported honestly
// instead of being silently snapped to 720p the first time the menu opens.
const char* const kResolutionNames[] = {
    "Original (1280 x 720)",
    "Full HD (1920 x 1080)",
    "1440p (2560 x 1440)",
    "4K (3840 x 2160)",
    "Ultrawide 21:9 (2560 x 1080)",
    "Ultrawide 21:9 (3440 x 1440)",
    "Ultrawide 24:10 (3840 x 1600)",
    "Super ultrawide 32:9 (5120 x 1440)",
    "Custom",
};

int ResolutionIndex(const Ng2Settings& s) {
  for (int i = 0; i < kResolutionCount; ++i) {
    if (kResolutions[i].w == s.window_width && kResolutions[i].h == s.window_height)
      return i;
  }
  return kResolutionCount;  // Custom
}

// 30 and 60 are what the console offered. 120 and 144 are here because they
// were asked for; the row says plainly what they do to a game that paces
// itself off the display.
constexpr int kFpsValues[] = {30, 60, 120, 144};
const char* const kFpsNames[] = {"30 Hz", "60 Hz (as shipped)", "120 Hz", "144 Hz"};

int FpsIndex(int fps) {
  for (int i = 0; i < IM_ARRAYSIZE(kFpsValues); ++i)
    if (kFpsValues[i] == fps) return i;
  return 1;  // 60
}

// Quality presets: one row that moves the three that matter together.
//
// Taken from re:Blue, including the part that makes it work - "Custom" is a
// state the row can *display* but not select. Anything else and every preset
// silently lies about what the other rows say.
struct QualityPreset {
  const char* name;
  int scale;
  const char* aa;
  int aniso;
};
constexpr QualityPreset kPresets[] = {
    {"Performance", 1, "none", -1},
    {"Balanced", 1, "fxaa", 2},
    {"Quality", 2, "fxaa", 4},
    {"Maximum", 3, "fxaa_extreme", 4},
};
constexpr int kPresetCount = IM_ARRAYSIZE(kPresets);
const char* const kPresetNames[] = {"Performance", "Balanced", "Quality",
                                    "Maximum", "Custom"};

int PresetIndex(const Ng2Settings& s) {
  for (int i = 0; i < kPresetCount; ++i) {
    if (kPresets[i].scale == s.resolution_scale &&
        kPresets[i].aniso == s.anisotropic && s.antialias == kPresets[i].aa)
      return i;
  }
  return kPresetCount;  // Custom
}

void ApplyPreset(Ng2Settings& s, int index) {
  if (index < 0 || index >= kPresetCount)
    return;
  s.resolution_scale = kPresets[index].scale;
  s.antialias = kPresets[index].aa;
  s.anisotropic = kPresets[index].aniso;
}

// The cvar's own values are terse ("fxaa_extreme"); these are what a player
// should read. Anything the build declares that is not in this list is shown
// under its raw name rather than dropped.
const char* AaLabel(const std::string& value) {
  if (value == "none") return "Off";
  if (value == "fxaa") return "FXAA";
  if (value == "fxaa_extreme") return "FXAA (extreme)";
  return value.c_str();
}

// Workarounds for faults, kept apart from the quality settings. They are not
// preferences: each one trades correct behaviour for getting past something
// broken, and both say so.
// The Chapter 12 crash workaround has no row of its own.
//
// It is a community patch (Gliniak, via Xenia's patch file for this title)
// whose author warns it breaks every chapter but the one it fixes, so it can
// not simply be left on - and it can not be turned on automatically either,
// because THE GAME GIVES NO SIGNAL THAT A PARTICULAR CHAPTER IS LOADING.
// That was checked rather than assumed: all fourteen s_chap_NN.ng2 files are
// opened together in one burst, the same burst every run, and the only other
// thing a chapter load touches is aurora12.wmv, which is the same file for
// chapter 1. Nothing distinguishes chapter 12 from any other.
//
// So it stays a cvar - `ng2_chapter12_workaround`, on the F4 screen and in
// ng2_settings.cfg - rather than a question put to someone who has no way of
// knowing the answer until the game stops working.
bool DrawWorkarounds(Ng2Settings& s, const PageOptions& opts) {
  (void)s;
  (void)opts;
  return false;
}

// The whole settings list, in one page.
//
// It used to be four tabs. One page reads better here: there are barely twenty
// settings, tabs hid half of them behind a click, and the order Display ->
// Enhancements is the order somebody actually sets them in.
//
// Every row is a real setting on a cvar this build registers. That note used to
// end by listing two things the runtime supposedly did not have - an output
// filter and texture replacement - and both have since turned out to be wrong.
// present_effect declares five values on a source-built runtime (the spatial
// CAS/FSR shaders are committed and merely gated off in the shipped build), and
// texture replacement now exists. Rows here are driven by what the RUNNING
// build declares rather than by a belief about it.
bool DrawSettings(Ng2Settings& s, const PageOptions& opts) {
  bool changed = false;
  const bool live = opts.restart_bound_editable;

  // --- Display -----------------------------------------------------------
  SectionHeader("Display");
  TightRows tight_display;
  if (ImGui::BeginTable("display", 2, kRowTableFlags)) {
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, kLabelWidth);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, kControlWidth);

    RowStart("Fullscreen", "Borderless fullscreen on the current monitor.");
    changed |= ImGui::Checkbox("##fullscreen", &s.fullscreen);

    ImGui::BeginDisabled(false);  // restart-bound, but always editable: the change is saved now and applied on the next launch (RestartTag says so), and ApplyLiveSettings never pushes these live
    // Only worth asking about when there is a choice to make.
    {
      const auto monitors = Monitors();
      if (monitors.size() > 1) {
        RowStart("Monitor", "Which display to open on. The window is sized to "
                            "fit whichever one is chosen.");
        std::vector<std::string> labels;
        std::vector<const char*> items;
        labels.reserve(monitors.size());
        for (const auto& m : monitors) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%d:  %d x %d%s", m.index + 1,
                        m.full_width, m.full_height,
                        m.primary ? "   (main)" : "");
          labels.emplace_back(buf);
        }
        for (const auto& label : labels)
          items.push_back(label.c_str());
        int pick = s.monitor;
        if (ImGui::Combo("##monitor", &pick, items.data(), int(items.size()))) {
          s.monitor = pick;
          // Picking a screen means "play on that one", so the resolution
          // follows it rather than leaving a size chosen for a different
          // display behind - which is how a 4K window ended up on a 1600p
          // screen in the first place. It is still free to be changed below.
          if (pick >= 0 && size_t(pick) < monitors.size()) {
            s.window_width = monitors[size_t(pick)].full_width;
            s.window_height = monitors[size_t(pick)].full_height;
          }
          changed = true;
        }
        if (!live) RestartTag();
      }
    }

    RowStart("Resolution",
             "The size of the window, or of the surface the picture is scaled "
             "to in fullscreen. The game itself always renders 16:9, at the "
             "internal render size and scale below, and is told a 16:9 display "
             "whatever this is set to - so on an ultrawide the picture is "
             "pillarboxed with Keep aspect ratio on, and stretched with it off.");
    int res_index = ResolutionIndex(s);
    if (ImGui::Combo("##resolution", &res_index, kResolutionNames,
                     IM_ARRAYSIZE(kResolutionNames))) {
      if (res_index < kResolutionCount) {
        s.window_width = kResolutions[res_index].w;
        s.window_height = kResolutions[res_index].h;
        changed = true;
      }
    }
    if (!live) RestartTag();

    // Say it here rather than letting the window silently come up smaller: a
    // size the screen cannot show is the difference between "4K" meaning
    // supersampling and meaning "the Play button is under the taskbar".
    // Only when the size is bigger than the SCREEN, not bigger than its work
    // area: picking a monitor sets its native resolution, and a window is
    // always a taskbar shorter than that. Warning about the taskbar every time
    // would be noise on the one path that is working correctly.
    {
      const auto all = Monitors();
      const size_t at =
          (s.monitor >= 0 && size_t(s.monitor) < all.size()) ? size_t(s.monitor)
                                                             : 0;
      if (!all.empty() && (s.window_width > all[at].full_width ||
                           s.window_height > all[at].full_height)) {
        RowStart("", "");
        ImGui::TextColored(kBad,
                           "Bigger than monitor %d (%d x %d) - it will open at "
                           "%d x %d.", int(at) + 1, all[at].full_width,
                           all[at].full_height,
                           std::min(s.window_width, all[at].width),
                           std::min(s.window_height, all[at].height));
      }
    }

    RowStart("Frame rate",
             "Ninja Gaiden II paces its own logic off the refresh rate it is "
             "told the display has. 60 is what the console ran.");
    int fps_index = FpsIndex(s.fps);
    if (ImGui::Combo("##fps", &fps_index, kFpsNames, IM_ARRAYSIZE(kFpsNames))) {
      s.fps = kFpsValues[fps_index];
      changed = true;
    }
    if (!live) RestartTag();
    // Not a preference but a behaviour change, so it stays on the page rather
    // than hiding behind a marker.
    if (s.fps > 60)
      Muted("Above 60 the game runs faster, not smoother.");
    ImGui::EndDisabled();

    RowStart("V-Sync",
             "Off does not just tear here. The game paces its logic off the "
             "display, so without V-Sync it runs faster than it should.");
    changed |= ImGui::Checkbox("##vsync", &s.vsync);
    // Same hazard as fps > 60 one row up, so it gets the same treatment: the
    // consequence is stated on the page, not only in a tooltip nobody hovers.
    // Off raises the guest's vblank from 60 Hz to 1000 Hz, and this title
    // advances its logic on vblank.
    if (!s.vsync)
      Muted("Off makes the game run faster than it should, not just tear.");

    RowStart("Keep aspect ratio",
             "Pillarbox the 16:9 picture on a wider window instead of "
             "stretching it to fill.");
    changed |= ImGui::Checkbox("##letterbox", &s.letterbox);

    RowStart("Hide the pointer after",
             "Seconds of mouse stillness over the window before the pointer "
             "disappears. 0 keeps it visible.");
    changed |= ImGui::SliderInt("##cursorhide", &s.cursor_hide_seconds, 0, 30,
                                s.cursor_hide_seconds == 0 ? "never" : "%d s");

    RowStart("Keyboard and mouse",
             "Drives the guest controller from the keyboard. The runtime has "
             "the driver but leaves it off, so without this only a real pad "
             "works. Bindings are on the F4 screen under Input / Keybinds - "
             "Enter is Start and Semicolon or Space is A.");
    changed |= ImGui::Checkbox("##mnk", &s.keyboard_control);

    ImGui::EndTable();
  }

  // --- Enhancements ------------------------------------------------------
  SectionHeader("Enhancements");
  TightRows tight_enhance;
  if (ImGui::BeginTable("enhance", 2, kRowTableFlags)) {
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, kLabelWidth);
    ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, kControlWidth);

    RowStart("Quality preset",
             "Sets supersampling, antialiasing and texture filtering together. "
             "Change any of them below and this reads Custom.");
    int preset = PresetIndex(s);
    if (ImGui::Combo("##preset", &preset, kPresetNames,
                     IM_ARRAYSIZE(kPresetNames))) {
      if (preset < kPresetCount) {
        ApplyPreset(s, preset);
        changed = true;
      }
    }

    ImGui::BeginDisabled(false);  // restart-bound, but always editable: the change is saved now and applied on the next launch (RestartTag says so), and ApplyLiveSettings never pushes these live
    RowStart("Internal render size",
             "A patch on the game itself: it renders at 1120x584 internally "
             "and scales that up. 1280x720 removes the upscale. The same "
             "change as the Xenia community patch for this title.");
    int size_index = s.internal_720p ? 1 : 0;
    const char* sizes[] = {"1120 x 584 (as shipped)", "1280 x 720 (patched)"};
    if (ImGui::Combo("##internal", &size_index, sizes, 2)) {
      s.internal_720p = size_index == 1;
      changed = true;
    }
    if (!live) RestartTag();

    RowStart("Supersampling",
             "Renders the game's own framebuffer at a multiple of its size and "
             "filters it back down. The sharpest setting here, and the most "
             "expensive: the cost goes with the SQUARE of the number, so 4x is "
             "sixteen times the pixels. Measured on this machine at a 1080p "
             "window, every setting here held 60 fps - but that was measured on "
             "the menu, which is a light scene, so treat the high ones as worth "
             "trying rather than free.");
    int scale_index = std::clamp(s.resolution_scale - 1, 0, 7);
    // All the way to the plugin's own ceiling of 8. Stopping short of it was
    // arbitrary twice over - first at 3, then at 6 - and on a card that can
    // afford more it is image quality left on the table.
    //
    // This is also what raises SHADOW resolution, which is measured rather than
    // assumed: the probe showed this title resolving a 512x512 depth buffer
    // through a scaled resolve, so at 3x it is already allocated at 1536x1536.
    const char* scales[] = {"Off", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
    if (ImGui::Combo("##scale", &scale_index, scales, IM_ARRAYSIZE(scales))) {
      s.resolution_scale = scale_index + 1;
      changed = true;
    }
    if (!live) RestartTag();
    ImGui::EndDisabled();

    // Not restart-bound: the plugin applies this post-process per swap, so it
    // changes with the next frame even in the overlay.
    RowStart("Antialiasing",
             "Post-process FXAA over the finished frame. Cheap, and it softens "
             "the image a little; supersampling is the higher-quality route if "
             "the GPU can afford it.");
    const auto aa_values = AllowedValues("swap_post_effect", s.antialias, nullptr);
    int aa_index = 0;
    for (size_t i = 0; i < aa_values.size(); ++i)
      if (aa_values[i] == s.antialias) aa_index = static_cast<int>(i);
    std::vector<const char*> aa_names;
    for (const auto& v : aa_values) aa_names.push_back(AaLabel(v));
    if (ImGui::Combo("##aa", &aa_index, aa_names.data(),
                     static_cast<int>(aa_names.size()))) {
      s.antialias = aa_values[aa_index];
      changed = true;
    }

    // Sharpening. Restart-bound: the presenter builds its pipeline once.
    ImGui::BeginDisabled(false);  // was disabled on the setup screen; now always editable, saved and applied next launch
    ImGui::BeginDisabled(false);  // was disabled on the setup screen; now always editable, saved and applied next launch
    RowStart("Accurate depth",
             "Emulates the console's 24-bit float depth buffer exactly instead "
             "of approximating it. Costs pixel-shader work and buys back depth "
             "precision, which is where shadow acne and z-fighting come from. "
             "Worth trying if shadows shimmer or surfaces flicker where they "
             "meet.");
    changed |= ImGui::Checkbox("##accdepth", &s.accurate_depth);
    if (!live) RestartTag();
    ImGui::EndDisabled();

    RowStart("Fuzzy alpha test",
             "Compares alpha-test values approximately rather than exactly. The "
             "plugin offers this specifically to stop flickering on NVIDIA "
             "cards, so it is worth a try if foliage, chains or grates shimmer. "
             "It does change what the alpha test accepts, so it is off unless "
             "asked for.");
    changed |= ImGui::Checkbox("##fuzzyalpha", &s.fuzzy_alpha);

    RowStart("Skip chapter cinematics",
             "Dismisses the in-engine cinematic each chapter opens with, so a "
             "replay does not need the button pressed every time.\n\n"
             "It arms when the chapter's own data loads and disarms the instant "
             "you touch the controller - so a cinematic you want to watch is "
             "one nudge away from being left alone, and it can never still be "
             "pressing A once you have control.");
    if (ImGui::Checkbox("##skipcine", &s.skip_cinematics)) {
      SetAutoSkipEnabled(s.skip_cinematics);
      changed = true;
    }

    RowStart("On-screen readouts",
             "Shown top right during play, and toggled with F8 without opening "
             "this screen.\n\n"
             "Three separate switches because they answer different questions: "
             "frame rate is \"is it smooth\", GPU is \"is the card the limit\", "
             "and video memory is \"can this machine hold the texture pack\".");
    changed |= ImGui::Checkbox("Show (F8)##hudon", &s.hud_enabled);
    // Which ones stay editable while off, so a choice can be made before
    // showing them rather than by trial and error on screen.
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    changed |= ImGui::Checkbox("FPS##hud", &s.hud_fps);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("GPU##hud", &s.hud_gpu);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("VRAM##hud", &s.hud_vram);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("CPU##hud", &s.hud_cpu);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("RAM##hud", &s.hud_ram);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Bars##hud", &s.hud_bars);
    HelpMarker("Draws a small bar under each readout showing its current level "
               "at a glance, in the Fable II style.");
    if (s.hud_enabled && !s.hud_fps && !s.hud_gpu && !s.hud_vram && !s.hud_cpu &&
        !s.hud_ram)
      Muted("Nothing is selected, so nothing will show.");
    // Separate switch, because it is a separate display: these bars are in the
    // Textures section of this menu, not on screen during play.
    changed |= ImGui::Checkbox("Live cost bars in this menu##hudbars",
                               &s.hud_menu_bars);

    RowStart("Sharpening",
             "FidelityFX CAS sharpens the finished frame, which suits a game "
             "that renders at 1120x584 and scales up. FSR upscales as well as "
             "sharpening.\n\n"
             "Only the filters this build actually has are listed.");
    const auto pe_values = AllowedValues("present_effect", s.present_effect, nullptr);
    int pe_index = 0;
    for (size_t i = 0; i < pe_values.size(); ++i)
      if (pe_values[i] == s.present_effect) pe_index = static_cast<int>(i);
    std::vector<const char*> pe_names;
    for (const auto& v : pe_values) pe_names.push_back(v.c_str());
    if (ImGui::Combo("##presenteffect", &pe_index, pe_names.data(),
                     static_cast<int>(pe_names.size()))) {
      s.present_effect = pe_values[pe_index];
      changed = true;
    }
    ImGui::EndDisabled();
    if (pe_values.size() <= 1) {
      Muted("This runtime offers only bilinear - the CAS and FSR shaders are "
            "compiled out of the plugin it shipped with.");
    } else if (!live) {
      RestartTag();
    }

    if (s.present_effect != "bilinear") {
      // Not restart-bound: the presenter reads it per frame.
      RowStart("Extra sharpness",
               "Added on top of what the filter does by itself. 0 leaves the "
               "filter's own amount.");
      ImGui::SetNextItemWidth(240.0f);
      changed |= ImGui::SliderFloat("##cassharp", &s.cas_sharpness, 0.0f, 1.0f, "%.2f");
    }

    ImGui::BeginDisabled(false);  // restart-bound, but always editable: the change is saved now and applied on the next launch (RestartTag says so), and ApplyLiveSettings never pushes these live
    RowStart("Texture cache",
             "How much host memory the GPU may keep textures in. The plugin's "
             "own limits are console-era numbers on a modern card; raising them "
             "means fewer evictions and re-uploads, which shows up as fewer "
             "hitches while an area streams in - not as a sharper picture.");
    // 8 GB is the plugin's own ceiling (its hard limit allows 8192 MB, the
    // soft limit 4096). It is here because an upscaled pack needs it: 4x RGBA
    // replacements cost roughly 128x a DXT1 original, so a 1290-texture pack is
    // about 6 GB - well past the 4 GB that used to be the largest choice, and a
    // cache smaller than the working set evicts and re-uploads constantly.
    // Choose both memory settings from the hardware. Runs once on first launch;
    // this button is how it is re-run - which matters more than it sounds,
    // because the reading is a SNAPSHOT of free video memory. Detecting while
    // something large is on the GPU gives a low answer that would otherwise
    // stay low forever.
    // Function-local: DrawSettings is shared by the setup screen and the F10
    // overlay, and only one of them is on screen at a time, so this is display
    // state for the button rather than anything either surface owns.
    static DetectedSettings last_detect;
    if (ImGui::Button("Detect from hardware")) {
      const GpuInfo gpu = DetectGpu();
      last_detect = RecommendSettings(gpu);
      if (gpu.valid) {
        s.texture_cache_mb = last_detect.texture_cache_mb;
        s.texture_scale = last_detect.texture_scale;
        s.hardware_detected = true;
        changed = true;
      }
    }
    if (!last_detect.summary.empty()) {
      Muted("%s", last_detect.summary.c_str());
      if (!last_detect.caveat.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "%s",
                           last_detect.caveat.c_str());
      }
    }
    ImGui::Spacing();

    static const int kCacheMb[] = {0, 1024, 2048, 4096, 8192};
    static const char* const kCacheNames[] = {"Default (384 MB)", "1 GB",
                                              "2 GB", "4 GB",
                                              "8 GB (for a texture pack)"};
    int cache_index = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kCacheMb); ++i)
      if (kCacheMb[i] == s.texture_cache_mb) cache_index = i;
    if (ImGui::Combo("##texcache", &cache_index, kCacheNames,
                     IM_ARRAYSIZE(kCacheNames))) {
      s.texture_cache_mb = kCacheMb[cache_index];
      changed = true;
    }
    if (!live) RestartTag();

    RowStart("Anisotropic filtering",
             "Forces a filtering level on every texture the game samples. "
             "\"Game default\" leaves the title's own sampler settings alone.");
    const char* aniso[] = {"Game default", "1x", "2x", "4x", "8x", "16x"};
    int aniso_index = std::clamp(s.anisotropic + 1, 0, 5);
    if (ImGui::Combo("##aniso", &aniso_index, aniso, 6)) {
      s.anisotropic = aniso_index - 1;
      changed = true;
    }
    if (!live) RestartTag();
    ImGui::EndDisabled();

    RowStart("Skip intro videos",
             "Goes straight past the opening sequence and the attract demos "
             "on start. Chapter-loading videos are left alone - the game "
             "treats one of those failing to open as a bad disc and stops.");
    bool skip = s.video_mode == 2;
    if (ImGui::Checkbox("##skipvideo", &skip)) {
      // Unticking has to return to 0 (the game plays them), not 1. 1 was the
      // old overlay mode, and leaving the box unticked in mode 1 meant the
      // setting could never get back to its own default.
      s.video_mode = skip ? 2 : 0;
      changed = true;
    }

    RowStart("Dither the output", "Hides colour banding on 8-bit displays.");
    changed |= ImGui::Checkbox("##dither", &s.present_dither);

    ImGui::EndTable();
  }

  return changed;
}


}  // namespace

// ---------------------------------------------------------------------------

MenuFonts& Fonts() {
  static MenuFonts fonts;
  return fonts;
}

void LoadMenuFonts(ImFontAtlas* atlas) {
  if (atlas == nullptr)
    return;
  // Segoe UI ships with every supported Windows version, so this is not a
  // bundled asset that can go missing from a release - and if it somehow is
  // missing, both pointers stay null and the SDK's default font is used.
  const char* kBody = "C:\\Windows\\Fonts\\segoeui.ttf";
  const char* kTitle = "C:\\Windows\\Fonts\\seguibl.ttf";  // Segoe UI Black
  std::error_code ec;
  if (std::filesystem::exists(kBody, ec)) {
    Fonts().body = atlas->AddFontFromFileTTF(kBody, Fonts().body_size);
  }
  const char* title_path =
      std::filesystem::exists(kTitle, ec) ? kTitle
      : std::filesystem::exists(kBody, ec) ? kBody
                                           : nullptr;
  if (title_path != nullptr) {
    Fonts().title = atlas->AddFontFromFileTTF(title_path, Fonts().title_size);
  }
  REXLOG_INFO("Menu fonts: body={} title={}", Fonts().body != nullptr,
              Fonts().title != nullptr);
}

void ApplyMenuStyle(ImGuiStyle& s) {
  // Start from a known base rather than patching whatever the SDK left, so the
  // result does not depend on the SDK's own theme changing under us.
  ImGui::StyleColorsDark(&s);

  s.WindowRounding = 6.0f;
  s.ChildRounding = 4.0f;
  s.FrameRounding = 4.0f;
  s.GrabRounding = 3.0f;
  s.TabRounding = 4.0f;
  s.PopupRounding = 4.0f;
  s.ScrollbarRounding = 6.0f;
  s.WindowBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.WindowPadding = ImVec2(22.0f, 18.0f);
  s.FramePadding = ImVec2(10.0f, 6.0f);
  s.ItemSpacing = ImVec2(10.0f, 7.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.ScrollbarSize = 13.0f;
  s.GrabMinSize = 12.0f;
  s.SeparatorTextBorderSize = 2.0f;

  ImVec4* c = s.Colors;
  const ImVec4 accent = kAccent;
  const ImVec4 accent_hot(0.90f, 0.22f, 0.24f, 1.00f);

  c[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.93f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.075f, 0.085f, 0.97f);
  c[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.115f, 0.99f);
  c[ImGuiCol_Border] = ImVec4(0.24f, 0.24f, 0.27f, 0.85f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.27f, 0.30f, 1.00f);
  c[ImGuiCol_TitleBg] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.10f, 0.11f, 1.00f);
  c[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.10f, 0.60f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.36f, 0.39f, 1.00f);
  c[ImGuiCol_ScrollbarGrabActive] = accent;
  c[ImGuiCol_CheckMark] = accent_hot;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accent_hot;
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.23f, 1.00f);
  c[ImGuiCol_ButtonHovered] = accent;
  c[ImGuiCol_ButtonActive] = accent_hot;
  c[ImGuiCol_Header] = ImVec4(0.24f, 0.16f, 0.17f, 1.00f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.18f, 0.19f, 1.00f);
  c[ImGuiCol_HeaderActive] = accent;
  c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
  c[ImGuiCol_Tab] = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
  c[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.18f, 0.19f, 1.00f);
  c[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.13f, 0.14f, 1.00f);
  c[ImGuiCol_TabSelectedOverline] = accent_hot;
  c[ImGuiCol_TabDimmed] = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
  c[ImGuiCol_TabDimmedSelected] = ImVec4(0.20f, 0.12f, 0.13f, 1.00f);
  c[ImGuiCol_PlotHistogram] = accent;
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.40f, 0.16f, 0.17f, 0.75f);
}

void ApplyLiveSettings(const Ng2Settings& s, rex::ui::Window* window) {
  // FXAA is a post-process the plugin applies per swap, so it changes with the
  // next frame - which is why the antialiasing row is not restart-bound in the
  // overlay even though it belongs to the GPU plugin.
  SetCvar("swap_post_effect", s.antialias);
  SetCvar("present_effect", s.present_effect);
  SetCvar("present_cas_additional_sharpness", std::to_string(s.cas_sharpness));
  SetCvar("present_dither", s.present_dither ? "true" : "false");
  SetCvar("present_letterbox", s.letterbox ? "true" : "false");
  SetCvar("vsync", s.vsync ? "true" : "false");
  SetCvar("mnk_mode", s.keyboard_control ? "true" : "false");
  // Read afresh every time the guest opens a video, so turning this on mid-run
  // takes effect at the next one rather than at the next launch.
  //
  // Passed through as-is. This used to fold 0 into 1, from when the guest's own
  // decoder produced garbled video and replacing the picture was the only way
  // to watch anything. The v0.5.0 codegen fix repaired that decoder, 0 became
  // the right default and the setting says so - but this line still forced 1,
  // so the replacement decoder ran on every launch no matter what was chosen,
  // and mode 0 was unreachable from the only path that runs at startup.
  SetCvar("ng2_video_mode", std::to_string(s.video_mode));

  // The texture pack, live. The plugin drops every texture when this changes,
  // so the scene in front of the player switches over within a frame or two -
  // which is the only way to actually judge a texture pack, since comparing it
  // against a screenshot from a previous run compares two different moments.
  // Dumping is RESTART-REQUIRED: it is delivered only in the launch tuning file
  // (ng2_tuning.h), never pushed live. Toggling it mid-session used to clear the
  // cache and dump "the scene in front of the player", which made players think a
  // stage was being captured when everything already resident - and the whole
  // start of the stage - was never re-loaded and so never written. On from launch,
  // every texture the stage loads passes through the dump path. So nothing here.
  SetCvar("texture_pack_path",
          (s.texture_pack && !s.texture_path.empty())
              ? (s.ResolvedTexturePath() / "pack").generic_string()
              : std::string());
  if (window != nullptr) {
    window->SetFullscreen(s.fullscreen);
    // Auto-hide is a *mode*, not just a delay. The window only ever hides the
    // pointer while its visibility is kAutoHidden; the delay alone does
    // nothing while it stays kVisible - which is why "Hide the pointer after"
    // never hid anything: the delay was set, the mode never was. So drive the
    // mode from the setting. >0 arms auto-hide with that idle delay (the
    // pointer shows on motion and hides once it stops); 0 means never hide,
    // i.e. plain kVisible.
    if (s.cursor_hide_seconds > 0) {
      window->SetCursorAutoHideDelayMs(
          static_cast<uint32_t>(s.cursor_hide_seconds) * 1000u);
      window->SetCursorVisibility(
          rex::ui::Window::CursorVisibility::kAutoHidden);
    } else {
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
  }
}

// --- SetupScreen -----------------------------------------------------------

SetupScreen::SetupScreen(rex::ui::ImGuiDrawer* drawer, Ng2Settings* settings,
                         std::function<void(bool)> on_done,
                         std::function<void()> on_advanced)
    : ImGuiDialog(drawer),
      settings_(settings),
      on_done_(std::move(on_done)),
      on_advanced_(std::move(on_advanced)) {
  install_dest_ = settings_->ResolvedGamePath();
  // The disc image from last time, if it is still there. Only for the field:
  // nothing is installed without pressing the button.
  if (!settings_->iso_path.empty()) {
    std::error_code ec;
    const std::filesystem::path remembered = Ng2Settings::Beside(settings_->iso_path);
    if (std::filesystem::is_regular_file(remembered, ec)) {
      iso_path_ = remembered;
      iso_info_ = InspectDisc(iso_path_);
    }
  }
  // Test seam, matching the NG2_* overrides the app already reads: preselect
  // the disc image and the destination so the installer can be driven by a
  // script. Without it the only way in is a native file dialog, which cannot
  // be automated without typing a path into a modal window.
  if (const char* iso = std::getenv("NG2_ISO"); iso && *iso) {
    iso_path_ = iso;
    iso_info_ = InspectDisc(iso_path_);
  }
  if (const char* dest = std::getenv("NG2_INSTALL_DEST"); dest && *dest)
    install_dest_ = dest;

  RefreshGame();
  RefreshDlc();
}

SetupScreen::~SetupScreen() {
  // The workers write into progress objects that die with this object.
  if (install_thread_.joinable()) {
    progress_.cancel = true;
    install_thread_.join();
  }
}

void SetupScreen::RefreshGame() {
  const auto path = settings_->ResolvedGamePath();
  game_path_inspected_ = path.string();
  game_info_ = InspectFolder(path);
}

void SetupScreen::RefreshDlc() {
  const auto path = settings_->ResolvedDlcPath();
  dlc_path_scanned_ = path.string();
  dlc_count_ = CountStfsPackages(path);
}

void SetupScreen::StartInstall() {
  if (install_thread_.joinable())
    install_thread_.join();
  install_started_ = true;
  install_adopted_ = false;
  install_thread_ = ExtractDiscAsync(iso_path_, install_dest_, progress_);
}


// Chains extraction into video preparation. Called every frame from the
// installer section rather than from the worker, so the two threads are only
// ever started and joined on the UI thread.
void SetupScreen::PollInstall() {
  // Adopting the destination is about the INSTALL, not about the videos.
  //
  // It used to live inside the branch below, which only runs when video
  // preparation is queued - so an install with no preparation to do finished
  // with the game folder still pointing wherever it pointed before. Rescan then
  // re-inspected the OLD path and reported nothing, and the freshly installed
  // files only appeared after choosing the folder by hand.
  //
  // Once per install, not once per frame: this also writes the settings.
  if (install_started_ && progress_.complete.load() &&
      !progress_.failed.load() && !install_adopted_) {
    install_adopted_ = true;
    if (settings_->ResolvedGamePath() != install_dest_) {
      settings_->game_path = install_dest_.string();
      settings_->Save();
    }
    RefreshGame();
  }
}

void SetupScreen::OnDraw(ImGuiIO& io) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("ng2setup", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  PushMenuFont(Fonts().body, Fonts().body_size);

  // One centred column. Full-window rows would run a line of help text across
  // 1600 pixels, which is unreadable and looks like a log file rather than a
  // front end.
  constexpr float kColumnWidth = 1180.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float column = std::min(kColumnWidth, avail);
  const float indent = (avail - column) * 0.5f;
  if (indent > 0.0f)
    ImGui::Indent(indent);
  ImGui::BeginGroup();
  ImGui::PushItemWidth(column);

  PushMenuFont(Fonts().title, Fonts().title_size);
  ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
  ImGui::TextUnformatted("NINJA GAIDEN II");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  Muted("Static recompilation for PC.  Version %s", NG2_VERSION);
  ImGui::Dummy(ImVec2(0.0f, 6.0f));

  // Footer height, reserved so the page body scrolls instead of pushing the
  // buttons off the bottom on a small window.
  const float footer = ImGui::GetFrameHeightWithSpacing() * 2.6f;
  ImGui::BeginChild("body", ImVec2(column, -footer), ImGuiChildFlags_None);

  // Two columns, so the whole thing is visible at once on a 720p window:
  // settings on the left, because they are what somebody comes back for, and
  // content on the right, which is usually set once and never touched again.
  // Stacked in one column it ran to two screens and the settings were the half
  // that fell off the bottom.
  if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawSettings(*settings_, PageOptions{});

    ImGui::TableSetColumnIndex(1);
    DrawContent();
    DrawWorkarounds(*settings_, PageOptions{});
    ImGui::EndTable();
  }

  ImGui::EndChild();

  ImGui::Separator();
  DrawFooter(column);

  ImGui::PopItemWidth();
  ImGui::EndGroup();
  if (indent > 0.0f)
    ImGui::Unindent(indent);
  ImGui::PopFont();
  ImGui::End();
  (void)io;
}

void SetupScreen::DrawContent() {
  SectionHeader("Game data",
                "The folder holding default.xex and the game's data files - "
                "the contents of your own disc.");
  PathField("##gamepath", game_path_inspected_);
  if (ImGui::Button("Choose folder...")) {
    if (auto picked = PickFolder("Select the folder containing default.xex",
                                 settings_->ResolvedGamePath())) {
      settings_->game_path = picked->string();
      RefreshGame();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Rescan"))
    RefreshGame();

  ImGui::TextColored(game_info_.Usable() ? kGood : kBad, "%s",
                     game_info_.message.c_str());
  if (game_info_.Usable() && !game_info_.IsNg2()) {
    ImGui::TextColored(kBad,
                       "This is not Ninja Gaiden II. The recompiled code is "
                       "this game's; another title will not run.");
  }

  DrawInstaller();
  DrawDlc();
  DrawSaves();
}

void SetupScreen::DrawInstaller() {
  SectionHeader("Install from a disc image",
                "Copies a .iso to a folder on disk. The runtime mounts a "
                "folder, not an image, so an ISO has to be extracted once "
                "before it can be played. Re-running skips files already "
                "there at the right size.");
  PathField("##isopath", iso_path_.string());
  if (ImGui::Button("Choose disc image...")) {
    if (auto picked = PickFile("Select an Xbox 360 disc image",
                               {{"Disc images", "*.iso;*.img;*.xex"},
                                {"All files", "*.*"}},
                               iso_path_)) {
      iso_path_ = *picked;
      settings_->iso_path = iso_path_.string();
      iso_info_ = InspectDisc(iso_path_);
    }
  }
  if (!iso_path_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(iso_info_.Usable() ? kGood : kBad, "%s",
                       iso_info_.message.c_str());
  }

  // Where it lands only matters once there is something to install, and it
  // defaults to the game folder. Showing it unconditionally cost three rows on
  // a screen that has to fit, for a question most people never answer.
  PollInstall();
  const bool busy = progress_.running.load();
  if (!iso_path_.empty()) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Install to");
    PathField("##installdest", install_dest_.string());
    if (ImGui::Button("Choose destination...")) {
      if (auto picked = PickFolder("Where should the game data be installed?",
                                   install_dest_)) {
        install_dest_ = *picked;
      }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !iso_info_.Usable() || install_dest_.empty());
    if (ImGui::Button("Install")) {
      StartInstall();
    }
    ImGui::EndDisabled();
  }

  if (busy || install_started_) {
    // An install is one job: copy the disc. It used to re-encode the videos
    // afterwards to work around the broken guest decoder, which is why this
    // bar was once weighted between two phases.
    const char* phase = "Copying the disc";
    const uint64_t total = progress_.bytes_total.load();
    const uint64_t done = progress_.bytes_done.load();
    float fraction =
        total > 0 ? static_cast<float>(double(done) / double(total)) : 0.0f;
    if (progress_.complete.load())
      fraction = 1.0f;

    // A progress bar that can read 106% is a bar nobody can trust, so this is
    // a guarantee rather than an assumption about the arithmetic above it.
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);

    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%s  %.0f%%", phase,
                  fraction * 100.0f);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
    if (progress_.running.load())
      Muted("%s", progress_.CurrentFile().c_str());

    if (busy) {
      if (ImGui::Button("Cancel install"))
        progress_.cancel = true;
    } else if (progress_.complete.load()) {
      // One-shot: adopt the destination as the game folder so Play lights up
      // without the user having to point at what we just wrote.
      if (progress_.failed.load()) {
        ImGui::TextColored(kBad, "%s", progress_.Error().c_str());
      } else {
        ImGui::TextColored(kGood, "Install completed - ready to play.");
      }
    }
  }
}

void SetupScreen::DrawDlc() {
  SectionHeader("Downloadable content",
                "A folder of Xbox 360 content packages (LIVE / PIRS / CON). "
                "They are installed into the game's content store on every "
                "launch; doing it twice is harmless.");
  PathField("##dlcpath", dlc_path_scanned_);
  if (ImGui::Button("Choose DLC folder...")) {
    if (auto picked = PickFolder("Select the folder holding the DLC packages",
                                 settings_->ResolvedDlcPath())) {
      settings_->dlc_path = picked->string();
      RefreshDlc();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Rescan##dlc"))
    RefreshDlc();

  if (dlc_count_ > 0) {
    ImGui::TextColored(kGood, "%d package%s found", dlc_count_,
                       dlc_count_ == 1 ? "" : "s");
  } else {
    Muted("No packages in this folder. DLC is optional.");
  }
}

// Importing a save.
//
// A save is an STFS package like the DLC, but it has to land under the
// signed-in profile rather than in the downloadable-content store, and there is
// no ContentManager on this screen to do that with - the runtime does not exist
// yet. So the file is only read here, for its name and to reject the wrong
// thing early, and the import itself is queued for the moment the game starts.
void SetupScreen::DrawSaves() {
  SectionHeader("Import saved games",
                "A folder of Xbox 360 save packages for Ninja Gaiden II, as "
                "extracted from a console or unpacked from a save pack. Every "
                "save in it is copied into this profile, and the game then "
                "lists them under LOAD GAME.");
  PathField("##savepath", save_path_.string());
  if (ImGui::Button("Choose a folder of saves...")) {
    if (auto picked = PickFolder("Select a folder containing saves",
                                 save_path_)) {
      save_path_ = *picked;
      RefreshSaves();
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(save_path_.empty());
  if (ImGui::Button("Rescan##saves"))
    RefreshSaves();
  ImGui::EndDisabled();

  if (save_path_.empty()) {
    Muted("Optional. Saves already imported stay where they are.");
    return;
  }

  if (save_count_ > 0) {
    ImGui::TextColored(kGood, "%d save%s found - imported when the game "
                              "starts.", save_count_,
                       save_count_ == 1 ? "" : "s");
    // The first few names, because "18 saves found" and "18 files that happen
    // to be in that folder" look identical until one of them is read out.
    if (!save_message_.empty())
      Muted("%s", save_message_.c_str());
  } else {
    ImGui::TextColored(kBad, "No Ninja Gaiden II saves in that folder.");
  }
}

// Scanning is separate from picking so the Rescan button can reuse it - a save
// pack that gets unzipped after the folder was chosen is otherwise invisible
// until the folder is chosen again, which is the same trap the game folder's
// Rescan fell into.
void SetupScreen::RefreshSaves() {
  const auto found = FindSavePackages(save_path_);
  save_count_ = int(found.size());
  QueueSaveImports(found);

  save_message_.clear();
  for (size_t i = 0; i < found.size() && i < 3; ++i) {
    const SaveInfo info = InspectSavePackage(found[i]);
    if (!save_message_.empty())
      save_message_ += "   ";
    save_message_ += info.message;
  }
  if (found.size() > 3)
    save_message_ += "   ...";
}

// No longer on the setup screen - the key list moved out when the workarounds
// needed the room, and both F4 and the in-game overlay list the binds anyway.
// Kept because the "where settings are kept" note is the only place that is
// written down in the app itself.
// "Copy diagnostics", on both settings surfaces.
//
// The three things a report needs - the log, the settings and what the machine
// is - live in three places, and a report that arrives without them costs a
// round trip to ask. One button writes them into one file and puts its path on
// the clipboard, so sending it is a drag rather than a scavenger hunt.
void DrawDiagnosticsSection() {
  SectionHeader("Report a problem",
                "Gathers this session's log, your settings and what this "
                "machine is into a single text file to attach to a bug "
                "report. It contains no game data.");
  // Held across frames so the result stays readable after the click.
  static std::string status;
  static bool status_ok = false;
  if (ImGui::Button("Copy diagnostics to a file")) {
    const auto result = ng2::WriteDiagnostics(
        Ng2Settings::Path(),
        rex::filesystem::GetExecutableFolder() / "logs");
    status_ok = result.ok;
    if (result.ok) {
      const bool copied = ng2::CopyToClipboard(result.file.string());
      ng2::RevealInExplorer(result.file);
      status = result.file.filename().string();
      status += copied ? " - written, path copied, folder opened"
                       : " - written, folder opened";
    } else {
      status = result.error;
    }
  }
  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(status_ok ? kGood : kBad, "%s", status.c_str());
  }
  Muted("Written to diagnostics\ beside the game. Have a look before you "
        "post it - it names your folders.");
}

void SetupScreen::DrawAbout() {
  SectionHeader("Keys");
  ImGui::BulletText("F10  -  these settings, over the running game");
  ImGui::BulletText("F4   -  every runtime setting, unfiltered");
  ImGui::BulletText("F3   -  frame timing overlay");
  ImGui::BulletText("`    -  console");

  SectionHeader("Where settings are kept");
  Muted("ng2_settings.cfg holds the choices on this screen. cache/"
        "ng2_tuning.toml is rewritten from them on every launch and is not "
        "meant to be edited. ng2.toml is the runtime's own config, written by "
        "the F4 screen.");

  SectionHeader("This screen");
  Muted("It opens on the first run, and any time Shift is held at launch. "
        "Once you press Play it stays out of the way.");

  DrawDiagnosticsSection();
}

void SetupScreen::DrawFooter(float column_width) {
  const bool can_play = game_info_.Usable();

  // An install can run for minutes, and its own progress bar sits far enough
  // down the Content page to be below the fold - which read as a UI that had
  // simply stopped responding. The footer is always on screen, so the state
  // goes here too.
  if (!can_play) {
    ImGui::TextColored(kBad,
                       "Choose a folder containing default.xex, or install "
                       "from a disc image, before starting.");
  } else if (!game_info_.IsNg2()) {
    ImGui::TextColored(kBad, "The selected folder is not Ninja Gaiden II.");
  } else {
    Muted("Ready.");
  }

  if (on_advanced_ && ImGui::Button("Advanced (all cvars)...")) {
    on_advanced_();
  }
  if (on_advanced_ && ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Every cvar this build registers - about 200, of which this screen "
        "wraps roughly twenty. Sharpening, the cache limits and the depth "
        "options were all found in here first.\n\n"
        "IT OVERRIDES THIS SCREEN. What you set here is written to ng2.toml, "
        "which is applied BEFORE these settings on every launch - so a value "
        "set here silently wins, and the row above will look like it is being "
        "ignored. If a setting stops taking effect, look in ng2.toml first.");
  }
  // Play saves too, so this is strictly for reassurance - but "did that take?"
  // is a fair question to have about a settings screen, and answering it costs
  // one button.
  ImGui::SameLine();
  if (ImGui::Button("Save settings")) {
    settings_->Clamp();
    settings_->Save();
    saved_note_ = "Saved - these are the settings the game will start with.";
  }
  if (!saved_note_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kGood, "%s", saved_note_.c_str());
  }

  // Right-align the two decisions inside the centred column, not the window:
  // GetContentRegionMax is obsolete in this ImGui and the window is wider than
  // the content anyway.
  // SameLine's offset is measured from the *indented* line start, so it must
  // not include the column's own left offset - adding that applied the indent
  // twice and pushed Play off the right of the column. Measured: asking for
  // 820 put the button at 1010, exactly one indent too far.
  const float button_w = 130.0f;
  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::SameLine(column_width - button_w * 2.0f - style.ItemSpacing.x);
  if (ImGui::Button("Quit", ImVec2(button_w, 0)) && !finished_) {
    finished_ = true;
    on_done_(false);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_play || progress_.running.load());
  if (ImGui::Button("Play", ImVec2(button_w, 0)) && !finished_) {
    finished_ = true;
    settings_->configured = true;
    settings_->Clamp();
    settings_->Save();
    on_done_(true);
  }
  ImGui::EndDisabled();
}

// --- SettingsOverlay -------------------------------------------------------

SettingsOverlay::SettingsOverlay(rex::ui::ImGuiDrawer* drawer,
                                 Ng2Settings* settings, rex::ui::Window* window,
                                 std::function<void()> on_advanced,
                                 TextureJob* tex_job)
    : ImGuiDialog(drawer),
      settings_(settings),
      window_(window),
      on_advanced_(std::move(on_advanced)),
      tex_job_(tex_job) {}

SettingsOverlay::~SettingsOverlay() {
  // Deliberately does NOT touch the texture run: it is owned by the App
  // (tex_job_), not by this overlay, so closing the settings menu leaves an
  // upscale running. Only an explicit Cancel or the app exiting stops it -
  // which is the whole point of moving it out of here.
}

// Textures.
//
// Three steps in the order they are actually done - dump while playing,
// process once, then use the result - because anything else invites turning
// "use" on before a pack exists and concluding the feature is broken.
bool SettingsOverlay::DrawTextures() {
  Ng2Settings& s = *settings_;
  bool changed = false;

  SectionHeader("Textures",
                "Extract the game's textures, enlarge them with an AI "
                "upscaler, and load the results back in. Dumping costs a file "
                "write the first time each texture is seen; leave it off once "
                "a pack is made.");

  if (!tex_job_->tools_probed) {
    tex_job_->tools = FindTextureTools();
    tex_job_->tools_probed = true;
  }

  const bool have_path = !s.texture_path.empty();
  const std::filesystem::path dir =
      have_path ? s.ResolvedTexturePath() : std::filesystem::path();
  // Counted OFF the UI thread, and not every frame.
  //
  // These two walk the dump and pack folders. That was fine when a pack was a
  // few hundred files and fatal once it was not: at 10,669 dumped and 4,125
  // packed, this scanned ~14,800 directory entries EVERY FRAME the section was
  // open. Opening the menu during a video - when the disk is already busy -
  // produced a 2,318 ms frame, Windows reset the GPU driver for missing its
  // deadline, and the process died with nothing in the log.
  //
  // The counts are advisory: they say "you have a pack" and drive whether a
  // button is enabled. A value a few seconds stale is worth nothing less than a
  // fresh one, and is worth a great deal more than a stall.
  //
  // The numbers are the ones that matter to the player: textures that CAN be
  // enhanced, how many of those are in the pack, how many are waiting. Not
  // "files dumped" - that count included the HUD, fonts and video frames the
  // tool never packs, and "7,778 dumped, 5,907 in the pack" read as 1,871
  // textures missing when nothing was.
  struct TextureCounts {
    std::atomic<int> dumped{0};      // enhanceable textures in the dump
    std::atomic<int> packed{0};      // of those, in the pack
    std::atomic<int> waiting{0};     // of those, not yet in the pack
    std::atomic<int> excluded{0};    // dumped but never packed, by design
    std::atomic<int> tex_files{0};   // .tex files in the pack
    // What the pack says it was made with (pack/pack.txt). 0 = no record.
    std::atomic<int> manifest{0};
    std::atomic<int> pack_scale{0};
    std::atomic<int> pack_upscaler{0};        // 1 Lanczos, 2 Real-ESRGAN
    std::atomic<int> pack_strength_x100{0};
    std::atomic<bool> pack_complete{true};
    std::atomic<bool> counting{false};
    std::atomic<bool> have{false};
    double last = -1.0e9;
    std::string path;
  };
  static TextureCounts counts;
  if (have_path) {
    const std::string dir_str = dir.string();
    const double now = ImGui::GetTime();
    // Re-scan on a path change, otherwise at most every few seconds.
    const bool stale = (counts.path != dir_str) || (now - counts.last > 5.0);
    if (stale && !counts.counting.exchange(true)) {
      counts.path = dir_str;
      counts.last = now;
      std::thread([dir_str] {
        const PackCensus c = CountPack(std::filesystem::path(dir_str));
        counts.dumped.store(c.candidates);
        counts.packed.store(c.packed);
        counts.waiting.store(c.waiting);
        counts.excluded.store(c.excluded);
        counts.tex_files.store(c.tex_files);
        counts.manifest.store(c.manifest ? 1 : 0);
        counts.pack_scale.store(c.pack_scale);
        counts.pack_upscaler.store(c.pack_upscaler == "realesrgan" ? 2
                                   : c.pack_upscaler == "lanczos" ? 1 : 0);
        counts.pack_strength_x100.store(int(c.pack_strength * 100.0f + 0.5f));
        counts.pack_complete.store(c.pack_complete);
        counts.have.store(true);
        counts.counting.store(false);
      }).detach();
    }
  }
  const int dumped = have_path ? counts.dumped.load() : 0;
  const int packed = have_path ? counts.tex_files.load() : 0;
  const int in_pack = have_path ? counts.packed.load() : 0;
  const int waiting = have_path ? counts.waiting.load() : 0;
  const int excluded = have_path ? counts.excluded.load() : 0;
  const bool have_manifest = have_path && counts.manifest.load() != 0;
  const int pack_scale = counts.pack_scale.load();
  const int pack_upscaler = counts.pack_upscaler.load();
  const float pack_strength = float(counts.pack_strength_x100.load()) / 100.0f;
  const bool pack_complete = counts.pack_complete.load();
  const bool busy = tex_job_->progress.running.load();

  {
    TightRows tight;
    // Guarded like every other table here: a BeginTable() that returns false
    // (window collapsed or clipped) has no table for the rows to go into.
    if (!ImGui::BeginTable("texrows", 2, ImGuiTableFlags_SizingStretchProp))
      return changed;

    RowStart("Folder",
             "Where dumped and upscaled textures are kept. Needs room: the "
             "raw dump is roughly the size of the game's texture data.");
    {
      char buf[512];
      std::snprintf(buf, sizeof(buf), "%s", s.texture_path.c_str());
      // Leave room for a Browse button on the same line. Typing a path still
      // works - the field is the fallback for a path pasted from elsewhere -
      // but a picker is what most people expect, and every other folder on the
      // setup screen already has one.
      const float browse_w = 96.0f;
      ImGui::SetNextItemWidth(-(browse_w + ImGui::GetStyle().ItemSpacing.x));
      if (ImGui::InputText("##texpath", buf, sizeof(buf))) {
        s.texture_path = buf;
        changed = true;
      }
      ImGui::SameLine();
      if (ImGui::Button("Browse...##tex", ImVec2(browse_w, 0.0f))) {
        const std::filesystem::path start =
            s.texture_path.empty() ? std::filesystem::path()
                                   : std::filesystem::path(s.texture_path);
        if (auto picked = PickFolder("Select a folder for textures", start)) {
          s.texture_path = picked->string();
          changed = true;
        }
      }
    }

    RowStart("Dump while playing",
             "Writes every texture the game loads. Play the chapters you care "
             "about with this on - a run that only reaches the menu collects "
             "video frames and little else.\n\n"
             "It composes with the pack - the dump reads the GAME's textures, "
             "which a replacement never touches - but running both at once puts "
             "a file write AND a file read on the GPU thread for every new "
             "texture, and during a video that has been seen to stall the "
             "command stream. Prefer one at a time.");
    // One or the other, never both.
    //
    // Dumping writes a file and stats a folder on the GPU thread for every
    // texture the decoder creates; the pack reads one for every texture it
    // replaces. Together they put the disk in the middle of the render thread
    // and starve the command stream - measured, and previously misdiagnosed as
    // a video-mode fault. Making them mutually exclusive is the difference
    // between a documented warning nobody reads and a state that cannot happen.
    ImGui::BeginDisabled(!have_path || s.texture_pack);
    if (ImGui::Checkbox("##texdump", &s.texture_dump)) {
      if (s.texture_dump)
        s.texture_pack = false;
      changed = true;
    }
    ImGui::EndDisabled();
    RestartTag();
    if (s.texture_pack)
      Muted("Turn the pack off first - dumping and loading together stall the "
            "command stream.");
    else if (s.texture_dump)
      Muted("Dumping begins at the NEXT launch and captures the stage from its "
            "start. Turning it on mid-game would miss every texture already "
            "loaded, so it is applied on restart.");

    RowStart("Use the upscaled textures",
             "Loads the finished pack instead of the game's own textures. "
             "Nothing happens until a pack has been made.\n\n"
             "F9 toggles this during play, without opening this screen - which "
             "is the only practical way to compare the pack against the "
             "original, since the difference is in detail the eye loses while a "
             "menu is in the way.\n\n"
             "F9 does NOT change this setting. It is a look, not a decision, so "
             "leaving a comparison half-finished cannot quietly turn the pack "
             "off for the next launch. This checkbox is what persists.");
    ImGui::BeginDisabled(!have_path || packed == 0 || s.texture_dump);
    if (ImGui::Checkbox("##texuse", &s.texture_pack)) {
      if (s.texture_pack)
        s.texture_dump = false;
      changed = true;
    }
    ImGui::EndDisabled();
    if (s.texture_dump)
      Muted("Turn dumping off first - see above.");
    else if (have_path && packed == 0)
      Muted("No pack yet. Dump some textures and press Process textures.");

    ImGui::EndTable();
  }

  // Say the shortcut in the panel itself, not only in a tooltip: a key nobody
  // is told about is a key nobody presses.
  if (have_path && counts.have.load()) {
    if (dumped == 0)
      Muted("No textures dumped yet.");
    else if (waiting == 0)
      Muted("%d textures can be enhanced - all %d are in the pack.  Press F9 in "
            "game to switch the pack on and off and see the difference.",
            dumped, in_pack);
    else
      Muted("%d textures can be enhanced: %d in the pack, %d waiting to be "
            "processed.", dumped, in_pack, waiting);
    if (excluded > 0)
      Muted("(%d more were dumped but are never enhanced - HUD, fonts, video "
            "frames and other non-art - so they are not counted.)", excluded);
  }
  // Whether the enhanced textures are in use RIGHT NOW, from the renderer
  // itself rather than from the checkbox: the checkbox is what persists, F9
  // is what is live, and the two can differ mid-comparison. The counters are
  // the plugin's own, read across the DLL boundary by name.
  if (have_path && packed > 0) {
    const bool live = !rex::cvar::Query<std::string>("texture_pack_path").empty();
    const int32_t replaced = rex::cvar::Query<int32_t>("texture_pack_replaced");
    // Deliberately NOT a live count. The plugin's replaced/original counters
    // are per-scene - they count only the textures resident for what is on
    // screen this frame - so a number here ticks constantly and, worse, reads
    // as "only 57 textures were ever enhanced" when the pack holds thousands.
    // The stable, true figure is the census line above ("N can be enhanced: M
    // in the pack"). All that is worth saying live is whether the pack is
    // actually reaching the screen, which is a yes/no, not a count.
    if (!live)
      Muted("Enhanced textures: OFF - the game is showing its original textures.  "
            "Tick 'Use the upscaled textures' or press F9.");
    else if (replaced > 0)
      Muted("Enhanced textures: ON and in use - the pack is replacing textures "
            "on screen now.  F9 switches.");
    else
      Muted("Enhanced textures: ON - nothing on this screen is in the pack yet, "
            "so it looks unchanged here.  F9 switches.");
  }
  if (s.texture_dump || s.texture_pack)
    Muted("Takes effect next launch - the GPU reads these at startup.");

  // --- the run -----------------------------------------------------------
  if (busy) {
    // The run has steps - decode everything, then upscale the art - and the
    // script reports each as its own bar. A new step restarts the bar AND the
    // clock: with one clock the estimate for the upscaling step included the
    // whole of the decoding step's time and quoted 228 minutes for a
    // 26-minute job, right after showing 100%.
    const int phase = tex_job_->progress.phase.load();
    const int phases = tex_job_->progress.phases.load();
    if (phase != tex_phase_seen_) {
      tex_phase_seen_ = phase;
      tex_started_at_ = ImGui::GetTime();
    }

    const double done = double(tex_job_->progress.files_done.load());
    const double total = double(tex_job_->progress.files_total.load());
    float fraction = total > 0.0 ? float(done / total) : 0.0f;
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);

    char step[40] = "";
    if (phases > 0)
      std::snprintf(step, sizeof(step), "Step %d of %d  -  ", phase, phases);

    // Time remaining, from the rate so far in THIS step. Shown only once there
    // is enough done to mean anything - an estimate off the first file is a
    // guess with a number on it, which is worse than no number.
    char overlay[128];
    const double elapsed = ImGui::GetTime() - tex_started_at_;
    if (done >= 3.0 && elapsed > 1.0 && total > done) {
      const double remain = (elapsed / done) * (total - done);
      if (remain >= 90.0)
        std::snprintf(overlay, sizeof(overlay), "%s%.0f%%  -  about %.0f min left",
                      step, fraction * 100.0f, remain / 60.0);
      else
        std::snprintf(overlay, sizeof(overlay), "%s%.0f%%  -  about %.0f s left",
                      step, fraction * 100.0f, remain);
    } else {
      std::snprintf(overlay, sizeof(overlay), "%s%.0f%%", step, fraction * 100.0f);
    }
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
    const std::string phase_label = tex_job_->progress.PhaseLabel();
    if (!phase_label.empty())
      Muted("%s", phase_label.c_str());
    Muted("%s", tex_job_->progress.CurrentFile().c_str());
    // The runner polls this and kills the whole process tree - the Python
    // launcher, the interpreter and the upscaler - within a moment.
    if (tex_job_->progress.cancel.load()) {
      Muted("Stopping...");
    } else if (ImGui::Button("Cancel")) {
      tex_job_->progress.cancel = true;
    }
  } else {
    if (tex_job_->thread.joinable())
      tex_job_->thread.join();

    // How far to enlarge. The warning next to 4x and 8x is the measured cost,
    // not a hedge: replacements are uncompressed, so a 4x pack of this size
    // came to 5.9 GB against a 4 GB maximum cache and thrashed hard enough to
    // produce two-second hitches.
    static const int kScales[] = {2, 4, 8};
    static const char* const kScaleNames[] = {
        "2x  (about 1.5 GB - recommended)",
        "4x  (about 6 GB - needs the 8 GB cache, may still hitch)",
        "8x  (about 24 GB - for future hardware)"};
    int scale_index = 0;
    for (int i = 0; i < IM_ARRAYSIZE(kScales); ++i)
      if (kScales[i] == s.texture_scale) scale_index = i;
    ImGui::TextUnformatted("Upscale");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(330.0f);
    if (ImGui::Combo("##texscale", &scale_index, kScaleNames, IM_ARRAYSIZE(kScaleNames))) {
      s.texture_scale = kScales[scale_index];
      changed = true;
    }

    // Live cost, beside the switches that cause it.
    //
    // The texture pack's price is video memory and the copies that go with it,
    // and until this was here the only way to find that out was to play until
    // it stuttered. Toggling the pack with F9 while watching these shows the
    // difference immediately, which is the whole reason they sit in THIS
    // section rather than on a diagnostics page.
    if (s.hud_menu_bars) {
      const PerfSample p = GetPerfSample();
      ImGui::TextUnformatted("Live cost");
      ImGui::SameLine(140.0f);
      ImGui::SetNextItemWidth(150.0f);
      char cpu_text[32];
      std::snprintf(cpu_text, sizeof(cpu_text), "CPU %.0f%%", p.cpu_percent);
      ImGui::ProgressBar(p.cpu_percent / 100.0f, ImVec2(150.0f, 0.0f), cpu_text);
      ImGui::SameLine();
      char gpu_text[32];
      if (p.gpu_valid)
        std::snprintf(gpu_text, sizeof(gpu_text), "GPU %.0f%%", p.gpu_percent);
      else
        std::snprintf(gpu_text, sizeof(gpu_text), "GPU n/a");
      ImGui::ProgressBar(p.gpu_valid ? p.gpu_percent / 100.0f : 0.0f,
                         ImVec2(150.0f, 0.0f), gpu_text);
      if (p.vram_valid) {
        ImGui::TextUnformatted("Video memory");
        ImGui::SameLine(140.0f);
        char vram_text[64];
        std::snprintf(vram_text, sizeof(vram_text), "%.1f / %.1f GB",
                      p.vram_mb / 1024.0f, p.vram_total_mb / 1024.0f);
        ImGui::ProgressBar(p.vram_total_mb > 0.0f ? p.vram_mb / p.vram_total_mb : 0.0f,
                           ImVec2(306.0f, 0.0f), vram_text);
        Muted("This process only - other applications on the GPU are not counted.");
      }
      ImGui::Spacing();
    }

    // The AI upscaler, and the download that enables it. Not shipped with the
    // port: 43 MB of third-party binary under its own licence, so whether it is
    // on the machine stays the player's decision.
    const bool ai_ready = have_path && UpscalerInstalled(dir);

    // A method, not a tick-box.
    //
    // These are two upscalers, not "off" and "on": unticking the old checkbox
    // did not disable enhancement, it selected Lanczos - which is what
    // upscale_textures.py falls back to and is a perfectly good result. A
    // checkbox called "Enhance with AI" implies the alternative is no
    // enhancement at all, and that is simply wrong about what the pack
    // contains.
    //
    // The AI entry stays selectable only while the upscaler is actually
    // installed, so the list can never offer a method that would fail.
    // Plain label, NOT RowStart: this is outside the table that ended above,
    // and RowStart calls ImGui::TableNextRow, which dereferences the current
    // table without checking. Called with no table open it reads through a
    // null pointer and takes the process with it - which is exactly what it
    // did here, as an access violation in rexruntime at ImGui::TableNextRow.
    ImGui::TextUnformatted("Upscaler");
    HelpMarker("Lanczos is a high-quality resample and needs nothing extra. "
               "Real-ESRGAN is a trained model that adds detail; the port "
               "ships it under tools/upscaler, and it needs a Vulkan-capable "
               "GPU.");
    const char* upscalers[] = {"Lanczos (built in)",
                               "Real-ESRGAN AI (adds detail)"};
    int upscaler_index = (s.texture_ai && ai_ready) ? 1 : 0;
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("##upscaler", &upscaler_index, upscalers,
                     ai_ready ? 2 : 1)) {
      s.texture_ai = upscaler_index == 1;
      changed = true;
    }
    // A setting that survived from a machine with the upscaler installed must
    // not silently claim AI on one without it.
    if (s.texture_ai && !ai_ready) {
      s.texture_ai = false;
      changed = true;
    }

    if (ai_ready) {
      ImGui::SetNextItemWidth(240.0f);
      changed |= ImGui::SliderFloat("Detail strength", &s.texture_ai_strength,
                                    0.0f, 1.0f, "%.2f");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "How much of the model's fine detail is laid over the original.\n"
            "The tone and colour always stay the game's - only the detail is\n"
            "borrowed, so a higher value sharpens rather than repaints.");
      }
    } else if (have_path) {
      Muted("The AI upscaler is missing from tools/upscaler; a copy can be "
            "downloaded into the texture folder instead.");
      // Say what it needs BEFORE the button, so the requirements are not
      // something discovered by a failure.
      Muted("It needs: Python (for the download and the packing step), a "
            "Vulkan-capable GPU, about 100 MB of disk for the tool, and roughly "
            "1.5 GB per 1000 textures at 2x. Nothing is installed into your "
            "Python - the upscaler is a standalone executable that lives in the "
            "texture folder and can be deleted at any time.");
      Muted("Source: the official Real-ESRGAN release (xinntao/Real-ESRGAN), "
            "under its own licence (BSD-3). The port ships a copy under "
            "tools/upscaler; this download replaces one that went missing.");
      const bool downloading = tex_job_->progress.running.load();
      ImGui::BeginDisabled(downloading || tex_job_->tools.python.empty());
      if (ImGui::Button("Download AI upscaler (43 MB)")) {
        tex_started_at_ = ImGui::GetTime();
        tex_phase_seen_ = 0;
        tex_job_->thread = DownloadUpscalerAsync(tex_job_->tools, dir, tex_job_->progress);
      }
      ImGui::EndDisabled();
      if (tex_job_->tools.python.empty())
        Muted("Python is needed to download it.");
    }
    ImGui::Spacing();

    // What the pack was made with, against what is selected now. A pack made
    // with other settings cannot be topped up - the new textures would not
    // match the old - so a difference forces a full run, and is said here
    // rather than discovered after half an hour.
    const bool want_ai = s.texture_ai && ai_ready;
    const int want_upscaler = want_ai ? 2 : 1;
    const bool settings_differ =
        have_manifest &&
        (pack_scale != s.texture_scale || pack_upscaler != want_upscaler ||
         (want_ai && std::fabs(pack_strength - s.texture_ai_strength) > 0.005f));
    // A pack whose last run stopped halfway is NOT a reason to redo it: every
    // texture it wrote is whole and made with its recorded settings, and the
    // tool redoes any file a stop cut short. Requiring completion here made a
    // full-disk failure 2,449 textures into a run cost all 22,026 again.
    const bool must_redo = packed > 0 && (!have_manifest || settings_differ);
    if (have_manifest) {
      char made[80];
      if (pack_upscaler == 2)
        std::snprintf(made, sizeof(made), "%dx, Real-ESRGAN, detail %.2f",
                      pack_scale, pack_strength);
      else
        std::snprintf(made, sizeof(made), "%dx, Lanczos", pack_scale);
      if (!pack_complete)
        Muted("The pack's last run was stopped halfway (%s) - the next run continues "
              "with the textures still missing.", made);
      else if (settings_differ)
        Muted("The pack was made at %s, which differs from the settings above - "
              "the next run redoes every texture.", made);
      else
        Muted("The pack was made at %s - matches the settings above.", made);
    } else if (packed > 0) {
      Muted("The pack does not say what it was made with (made before v1.0.2) - "
            "the next run redoes every texture and records it.");
    }

    // Only what is missing, unless asked - or forced - to redo. A pack of
    // ~6,000 takes half an hour with the AI; the textures dumped since take
    // minutes, and redoing everything to get them was the only option before.
    bool redo_box = must_redo || tex_redo_all_;
    ImGui::BeginDisabled(must_redo);
    if (ImGui::Checkbox("Redo textures already in the pack", &redo_box) && !must_redo)
      tex_redo_all_ = redo_box;
    ImGui::EndDisabled();
    HelpMarker("Off: only textures not yet in the pack are processed, which is "
               "quick. On: every texture is done again - use it after changing "
               "the upscale factor, the upscaler or the detail strength. Ticked "
               "and greyed means the pack no longer matches the settings, so the "
               "full run is required.");
    const bool redo = must_redo || tex_redo_all_;

    const bool nothing_to_do = !redo && waiting == 0;
    const bool can_run =
        have_path && dumped > 0 && !tex_job_->tools.python.empty() && !nothing_to_do;
    // The button says how much work it is: "Process 128 waiting textures" is
    // a different decision from "Process all 5,907 textures".
    char run_label[80];
    if (redo)
      std::snprintf(run_label, sizeof(run_label), "Process all %d textures##texrun", dumped);
    else
      std::snprintf(run_label, sizeof(run_label), "Process %d waiting texture%s##texrun",
                    waiting, waiting == 1 ? "" : "s");
    // Exactly what the run will do, stated before the button is pressed.
    if (can_run) {
      char detail[40] = "";
      if (want_ai)
        std::snprintf(detail, sizeof(detail), ", detail %.2f", s.texture_ai_strength);
      Muted("Will process %s at %dx with %s%s.",
            redo ? "every texture that can be enhanced" : "only the waiting textures",
            s.texture_scale, want_ai ? "Real-ESRGAN" : "Lanczos", detail);
    }
    ImGui::BeginDisabled(!can_run);
    if (ImGui::Button(run_label)) {
      tex_started_at_ = ImGui::GetTime();
      tex_phase_seen_ = 0;
      tex_job_->thread = UpscaleTexturesAsync(tex_job_->tools, dir, /*upscale=*/true,
                                         s.texture_scale, want_ai,
                                         s.texture_ai_strength,
                                         /*only_missing=*/!redo,
                                         tex_job_->progress);
    }
    ImGui::EndDisabled();

    // Say what is missing rather than leaving a dead button. A greyed control
    // with no reason reads as a broken app.
    if (!have_path)
      Muted("Set a folder first.");
    else if (dumped == 0)
      Muted("Nothing dumped yet - turn dumping on and play.");
    else if (tex_job_->tools.python.empty())
      Muted("Python was not found, and the upscaler needs it.");
    else if (nothing_to_do)
      Muted("Nothing is waiting - every texture that can be enhanced is in the "
            "pack. Tick Redo to rebuild it.");

    if (tex_job_->progress.failed.load())
      Muted("%s", tex_job_->progress.Error().c_str());
    else if (tex_job_->progress.complete.load())
      Muted("Done - %d texture(s) in the pack.", packed);
  }

  return changed;
}

bool SettingsOverlay::DrawUpdates() {
  SectionHeader("Updates");
  bool changed = false;

  bool check = settings_->check_for_updates;
  if (ImGui::Checkbox("Check for updates when the game starts", &check)) {
    settings_->check_for_updates = check;
    settings_->Save();
    changed = true;
  }
  HelpMarker(
      "On launch the game asks GitHub whether a newer version exists and, if "
      "so, offers to download and install it. The check runs in the background "
      "and never delays startup. Turn it off to keep launch offline - the "
      "'Check now' button below still works by hand.");

  const ng2::update::Snapshot s = ng2::update::Get();
  ImGui::Spacing();

  using P = ng2::update::Phase;
  switch (s.phase) {
    case P::kChecking:
      Muted("Checking for updates...");
      break;
    case P::kUpToDate:
      Muted("You have v%s - the latest version.", s.current_version.c_str());
      break;
    case P::kAvailable:
      Muted("v%s is available (you have v%s).", s.latest_version.c_str(),
            s.current_version.c_str());
      ImGui::Spacing();
      if (ImGui::Button("Download and install")) {
        ng2::update::BeginUpdate();
      }
      break;
    case P::kDownloading: {
      Muted("Downloading v%s...", s.latest_version.c_str());
      const float frac =
          s.bytes_total > 0 ? static_cast<float>(s.progress) : 0.0f;
      std::string label = s.bytes_total > 0
                              ? FormatBytes(s.bytes_done) + " / " +
                                    FormatBytes(s.bytes_total)
                              : FormatBytes(s.bytes_done);
      ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), label.c_str());
      break;
    }
    case P::kReadyToApply:
      Muted("v%s is ready. The game will close, install it, and reopen.",
            s.latest_version.c_str());
      ImGui::Spacing();
      if (ImGui::Button("Restart and install now")) {
        ng2::update::ApplyNow();
      }
      break;
    case P::kApplying:
      Muted("Installing v%s... the game will restart.",
            s.latest_version.c_str());
      break;
    case P::kError:
      Muted("Update failed: %s", s.error.c_str());
      break;
    case P::kIdle:
    default:
      Muted("You have v%s.", s.current_version.c_str());
      break;
  }

  ImGui::Spacing();
  const bool busy = (s.phase == P::kChecking || s.phase == P::kDownloading ||
                     s.phase == P::kApplying);
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Check now")) {
    ng2::update::CheckAsync();
  }
  ImGui::EndDisabled();

  return changed;
}

void SettingsOverlay::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowSize(ImVec2(680, 620), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  // Before Begin, so the title bar is drawn in the menu's font too. Pushed
  // after it, the window keeps the SDK's 13px bitmap face and the title reads
  // as a different application from its own contents.
  PushMenuFont(Fonts().body, Fonts().body_size);
  // The version is in the title because this window is the one thing always
  // reachable while playing, and "which build am I actually running" turned out
  // to be a question worth being able to answer without reading a log.
  char title[96];
  std::snprintf(title, sizeof(title), "Ninja Gaiden II - Settings  (v%s)",
                NG2_VERSION);
  // No collapse arrow. Collapsing made Begin() return false while the body
  // below kept submitting widgets; the texture table was begun without a
  // check and its first row read through a null table pointer (crash dump
  // 2026-09-11 23:37, ImGui::TableNextRow). A collapsed settings window is
  // of no use anyway; F10 closes it.
  if (!ImGui::Begin(title, nullptr,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    ImGui::PopFont();
    return;
  }

  PageOptions opts;
  opts.restart_bound_editable = false;

  bool changed = false;
  ImGui::BeginChild("body", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f));
  changed |= DrawSettings(*settings_, opts);
  changed |= DrawWorkarounds(*settings_, opts);

  changed |= DrawTextures();

  SectionHeader("Content");
  PathField("##gamepath", settings_->ResolvedGamePath().string());
  PathField("##dlcpath", settings_->ResolvedDlcPath().string());
  ImGui::Spacing();
  Muted("Game data and DLC are chosen on the setup screen, which runs before "
        "the game is loaded. Saves are imported there too.");

  DrawUpdates();

  DrawDiagnosticsSection();

  // Getting back to that screen used to mean knowing to hold Shift while
  // launching, which is not something anyone discovers. It lives here, in the
  // scrolling body, rather than beside the buttons: put in the footer it grew
  // the fixed area by a row and pushed Save off the bottom of the window.
  bool setup_next = !settings_->configured;
  if (ImGui::Checkbox("Show the setup screen at the next launch",
                      &setup_next)) {
    settings_->configured = !setup_next;
    settings_->Save();
    status_ = setup_next ? "The setup screen will open next time."
                         : "The setup screen will be skipped next time.";
  }
  HelpMarker("The screen that picks the game folder, installs a disc image and "
             "imports saves. Holding Shift while launching still works too.");
  ImGui::EndChild();

  if (changed) {
    // Everything the presenter re-reads takes effect on the next paint. The
    // rest is written to disk and waits for the next launch, which is what the
    // greyed rows above are telling the player.
    ApplyLiveSettings(*settings_, window_);
  }

  ImGui::Separator();
  // Nothing is greyed any more (v1.0.0 made every row editable), so the old
  // "greyed settings are fixed for this session" line described controls that
  // no longer exist and contradicted the red tag beside the rows it meant.
  Muted("Settings marked 'restart required' are saved now and applied on the "
        "next launch - the window and the guest video mode are built during "
        "startup.");


  if (on_advanced_ && ImGui::Button("Advanced (all cvars)...")) {
    on_advanced_();
  }
  if (on_advanced_ && ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Every cvar this build registers - about 200, of which this screen "
        "wraps roughly twenty. Sharpening, the cache limits and the depth "
        "options were all found in here first.\n\n"
        "IT OVERRIDES THIS SCREEN. What you set here is written to ng2.toml, "
        "which is applied BEFORE these settings on every launch - so a value "
        "set here silently wins, and the row above will look like it is being "
        "ignored. If a setting stops taking effect, look in ng2.toml first.");
  }
  ImGui::SameLine();
  if (ImGui::Button("Save settings")) {
    settings_->Clamp();
    settings_->Save();
    status_ = "Saved - these are the settings the game will start with.";
  }
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kGood, "%s", status_.c_str());
  }

  ImGui::End();
  ImGui::PopFont();
}

// A line of text over the running game.
//
// Drawn on the FOREGROUND draw list rather than in a window, for the same
// reason the video overlay is: an ImGui window is not composited over the
// guest's own rendering while the game is drawing, and would simply not appear.
}  // namespace ng2
