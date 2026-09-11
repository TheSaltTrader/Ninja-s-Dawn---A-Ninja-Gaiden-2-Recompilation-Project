// The texture-pack indicator: a line in the top-left corner saying which set of
// textures you are looking at.
//
// It exists because the F9 comparison is otherwise ambiguous. Switching the
// pack on and off changes the picture, but not always in a way that is obvious
// on the surface currently filling the screen - and when the difference is
// subtle, "which one am I looking at right now?" is exactly the question that
// makes the comparison useless. The indicator answers it, and the counts say
// how much of the scene each mode actually covers.
//
// It is deliberately NOT a permanent HUD element. It appears when the mode
// changes and fades out, so it is present at the moment of comparison and gone
// for normal play.

#pragma once

#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

// Global namespace, like its definition in ng2_settings.h - declaring it inside
// ng2 would quietly create a DIFFERENT type.
struct Ng2Settings;

namespace ng2 {

// Is the texture cache still being warmed for this stage, and how far along?
//
// The answer comes from the GPU plugin through cvars, because the plugin is a
// separate module: `texture_warm_total` is 0 when nothing is warming.
// `fraction` is 0..1 and is only meaningful while `warming` is true.
struct WarmState {
  bool warming = false;
  float fraction = 0.0f;
  int done = 0;
  int total = 0;
};
WarmState GetWarmState();

// The cache bar, top left. Blue while filling, green at 100%.
class WarmOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit WarmOverlay(rex::ui::ImGuiDrawer* drawer);
  ~WarmOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Call when the pack is switched on or off. Shows the indicator for a few
// seconds; safe to call when no overlay exists.
void NotifyTexturePack(bool enabled);

// Points the HUD at the live settings so its three switches take effect
// immediately rather than at the next launch.
void SetHudSettings(const Ng2Settings* settings);

// FPS / GPU / video memory, top right. Always present, draws only what is
// switched on - and counts presented frames even when nothing is shown, so
// turning the readout on does not start from a blank rate.
class PerfHudOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PerfHudOverlay(rex::ui::ImGuiDrawer* drawer);
  ~PerfHudOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

class TextureNotifyOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit TextureNotifyOverlay(rex::ui::ImGuiDrawer* drawer);
  ~TextureNotifyOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace ng2
