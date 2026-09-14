#include "ng2_texnotify.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ng2_menu.h"  // Fonts()
#include "ng2_perf.h"
#include "ng2_settings.h"

namespace ng2 {
namespace {

// How long the indicator stays up, and how much of that is spent fading. Long
// enough to read after pressing the key, short enough not to sit over the game.
constexpr double kHoldSeconds = 3.5;
constexpr double kFadeSeconds = 0.9;

std::atomic<double> g_shown_at{-1000.0};
std::atomic<bool> g_enabled{false};

double NowSeconds() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration<double>(clock::now() - t0).count();
}

}  // namespace

// The settings the HUD reads. Pointed at the app's own settings object so the
// readouts follow the checkboxes with no copying and no staleness.
static const Ng2Settings* g_hud_settings = nullptr;
void SetHudSettings(const Ng2Settings* settings) { g_hud_settings = settings; }

void NotifyTexturePack(bool enabled) {
  g_enabled.store(enabled, std::memory_order_release);
  g_shown_at.store(NowSeconds(), std::memory_order_release);
}

TextureNotifyOverlay::TextureNotifyOverlay(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}

TextureNotifyOverlay::~TextureNotifyOverlay() = default;

void TextureNotifyOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  const double age = NowSeconds() - g_shown_at.load(std::memory_order_acquire);
  if (age < 0.0 || age > kHoldSeconds)
    return;

  float alpha = 1.0f;
  if (age > kHoldSeconds - kFadeSeconds)
    alpha = float((kHoldSeconds - age) / kFadeSeconds);
  alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

  const bool enabled = g_enabled.load(std::memory_order_acquire);

  // Read across the DLL boundary: these are defined by the GPU plugin, which is
  // not on this executable's link line, so the registry is the only route.
  const int32_t replaced = REXCVAR_QUERY(int32_t, texture_pack_replaced);
  const int32_t original = REXCVAR_QUERY(int32_t, texture_pack_original);

  char line1[128];
  char line2[160];
  std::snprintf(line1, sizeof(line1), "%s",
                enabled ? "ENHANCED TEXTURES" : "ORIGINAL TEXTURES");
  if (enabled) {
    std::snprintf(line2, sizeof(line2), "%d enhanced loaded, %d original (not in pack)",
                  replaced, original);
  } else {
    std::snprintf(line2, sizeof(line2), "%d original loaded - the pack is off", original);
  }

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24.0f, vp->WorkPos.y + 22.0f));
  ImGui::SetNextWindowBgAlpha(0.55f * alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  // NoInputs matters: this must never take a click away from the game, and it
  // is drawn while the player is holding a controller, not a mouse.
  if (ImGui::Begin("##ng2_texnotify", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove)) {
    // Green when the pack is on, a muted grey-green when it is off, so the two
    // states are told apart at a glance and not only by reading the words.
    const ImVec4 on(0.30f, 0.95f, 0.42f, alpha);
    const ImVec4 off(0.62f, 0.72f, 0.64f, alpha);
    if (ImFont* f = Fonts().title)
      ImGui::PushFont(f);
    ImGui::TextColored(enabled ? on : off, "%s", line1);
    if (Fonts().title)
      ImGui::PopFont();
    ImGui::TextColored(ImVec4(0.85f, 0.90f, 0.86f, alpha * 0.92f), "%s", line2);
    ImGui::TextColored(ImVec4(0.70f, 0.76f, 0.71f, alpha * 0.75f), "F9 to switch");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void PerfHudOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  // Counted here rather than on a timer: this runs once per PRESENTED frame, so
  // it measures frames the player actually saw.
  PerfFrameTick();

  const Ng2Settings* s = g_hud_settings;
  if (!s || !s->hud_enabled ||
      (!s->hud_fps && !s->hud_gpu && !s->hud_vram && !s->hud_cpu))
    return;

  const PerfSample p = GetPerfSample();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x - 18.0f, vp->WorkPos.y + 18.0f),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.42f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
  if (ImGui::Begin("##ng2_perfhud", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove)) {
    const ImVec4 good(0.55f, 0.95f, 0.60f, 1.0f);
    const ImVec4 warn(0.98f, 0.82f, 0.35f, 1.0f);
    const ImVec4 bad(0.98f, 0.45f, 0.40f, 1.0f);
    const ImVec4 label(0.72f, 0.78f, 0.74f, 1.0f);
    // Fixed per-stat bar colours so each bar is told apart at a glance,
    // independent of the value: FPS blue, CPU and GPU red, VRAM green.
    const ImVec4 blue(0.40f, 0.66f, 0.98f, 1.0f);
    const ImVec4 red = bad;
    const ImVec4 green = good;

    // A single-value bar under a readout, coloured to match the number on a dim
    // track - the instantaneous style of the Fable II overlay. Only drawn when
    // the player has bars on.
    const bool bars = s->hud_bars;
    auto Bar = [&](float fraction, const ImVec4& col) {
      if (!bars)
        return;
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);  // ProgressBar fill
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.18f, 0.17f, 0.85f));
      ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(176.0f, 7.0f), "");
      ImGui::PopStyleColor(2);
    };

    if (s->hud_fps) {
      // Against 60, which is what this title targets.
      const ImVec4 c = p.fps >= 57.0f ? good : (p.fps >= 45.0f ? warn : bad);
      ImGui::TextColored(label, "FPS");
      ImGui::SameLine();
      ImGui::TextColored(c, "%5.1f", p.fps);
      Bar(p.fps / 60.0f, blue);
    }
    if (s->hud_cpu) {
      static const float cores =
          float(std::max(1u, std::thread::hardware_concurrency()));
      const float busy = p.cpu_percent / 100.0f * cores;
      const ImVec4 c =
          p.cpu_percent < 50.0f ? good : (p.cpu_percent < 80.0f ? warn : bad);
      ImGui::TextColored(label, "CPU");
      ImGui::SameLine();
      ImGui::TextColored(c, "%5.0f%%", p.cpu_percent);
      ImGui::SameLine();
      ImGui::TextColored(label, " %.1f of %.0f", busy, cores);
      Bar(p.cpu_percent / 100.0f, red);
    }
    if (s->hud_gpu) {
      ImGui::TextColored(label, "GPU");
      ImGui::SameLine();
      if (p.gpu_valid) {
        const ImVec4 c = p.gpu_percent < 80.0f ? good : (p.gpu_percent < 95.0f ? warn : bad);
        ImGui::TextColored(c, "%5.0f%%", p.gpu_percent);
        Bar(p.gpu_percent / 100.0f, red);
      } else {
        // Never a zero that looks like an idle GPU.
        ImGui::TextColored(label, "    n/a");
      }
    }
    if (s->hud_vram) {
      ImGui::TextColored(label, "VRAM");
      ImGui::SameLine();
      if (p.vram_valid) {
        const float frac = p.vram_total_mb > 0.0f ? p.vram_mb / p.vram_total_mb : 0.0f;
        const ImVec4 c = frac < 0.7f ? good : (frac < 0.9f ? warn : bad);
        ImGui::TextColored(c, "%.1f / %.1f GB", p.vram_mb / 1024.0f,
                           p.vram_total_mb / 1024.0f);
        Bar(frac, green);
      } else {
        ImGui::TextColored(label, "n/a");
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

WarmState GetWarmState() {
  WarmState w;
  // Read across the DLL boundary: these are the GPU plugin's, and the registry
  // is the only route to them from here.
  w.total = REXCVAR_QUERY(int32_t, texture_warm_total);
  w.done = REXCVAR_QUERY(int32_t, texture_warm_done);
  w.warming = w.total > 0 && w.done < w.total;
  w.fraction = w.total > 0 ? float(w.done) / float(w.total) : 0.0f;
  if (w.fraction > 1.0f)
    w.fraction = 1.0f;
  return w;
}

WarmOverlay::WarmOverlay(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}
WarmOverlay::~WarmOverlay() = default;

void WarmOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  // Held briefly at 100% so the bar is SEEN to finish. Vanishing the instant
  // the last file lands makes a fast stage look like nothing happened, and
  // leaves the player wondering whether it ran at all.
  static double full_at = -1000.0;
  const WarmState w = GetWarmState();
  const double now = ImGui::GetTime();
  if (w.warming)
    full_at = now;
  const bool linger = (now - full_at) < 1.2;
  if (!w.warming && !linger)
    return;

  const float fraction = w.warming ? w.fraction : 1.0f;
  const bool complete = fraction >= 0.999f;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24.0f, vp->WorkPos.y + 22.0f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  if (ImGui::Begin("##ng2_warm", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoMove)) {
    if (ImFont* f = Fonts().title)
      ImGui::PushFont(f);
    ImGui::TextColored(complete ? ImVec4(0.30f, 0.95f, 0.42f, 1.0f)
                                : ImVec4(0.45f, 0.70f, 1.00f, 1.0f),
                       complete ? "TEXTURE CACHE READY" : "LOADING TEXTURE CACHE");
    if (Fonts().title)
      ImGui::PopFont();

    // Blue while filling, green when full - the colour is the state, so it
    // reads at a glance without anyone parsing the numbers.
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                          complete ? ImVec4(0.24f, 0.80f, 0.35f, 1.0f)
                                   : ImVec4(0.26f, 0.55f, 0.95f, 1.0f));
    char overlay_text[48];
    std::snprintf(overlay_text, sizeof(overlay_text), "%d / %d  (%.0f%%)",
                  w.warming ? w.done : w.total, w.total ? w.total : w.done,
                  fraction * 100.0f);
    ImGui::ProgressBar(fraction, ImVec2(300.0f, 0.0f), overlay_text);
    ImGui::PopStyleColor();
    if (!complete)
      ImGui::TextColored(ImVec4(0.80f, 0.86f, 0.82f, 0.85f),
                         "Please wait - loading this stage's textures.");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

PerfHudOverlay::PerfHudOverlay(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}
PerfHudOverlay::~PerfHudOverlay() = default;

}  // namespace ng2
