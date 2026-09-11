// The settings menu, in its two surfaces.
//
// SetupScreen runs *before the guest boots*, from ReXApp::OnFinalizePaths -
// the one hook where the window and the ImGui drawer are already live but the
// runtime has not been constructed yet. That is what makes choosing the game
// data possible at all: the path it returns is the one the runtime mounts.
//
// SettingsOverlay is the same settings over a running game, on F10. Anything
// the presenter or the GPU plugin re-reads per frame is applied immediately;
// anything latched during startup is shown disabled with the reason, rather
// than accepted and silently ignored.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

#include "ng2_disc.h"
#include "ng2_hwdetect.h"
#include "ng2_textool.h"
#include "ng2_settings.h"

namespace rex::ui {
class Window;
}

// The port's own version, baked in from the VERSION file by CMake. Falls back
// only if something is built outside it, which nothing supported does.
#ifndef NG2_VERSION
#define NG2_VERSION "dev"
#endif

namespace ng2 {

// The menu's own fonts, loaded from ReXApp::OnConfigureFonts. Both may be null
// - the SDK's built-in 13px bitmap font is the fallback - so every use goes
// through a push that tolerates it, and a missing font file costs looks
// rather than function.
struct MenuFonts {
  ImFont* body = nullptr;
  ImFont* title = nullptr;
  float body_size = 18.0f;
  float title_size = 30.0f;
};
MenuFonts& Fonts();

// Loads them into the atlas the SDK is about to build. Has to happen in
// OnConfigureFonts: the atlas is finalised straight afterwards.
void LoadMenuFonts(ImFontAtlas* atlas);

// The menu's look. Applied globally, so the SDK's own overlays inherit it too
// - which is the point: one application, one theme.
void ApplyMenuStyle(ImGuiStyle& style);

// Pushes the hot-reloadable settings into their cvars, and the window state
// onto the window. Safe to call before the GPU plugin exists: a cvar that is
// not registered yet is skipped and logged, not guessed at.
void ApplyLiveSettings(const Ng2Settings& settings, rex::ui::Window* window);

class SetupScreen final : public rex::ui::ImGuiDialog {
 public:
  // on_done(true) = Play was pressed and `settings` holds the choices;
  // on_done(false) = the user quit. It is invoked from inside OnDraw, so the
  // app must defer any destruction of this object to the next UI tick.
  SetupScreen(rex::ui::ImGuiDrawer* drawer, Ng2Settings* settings,
              std::function<void(bool)> on_done,
              std::function<void()> on_advanced);
  ~SetupScreen() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawContent();
  void DrawInstaller();
  void DrawDlc();
  void DrawSaves();
  void DrawAbout();
  void DrawFooter(float column_width);

  void RefreshGame();
  void RefreshDlc();
  void RefreshSaves();
  void StartInstall();
  void PollInstall();

  Ng2Settings* settings_;
  std::function<void(bool)> on_done_;
  std::function<void()> on_advanced_;
  bool finished_ = false;

  // Cached inspection of the configured game folder, refreshed only when the
  // path changes - it walks the folder, and this runs every frame.
  std::string game_path_inspected_;
  DiscInfo game_info_;

  std::string saved_note_;

  std::string dlc_path_scanned_;
  int dlc_count_ = 0;

  // Save import. The package is only inspected here; the import itself needs a
  // ContentManager, which does not exist until after this screen is gone, so
  // the choice is queued and carried out on the way into the game.
  std::filesystem::path save_path_;
  std::string save_message_;
  int save_count_ = 0;

  // Installer state.
  std::filesystem::path iso_path_;
  DiscInfo iso_info_;
  std::filesystem::path install_dest_;
  ExtractProgress progress_;
  std::thread install_thread_;
  bool install_started_ = false;
  // The destination is adopted once per install, not once per frame - it also
  // writes the settings file.
  bool install_adopted_ = false;
};

// A texture dump/upscale run. Its lifetime is the application's, not the
// settings menu's: the worker owns a child process tree and writes into the
// pack folder, so closing the menu must neither cancel the run nor orphan a
// python.exe. The App owns one of these; the SettingsOverlay only borrows a
// pointer - it starts, shows and cancels the run, but never ends it merely by
// being closed. Only an explicit Cancel or the app exiting stops it.
struct TextureJob {
  ExtractProgress progress;
  std::thread thread;
  TextureTools tools;
  bool tools_probed = false;

  // Ask the run to stop and wait for it. The runner polls progress.cancel and
  // kills the whole child process tree within a moment, so this returns
  // promptly. Safe when nothing is running, and safe to call more than once.
  void CancelAndJoin() {
    if (thread.joinable()) {
      progress.cancel = true;
      thread.join();
    }
  }
  ~TextureJob() { CancelAndJoin(); }
};

class SettingsOverlay final : public rex::ui::ImGuiDialog {
 public:
  SettingsOverlay(rex::ui::ImGuiDrawer* drawer, Ng2Settings* settings,
                  rex::ui::Window* window, std::function<void()> on_advanced,
                  TextureJob* tex_job);
  ~SettingsOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool DrawTextures();

  Ng2Settings* settings_;
  rex::ui::Window* window_;
  std::function<void()> on_advanced_;
  std::string status_;

  // Texture pack. The run itself (thread, progress, tools) is owned by the App
  // and only borrowed here through tex_job_, so closing this overlay does not
  // stop it - see TextureJob. Never null while the overlay exists.
  TextureJob* tex_job_ = nullptr;
  double tex_started_at_ = 0.0;   // for the time estimate, restarted per step
  int tex_phase_seen_ = 0;        // the step the clock was last restarted for
  // Process everything again rather than only what is missing. Not a saved
  // setting: it is a decision about one run, taken after changing the scale,
  // the upscaler or its strength, and must not quietly apply to the next.
  bool tex_redo_all_ = false;

};

}  // namespace ng2
