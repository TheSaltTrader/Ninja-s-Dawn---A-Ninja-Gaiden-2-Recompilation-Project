// Self-update: check GitHub for a newer release on launch and, if the player
// agrees, download it and swap it in.
//
// The whole feature is app-side and needs no plugin change. The pieces:
//   - a background WinHTTP call to the GitHub "latest release" API, comparing
//     its tag against NG2_VERSION baked in by CMake;
//   - a WinHTTP download of the release zip into <install>/update/;
//   - a small PowerShell updater staged beside it that waits for this process
//     to exit (a running exe cannot overwrite its own image), copies the new
//     program files over the install WITHOUT touching game/, dlc/, user/ or the
//     settings, and relaunches ng2.exe.
//
// Two surfaces show it: a launch-time prompt (UpdateOverlay) that appears only
// when there is genuinely a newer version, and a section in the F10 settings
// menu with the on/off toggle and a "Check now" button.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

namespace ng2 {
namespace update {

enum class Phase {
  kIdle,          // nothing has happened yet this run
  kChecking,      // the API call is in flight
  kUpToDate,      // checked, and this build is the latest
  kAvailable,     // a newer version exists and can be installed
  kDownloading,   // fetching the release zip
  kReadyToApply,  // zip is staged, the updater is written, ready to restart
  kApplying,      // the updater has been launched; we are about to exit
  kError,         // the last action failed; see `error`
};

// A thread-safe copy of the state for the UI to render without holding a lock.
struct Snapshot {
  Phase phase = Phase::kIdle;
  std::string current_version;   // this build, from NG2_VERSION
  std::string latest_version;    // the tag on GitHub, 'v' stripped
  std::string notes;             // the release body, for "What's new"
  std::string error;             // populated only in kError
  double progress = 0.0;         // 0..1 while downloading
  std::uint64_t bytes_done = 0;
  std::uint64_t bytes_total = 0;
  bool dismissed = false;        // the launch prompt was closed this run
  bool user_started = false;     // the player asked for the update (vs a check)
};

// One-time wiring from the app: our version, where we are installed, and the
// exe to relaunch. Safe to call more than once.
void Init(const std::string& current_version,
          const std::filesystem::path& install_dir,
          const std::filesystem::path& exe_path);

// What to do once the update is staged and the player has confirmed: shut down
// cleanly and exit so the staged updater can replace the files and relaunch.
// The handler is expected not to return (it exits the process).
void SetApplyHandler(std::function<void()> handler);

// Start a background check against the GitHub releases API. Cheap, networked,
// never blocks the caller. The caller decides whether to call it - that is what
// the "check on launch" setting gates.
void CheckAsync();

// The player asked to install it: download the zip, stage the updater, and mark
// ready to apply. Marks user_started so the launch prompt will carry it through
// to the restart on its own.
void BeginUpdate();

// Run the staged updater and hand off to the apply handler (which exits). Safe
// to call more than once; only the first call does anything.
void ApplyNow();

// Close the launch prompt for the rest of this run (the "Later" button).
void Dismiss();

// A consistent snapshot of the current state.
Snapshot Get();

// The launch-time prompt: a small centred window that draws only when an update
// is available (or is being downloaded / installed / has failed after the
// player acted), and nothing at all otherwise.
class UpdateOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit UpdateOverlay(rex::ui::ImGuiDrawer* drawer);
  ~UpdateOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace update
}  // namespace ng2
