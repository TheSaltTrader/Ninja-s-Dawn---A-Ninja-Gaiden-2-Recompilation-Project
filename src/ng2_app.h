// ng2 - ReXGlue Recompiled Project

#pragma once

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/cvar.h>
#include <rex/ui/flags.h>

#include "ng2_disc.h"
#include "ng2_diagnostics.h"
#include "ng2_menu.h"
#include "ng2_platform.h"
#include "ng2_saveimport.h"
#include "ng2_hwdetect.h"
#include "ng2_autoskip.h"
#include "ng2_perf.h"
#include "ng2_texnotify.h"
#include "ng2_settings.h"
#include "ng2_tuning.h"
#include "ng2_update.h"
#include <rex/system/flags.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xmemory.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <system_error>
#include <thread>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

// Defined in patch_hooks.cpp. Declared here so the settings menu can drive
// the internal render size patch through the same cvars the hooks read.
REXCVAR_DECLARE(int32_t, ng2_render_width);
REXCVAR_DECLARE(int32_t, ng2_render_height);
REXCVAR_DECLARE(bool, ng2_chapter12_workaround);
REXCVAR_DECLARE(int32_t, ng2_video_mode);
REXCVAR_DECLARE(bool, ng2_setup);

// Defined in patch_hooks.cpp: guest address of a path that cannot resolve,
// filled in once the runtime exists.
extern uint32_t g_ng2_skip_path_va;

class Ng2App : public rex::ReXApp {
 public:
  Ng2Settings settings_;

  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // Select the Xenos GPU emulation plugin. Without this the runtime comes up
  // in "native rendering mode" and every Vd* kernel call is ignored, so the
  // guest never gets a ring buffer. CMakeLists stages rexgpu-xenos.dll next to
  // the executable via GPU_PLUGINS.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin = "xenos";

    ApplyDisplaySettings();
    ApplyTuning();

    // Treat the title as the full, licensed version so purchased DLC is
    // visible. 0 = nothing licensed, 1 = first license (the full game), -1 =
    // everything. Only applied when the value is still the default, so
    // --license_mask on the command line continues to win.
    if (REXCVAR_GET(license_mask) == 0)
      REXCVAR_SET(license_mask, 1u);
  }

  // Default the game data root to whatever the settings menu last chose, or
  // game/ beside the executable. --game_data_root still wins when given: an
  // explicit path on the command line also suppresses the setup screen, so
  // scripted runs never stop at a dialog.
  //
  // This is also where the settings file is read. It has to happen before
  // OnFinalizePaths (which needs the game path) and before OnPreSetup (which
  // needs everything else), and both run later.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    settings_.Load();
    ApplyEnvironmentOverrides();

    // First run on this machine: size the memory settings from the hardware
    // rather than shipping one number and hoping. Here, not later, because both
    // settings are read during startup - the cache limit reaches the GPU plugin
    // through the tuning file, which is written before the plugin loads.
    if (!settings_.hardware_detected) {
      const ng2::GpuInfo gpu = ng2::DetectGpu();
      const ng2::DetectedSettings rec = ng2::RecommendSettings(gpu);
      if (gpu.valid) {
        settings_.texture_cache_mb = rec.texture_cache_mb;
        settings_.texture_scale = rec.texture_scale;
        settings_.hardware_detected = true;
        settings_.Save();
        // Held, not logged: this hook runs BEFORE logging is initialised, so a
        // line written here goes nowhere - which is how a detection that picked
        // the wrong answer looked like a detection that never ran.
        detect_report_ = rec;
        detect_gpu_ = gpu;
      } else {
        // Do not mark it done: a machine where DXGI failed should be asked
        // again next launch rather than left on defaults forever.
        REXLOG_WARN("Hardware detect: no adapter found, keeping the defaults");
      }
    }

    settings_.Clamp();
    ApplyWindowSettings();
    if (paths.game_data_root.empty()) {
      paths.game_data_root = settings_.ResolvedGamePath();
    }
    UsePortableUserData(paths);
  }

  // The profile, its saves and the installed DLC, beside the executable.
  //
  // The runtime's own default is under Documents, which means a copy of the
  // game moved to another drive arrives with no saves and there is nothing
  // next to the executable worth backing up. `user\` beside ng2.exe makes the
  // whole install one folder.
  //
  // Whatever was already at the old location is MOVED IN the first time rather
  // than orphaned - which matters, because that is where any save imported so
  // far is. The old path is never hard-coded: it is read from what the runtime
  // had already filled in before this hook ran, so this survives the SDK
  // changing its mind about where the default should be.
  void UsePortableUserData(rex::PathConfig& paths) {
    const auto local = rex::filesystem::GetExecutableFolder() / "user";
    const auto previous = paths.user_data_root;
    if (previous == local)
      return;

    std::error_code ec;
    const bool have_local = std::filesystem::exists(local, ec);
    if (!have_local && !previous.empty() &&
        std::filesystem::is_directory(previous, ec)) {
      std::filesystem::create_directories(local, ec);
      std::filesystem::copy(previous, local,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        REXLOG_WARN("User data: could not bring {} across to {} ({})",
                    previous.string(), local.string(), ec.message());
      } else {
        REXLOG_INFO("User data: brought {} across to {}", previous.string(),
                    local.string());
      }
    }
    std::filesystem::create_directories(local, ec);
    paths.user_data_root = local;
    REXLOG_INFO("User data: {}", local.string());
  }

  // The setup screen, on the one hook where the window and ImGui are live but
  // the runtime has not been built yet - so the game path it returns is the
  // one that actually gets mounted.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    const bool path_from_cli = !REXCVAR_GET(game_data_root).empty();

    // Remember a game folder given on the command line, so a later launch with
    // NO arguments finds it instead of opening the setup screen. That argless
    // launch is exactly what the self-updater's relaunch is: without this, an
    // install whose game folder lived only on --game_data_root (a shortcut or a
    // script) would come back from an update on the setup screen and look like
    // it had lost its settings - the fault Fable II hit and fixed the same way.
    // Portable installs are unaffected: the setup screen already persists
    // game_path and keeps game/ beside the executable.
    if (path_from_cli) {
      const std::string cli_root = REXCVAR_GET(game_data_root);
      if (ng2::InspectFolder(std::filesystem::path(cli_root)).Usable() &&
          (!settings_.configured || settings_.game_path != cli_root)) {
        settings_.game_path = cli_root;
        settings_.configured = true;
        settings_.Save();
        REXLOG_INFO("Settings: remembered the command-line game folder {}",
                    cli_root);
      }
    }

    const bool game_ok = ng2::InspectFolder(settings_.ResolvedGamePath()).Usable();

    // Four reasons to stop: never configured, the player asked for it with
    // Shift, --ng2_setup (which is how the game's own "Quit Game" gets back
    // here), or the configured folder is not there any more (a moved or
    // unplugged drive should offer the picker, not a fatal error).
    const bool show = !path_from_cli &&
                      (!settings_.configured || ng2::ShiftHeld() ||
                       REXCVAR_GET(ng2_setup) || !game_ok);
    if (!show) {
      rex::PathConfig paths = defaults;
      if (!path_from_cli)
        paths.game_data_root = settings_.ResolvedGamePath();
      return paths;
    }

    REXLOG_INFO("Setup screen: {}",
                !settings_.configured    ? "first run"
                : ng2::ShiftHeld()       ? "Shift held at launch"
                : REXCVAR_GET(ng2_setup) ? "returned from the game"
                                         : "configured game folder is missing");
    StartUiPump();
    setup_screen_ = std::make_unique<ng2::SetupScreen>(
        imgui_drawer(), &settings_,
        [this, defaults, resume](bool play) {
          // Invoked from inside the dialog's own OnDraw: destroying it here
          // would delete the object being drawn, so hand the teardown to the
          // next UI tick.
          app_context().CallInUIThreadDeferred([this, defaults, resume, play] {
            StopUiPump();
            setup_screen_.reset();
            if (!play) {
              app_context().QuitFromUIThread();
              return;
            }
            // The window already exists by now, so a fullscreen choice made
            // on this screen cannot go through the cvar the window was built
            // from. Push it - and the presenter's settings - straight at the
            // live objects instead, which is the same path the in-game overlay
            // uses and is known to take effect immediately.
            ng2::ApplyLiveSettings(settings_, window());
            rex::PathConfig paths = defaults;
            paths.game_data_root = settings_.ResolvedGamePath();
            resume(paths);
          });
        },
        [this] { ToggleAdvancedSettings(); });
    return std::nullopt;
  }

  // F10 opens the same settings over the running game. F4 (the SDK's own raw
  // cvar browser) is left alone; this menu links to it rather than replacing
  // it, because the two answer different questions.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Prepared videos sit beside the executable. The overlay lives for the
    // whole run and draws nothing until the guest opens a video: once the game
    // is running there is no other per-frame hook of ours for a request from
    // the guest thread to be noticed by.
    // From the settings for now; OnPostSetup sets it again from the path the
    // runtime actually mounted, which is what --game_data_root changes and
    // what the setup screen may still change after this point.
    // The window title is the SDK's by default - "ng2 [rexglue-v0.10.0]" -
    // which names the SDK's version and never changes. Put the port's own
    // version where it is visible without opening anything.
    if (auto* w = window()) {
      w->SetTitle(std::string("Ninja Gaiden II  -  v") + NG2_VERSION);
    }

    // Lives for the whole run and draws nothing until the texture pack is
    // switched: there is no other per-frame hook of ours once the guest is
    // running.
    tex_notify_ = std::make_unique<ng2::TextureNotifyOverlay>(drawer);
    warm_overlay_ = std::make_unique<ng2::WarmOverlay>(drawer);

    // The launch-time update prompt. Draws nothing until a check finds a newer
    // version; the check itself is started in OnPostSetup when the setting is on.
    update_overlay_ = std::make_unique<ng2::update::UpdateOverlay>(drawer);

    // The readouts, and the sampler behind them. Started unconditionally: it
    // ticks twice a second and the menu's own CPU/GPU bars need it even when
    // nothing is shown on screen.
    ng2::SetHudSettings(&settings_);
    ng2::StartPerfMonitor();
    perf_hud_ = std::make_unique<ng2::PerfHudOverlay>(drawer);

    // Escape quits. The window's close button already does, but a full-screen
    // game with no visible chrome needs a key that gets you out.
    // [diag] F7 while the character is bobbing: find the oscillating float.
    rex::ui::RegisterBind("bind_ng2_oscscan", "F7",
                          "Scan for oscillating guest values", [] {
                            REXLOG_INFO("[osc] scan requested");
                            ng2::ScanOscillating();
                          });

    // F8 shows or hides the readouts, for the same reason F9 exists: a number
    // that answers "what is this setting costing me" is worth nothing if
    // reaching it means covering the screen with a menu. Which of the three are
    // shown stays as chosen; this only switches them on and off.
    rex::ui::RegisterBind(
        "bind_ng2_hud", "F8", "Show or hide the on-screen readouts", [this] {
          settings_.hud_enabled = !settings_.hud_enabled;
          REXLOG_INFO("F8: on-screen readouts {}",
                      settings_.hud_enabled ? "on" : "off");
          settings_.Save();
        });

    // F9 flips the texture pack on and off WITHOUT opening a menu.
    //
    // A texture pack is judged by comparing it against the original, and doing
    // that through a settings screen is nearly useless: by the time the overlay
    // is closed the eye has lost the detail it was comparing. On the same key,
    // in the same scene, the difference is obvious or it is not there.
    //
    // The plugin drops every texture when the path changes, so the swap lands
    // within a frame or two. It costs a rebuild of the resident set each way,
    // which is a brief hitch - unavoidable, and the point of the exercise.
    rex::ui::RegisterBind(
        "bind_ng2_texpack", "F9", "Toggle the upscaled texture pack", [this] {
          if (settings_.texture_path.empty()) {
            REXLOG_WARN("F9: no texture folder is set - nothing to switch to");
            return;
          }
          // One switch at a time. A path change makes the plugin clear its
          // pack tables at once and drop the texture cache at the end of the
          // frame, after draining the GPU; a second press landing inside that
          // window flips the path back while the first clear is still pending.
          // The Fable II port, which shares this plugin, crashed on exactly
          // that: F9 twice within a second, and the process died with no
          // further log. Nobody compares textures at that rate, so a press
          // within 1.5 s of the previous one is dropped, and says so.
          const auto now = std::chrono::steady_clock::now();
          static auto last_switch = now - std::chrono::seconds(10);
          if (now - last_switch < std::chrono::milliseconds(1500)) {
            REXLOG_INFO("F9 ignored: the pack is still switching");
            return;
          }
          last_switch = now;
          settings_.texture_pack = !settings_.texture_pack;
          REXLOG_INFO("F9: texture pack {}", settings_.texture_pack ? "ON" : "OFF");
          ng2::ApplyLiveSettings(settings_, window());
          ng2::NotifyTexturePack(settings_.texture_pack);
          // Deliberately NOT saved. This key is for comparing the pack against
          // the original, and a comparison should not quietly become the stored
          // preference - leaving it mid-comparison once meant the next launch
          // started with the pack off while the menu still said it was on. The
          // checkbox is where the preference is set; this is a look.
        });

    rex::ui::RegisterBind("bind_ng2_quit", "Escape", "Quit the game",
                          [this] { QuitFromEscape(); });

    rex::ui::RegisterBind(
        "bind_ng2_settings", "F10", "Toggle the Ninja Gaiden II settings menu",
        [this, drawer] {
          if (overlay_) {
            overlay_.reset();
            return;
          }
          overlay_ = std::make_unique<ng2::SettingsOverlay>(
              drawer, &settings_, window(), [this] { ToggleAdvancedSettings(); },
              &tex_job_);
        });
  }

  // The menu's own font and theme. OnConfigureFonts runs while the ImGui atlas
  // is still open; OnConfigureStyle right after the SDK has applied its own
  // defaults, so this is a replacement rather than a patch.
  void OnConfigureFonts(ImFontAtlas* atlas) override { ng2::LoadMenuFonts(atlas); }

  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    ng2::ApplyMenuStyle(imgui_style);
    (void)ui_style;
  }

  // Everything this port started, stopped in the order it has to be stopped in.
  //
  // Escape and the window's close button both arrive here. The cheat worker
  // goes FIRST and is joined, not just asked to stop: it writes guest memory
  // sixty times a second, and the runtime's memory is torn down shortly after
  // this hook returns - a thread still running through that is a fault on the
  // way out, which is exactly the kind of exit that leaves a process behind.
  //
  // Settings are written before the dialogs go, so a change made in the overlay
  // and never explicitly saved is not lost by quitting.
  // Everything this port started, stopped before the process goes.
  //
  // This is NOT in OnShutdown, and that is the point: Escape and the close
  // button both end up inside the SDK's "Title terminated; hard-exiting
  // process", and OnShutdown does not run on that path - measured, by looking
  // for the line it logs and not finding it. Anything that has to happen before
  // the process dies has to happen here.
  //
  // The cheat worker goes first and is JOINED rather than merely asked to
  // stop: it writes guest memory sixty times a second and guest memory goes
  // away with the runtime. Settings are written next, so a change made in the
  // overlay and never explicitly saved survives quitting.
  void ShutdownCleanly() {
    if (shutting_down_)
      return;
    shutting_down_ = true;
    StopTitleWatcher();
    ng2::StopPerfMonitor();
    settings_.Clamp();
    settings_.Save();
    REXLOG_INFO("Shutdown: cheat worker joined, settings saved");
    StartExitWatchdog();
  }

  // Escape's quit: release what we own, then take the close button's path.
  //
  // It used to ask the runtime to quit gracefully (QuitFromUIThread), and
  // measured, that never comes back: our own teardown ran to completion and
  // logged it, and the process sat there until the watchdog below killed it
  // three seconds later, window black the whole time. The guest's threads are
  // fibers and the graceful path waits on something that never finishes. The
  // close button never showed it, because the SDK's close path terminates the
  // title and hard-exits ("Title terminated; hard-exiting process."). So
  // Escape now asks the window to close, which is exactly that path, with the
  // watchdog kept behind it in case the request is ever swallowed. Fable 2
  // made the same change first and measured it: 0.4 s where the watchdog
  // path took 3.3 s.
  //
  // Reachable from a test seam as well as the key: NG2_QUIT_AFTER=<seconds>
  // fires this from a timer, so whether the quit really exits, and how fast,
  // is something a script measures rather than something anyone believes.
  void QuitFromEscape() {
    REXLOG_INFO("Escape: quitting");
    ShutdownCleanly();
    tex_job_.CancelAndJoin();
    if (auto* w = window()) {
      w->RequestClose();
    } else {
      app_context().QuitFromUIThread();
    }
  }

  void ArmQuitSeam() {
    const int after = EnvInt("NG2_QUIT_AFTER", 0);
    if (after <= 0)
      return;
    REXLOG_INFO("Quit seam: Escape's path fires in {} s", after);
    std::thread([this, after] {
      std::this_thread::sleep_for(std::chrono::seconds(after));
      app_context().CallInUIThreadDeferred([this] { QuitFromEscape(); });
    }).detach();
  }

  // Make sure the process actually goes. Our own teardown is done by the time
  // this starts - the settings are written, our threads are joined - so what
  // remains is the runtime's, and it is not entitled to hang. On the close
  // request path this never fires; it is the backstop.
  void StartExitWatchdog() {
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      REXLOG_WARN("Shutdown: the runtime did not finish in 3 s; exiting now");
      std::fflush(nullptr);
      rex::FlushLogging();
      std::_Exit(0);
    }).detach();
  }

  // The game's own "Quit Game" leaves a black screen, and this is why.
  //
  // That menu entry calls XamLoaderTerminateTitle, which reaches
  // KernelState::TerminateTitle. That marks every guest thread, wakes the ones
  // blocked in a kernel wait, gives them 200ms to exit, and then DELIBERATELY
  // LEAVES THE STRAGGLERS RUNNING - force-killing a thread orphans whatever
  // host lock it holds.
  //
  // NG2's tasks are fibers, so its main guest thread spends essentially all of
  // its time inside SwitchToFiber rather than in a kernel wait. It never
  // reaches XThread::CheckTitleTermination, so it is always a straggler. The
  // SDK quits the app from a thread that Wait()s on exactly that thread
  // (ReXApp::LaunchModule), so the wait never returns, the app never quits,
  // and the window sits there with nothing left to draw into it.
  //
  // That also means OnGuestThreadExit could never have helped: it is called
  // from that same wait, for the main thread only, and the main thread is the
  // one that does not exit.
  //
  // What IS reliable is that TerminateTitle erases every guest thread from the
  // kernel's map whether or not it actually stopped. So the main thread's id
  // going missing is a stable, permanent signal that the title has terminated
  // - not a race against the 200ms drain, and not dependent on the
  // terminating_title_ flag, which TerminateTitle clears again before the
  // calling thread exits.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    if (thread)
      main_guest_thread_id_ = thread->thread_id();
    StartTitleWatcher();
  }

  void StartTitleWatcher() {
    if (main_guest_thread_id_ == 0)
      return;
    title_watcher_ = std::thread([this] {
      // The thread has to be SEEN in the kernel's map before its absence means
      // anything. This hook runs before main_thread->Resume(), and treating
      // "not there yet" as "the title ended" would relaunch the app in a loop
      // that only a task manager could stop - the single worst way this could
      // fail, so it is not left to timing.
      bool seen = false;
      while (!title_watcher_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto* kernel = REX_KERNEL_STATE();
        if (!kernel)
          continue;
        if (kernel->GetThreadByID(main_guest_thread_id_)) {
          seen = true;
          continue;
        }
        if (!seen)
          continue;

        // Closing the window ALSO terminates the title, and this must not turn
        // that into a relaunch. ReXApp::OnClosing calls TerminateTitle and then
        // _Exit(0) a few milliseconds later, so a poll landing inside that gap
        // would see exactly what a guest quit looks like - about a 2% chance per
        // close at this interval, which is far too likely for "closing the app
        // reopened it".
        //
        // Settling for longer than that gap is what tells them apart: on any
        // host quit path the process is gone before this returns, and on a
        // guest quit nothing else is happening and the state is permanent.
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        if (title_watcher_stop_.load(std::memory_order_acquire) || shutting_down_)
          return;
        REXLOG_INFO("The guest terminated the title - returning to the menu");
        ReturnToMenu();
        return;
      }
    });
  }

  // Detaches rather than joins. The watcher can be inside its settle sleep
  // while a host quit runs, and joining would block the quit for up to a
  // second; the stop flag is checked after that sleep, so a detached watcher
  // that wakes into a shutdown does nothing and exits.
  void StopTitleWatcher() {
    title_watcher_stop_.store(true, std::memory_order_release);
    if (title_watcher_.joinable())
      title_watcher_.detach();
  }

  // Quitting the game puts the player back on this port's own setup screen.
  //
  // It gets there by relaunching rather than by tearing the runtime down and
  // rebuilding it: the guest's stragglers are still running inside a runtime
  // whose memory would be going away underneath them, which is the same fault
  // the window-close path avoids by hard-exiting. A fresh process has none of
  // that, and it is fast enough that it reads as a screen change.
  //
  // --ng2_setup is what makes the new process stop at the setup screen instead
  // of booting straight back into the game, which is what it would otherwise
  // do with a game folder already configured.
  void ReturnToMenu() {
    if (shutting_down_)
      return;
    ShutdownCleanlyForRelaunch();
    const std::filesystem::path exe =
        rex::filesystem::GetExecutableFolder() / "ng2.exe";
    std::error_code ec;
    if (std::filesystem::exists(exe, ec)) {
      ng2::LaunchDetached(exe, "--ng2_setup");
    } else {
      REXLOG_WARN("Could not find {} to relaunch; quitting instead", exe.string());
    }
    std::fflush(nullptr);
    rex::FlushLogging();
    std::_Exit(0);
  }

  // ShutdownCleanly without the exit watchdog: the caller exits by itself and
  // a 3s _Exit racing a relaunch would be a second process starting while this
  // one is still holding the window.
  void ShutdownCleanlyForRelaunch() {
    if (shutting_down_)
      return;
    shutting_down_ = true;
    settings_.Clamp();
    settings_.Save();
    REXLOG_INFO("Shutdown for relaunch: cheat worker joined, settings saved");
  }

  // The apply handler for the self-update. By the time this runs the staged
  // PowerShell updater is already launched and waiting on this process's id: a
  // running exe holds its own image locked, so the swap can only happen once we
  // are gone. This exits the same way ReturnToMenu does - clean save, no
  // relaunch of our own (the updater relaunches us after it copies the files).
  void ExitForUpdate() {
    if (shutting_down_)
      return;
    StopTitleWatcher();
    ShutdownCleanlyForRelaunch();
    tex_job_.CancelAndJoin();
    REXLOG_INFO("Update: exiting so the installer can replace the files");
    std::fflush(nullptr);
    rex::FlushLogging();
    std::_Exit(0);
  }

  // Called when the window's close button is pressed, before the SDK's own
  // close handling terminates the title. Marks the watcher off so the
  // termination that is about to happen is not mistaken for a guest quit.
  bool OnWindowCloseRequested() override {
    title_watcher_stop_.store(true, std::memory_order_release);
    return true;
  }

  void OnShutdown() override {
    ShutdownCleanly();

    // Dialogs hold a raw pointer to the drawer, which the SDK tears down after
    // this hook. Drop them next. SetupScreen's destructor joins the install
    // thread, so quitting mid-install waits for the child process rather than
    // orphaning it.
    StopUiPump();
    setup_screen_.reset();
    overlay_.reset();
    advanced_.reset();
    tex_notify_.reset();
    warm_overlay_.reset();
    update_overlay_.reset();
    perf_hud_.reset();

    // Every bind this port registered, not just the two it started with - a
    // bind left behind holds a lambda capturing `this`.
    for (const char* bind :
         {"bind_ng2_settings", "bind_ng2_quit"}) {
      rex::ui::UnregisterBind(bind);
    }
    REXLOG_INFO("Shutdown: cheats stopped, settings saved, dialogs released");
  }

  // The SDK's cvar browser, which is the honest "everything else" surface:
  // it enumerates the registry rather than a hand-written list, so it cannot
  // fall behind the build.
  void ToggleAdvancedSettings() {
    if (advanced_) {
      advanced_.reset();
      return;
    }
    advanced_ = std::make_unique<rex::ui::SettingsDialog>(
        imgui_drawer(), rex::filesystem::GetExecutableFolder() / "ng2.toml");
  }

  // PC display settings.
  //
  // The host window and the guest video mode are separate: window_width/height
  // size the desktop window, while video_mode_* is what the title is told the
  // display is, which is what actually changes the rendered resolution and the
  // rate it targets. Passing these on the command line does nothing - the
  // launcher's parser does not reach these cvars - so they are set here, and
  // read from the environment so they stay configurable without a rebuild.
  //
  //   NG2_WIDTH / NG2_HEIGHT        window size (default 1280x720)
  //   NG2_FULLSCREEN=1              borderless fullscreen
  //   NG2_FPS                       guest refresh rate, 30-144 (default 60)
  //   NG2_RENDER_WIDTH / _HEIGHT    guest video mode, defaults to window size
  static int EnvInt(const char* name, int fallback) {
    const char* e = std::getenv(name);
    if (!e || !*e) return fallback;
    const int v = std::atoi(e);
    return v > 0 ? v : fallback;
  }

  // Environment overrides win over the saved file, for scripted testing. Read
  // once, in OnConfigurePaths, so every later consumer sees the same values.
  void ApplyEnvironmentOverrides() {
    settings_.window_width = EnvInt("NG2_WIDTH", settings_.window_width);
    settings_.window_height = EnvInt("NG2_HEIGHT", settings_.window_height);
    settings_.fps = EnvInt("NG2_FPS", settings_.fps);
    settings_.resolution_scale = EnvInt("NG2_SCALE", settings_.resolution_scale);
    if (std::getenv("NG2_FULLSCREEN"))
      settings_.fullscreen = EnvInt("NG2_FULLSCREEN", 0) != 0;
    if (const char* game = std::getenv("NG2_GAME"); game && *game)
      settings_.game_path = game;
    if (const char* dlc = std::getenv("NG2_DLC"); dlc && *dlc)
      settings_.dlc_path = dlc;
    // A scripted run must never stop at the setup screen.
    if (std::getenv("NG2_NO_SETUP"))
      settings_.configured = true;
    // The same import the setup screen queues, without the file dialog. It is
    // here because a save import is otherwise only reachable by hand, which
    // makes it untestable and unscriptable; it takes the identical path from
    // this point on.
    // Accepts a single save or a folder of them, the same as the picker does.
    if (const char* save = std::getenv("NG2_IMPORT_SAVE"); save && *save)
      ng2::QueueSaveImports(ng2::FindSavePackages(save));
  }

  // The three settings the window itself is built from. They have to be in
  // place before SetupPresentation creates it, which is BEFORE OnPreSetup - so
  // this runs from OnConfigurePaths, inside SetupEnvironment.
  //
  // Setting them in OnPreSetup looked like it worked, because the window ends
  // up the right size anyway once the guest sets its video mode. Fullscreen
  // has no such second chance: saved as on, it came back windowed every time,
  // and the only reason that was caught was photographing the window and
  // finding the capture still 1602x939 instead of the full screen.
  // A window bigger than the monitor is a window with its bottom edge off the
  // screen, and the footer of the setup screen lives on that edge. Picking 4K
  // on a 1440p display did exactly that: it read as "the menu went full screen
  // and some options are gone", when the options were simply below the desk.
  //
  // Only the WINDOW is clamped. The internal render scale is a different
  // setting and is what supersampling uses, so asking for 3x on a 1440p screen
  // still renders at 3x - the picture just arrives somewhere it can be seen.
  // The window the player asked for, in the units the runtime actually wants.
  //
  // window_width/height in the settings mean PHYSICAL pixels, because that is
  // what "4K" means to a person. The runtime's cvars mean LOGICAL pixels, which
  // Windows then multiplies by the display's scaling. On a 150% 4K screen those
  // differ by half again: filling it needs 2560 logical, and asking for 3840
  // produces a 5760-pixel window that cannot fit on any monitor here.
  //
  // That was the whole of "the app will not set up 4K". An earlier version of
  // this function made it worse rather than better - it clamped a logical
  // request against a physical work area, so a window that "fitted" was then
  // scaled up past the edge of the screen anyway.
  //
  // So: clamp in physical against the monitor this window will open on, then
  // convert once, on the way out.
  void ApplyWindowSizeForMonitor() {
    int screen_w = 0, screen_h = 0;
    float scale = 1.0f;
    if (!ng2::MonitorWorkArea(settings_.monitor, screen_w, screen_h, scale)) {
      // Could not tell; do not invent a limit or a scale.
      REXCVAR_SET(window_width, settings_.window_width);
      REXCVAR_SET(window_height, settings_.window_height);
      return;
    }
    // Clamp against the monitor it will open on, but convert with the scale
    // Windows will actually apply - which is the PRIMARY display's, not this
    // one's. Measured: with the 4K monitor's own 150% the window came out
    // 3218 pixels wide on a 3840 screen, because the conversion assumed a
    // scaling that was never applied.
    const float applied = ng2::SystemScale();
    const int physical_w = std::min(settings_.window_width, screen_w);
    const int physical_h = std::min(settings_.window_height, screen_h);
    const int logical_w = int(float(physical_w) / applied + 0.5f);
    const int logical_h = int(float(physical_h) / applied + 0.5f);

    REXLOG_INFO("Display: monitor {} is {}x{} at {:.0f}%; windows are scaled "
                "{:.0f}%, so {}x{} physical is {}x{} logical",
                settings_.monitor + 1, screen_w, screen_h, scale * 100.0f,
                applied * 100.0f, physical_w, physical_h, logical_w, logical_h);

    REXCVAR_SET(window_width, logical_w);
    REXCVAR_SET(window_height, logical_h);
  }

  void ApplyWindowSettings() {
    // The runtime has always had this and nothing drove it, so the window went
    // wherever Windows last put it - which on three screens is not necessarily
    // the one the size was chosen for.
    // The runtime's display numbers are ONE-based, with 0 meaning "wherever
    // Windows likes". Tested: 1 was the primary, 2 and 3 the other two, and 4
    // fell back to the primary. The setting is a 0-based index into the list
    // the menu shows, so it is shifted here rather than in the menu.
    rex::cvar::SetFlagByName("monitor",
                             std::to_string(settings_.monitor + 1));
    ApplyWindowSizeForMonitor();
    REXCVAR_SET(fullscreen, settings_.fullscreen);
  }

  void ApplyDisplaySettings() {
    // The guest is told a 16:9 display, whatever shape the window is.
    //
    // It used to be told the window size. Ninja Gaiden II renders 16:9 (the
    // internal render size below) and the presenter pillarboxes only when the
    // guest's display aspect differs from the window's - so a 3840x1600 window
    // made the guest report a 2.4:1 display, the 16:9 frame "matched" it, and
    // Keep aspect ratio never got a say: the whole game stretched. It went
    // unseen for as long as every window was 1280x720.
    //
    // The largest 16:9 box that fits the window: a 16:9 window is told its own
    // size as before, an ultrawide is told a display of its own height.
    int guest_w = settings_.window_width;
    int guest_h = settings_.window_height;
    if (settings_.ultrawide) {
      // True ultrawide: tell the game the monitor's real aspect so its 3D field
      // of view widens to match (verified). Scaled to ~1080 tall so the video
      // mode stays a sane size; the render surface is left native, which is
      // what keeps the EDRAM resolves valid.
      int mw = 0, mh = 0;
      float sc = 1.0f;
      if (ng2::MonitorWorkArea(settings_.monitor, mw, mh, sc) && mw > 0 && mh > 0) {
        guest_h = 1080;
        guest_w = 1080 * mw / mh;
      }
    } else if (guest_w * 9 > guest_h * 16) {
      guest_w = guest_h * 16 / 9;
    } else if (guest_w * 9 < guest_h * 16) {
      guest_h = guest_w * 9 / 16;
    }
    guest_w &= ~1;
    guest_h &= ~1;
    REXCVAR_SET(video_mode_width, guest_w);
    REXCVAR_SET(video_mode_height, guest_h);
    // The runtime treats a video mode equal to its default (1280x720) as unset
    // and substitutes the window size - the stretch again for any window that
    // is not itself 1280x720, and on a 125% display even that one is told
    // 1024x576, which is not high definition. This asks it to take the value
    // as set. By name, so a runtime without the option simply ignores it.
    if (rex::cvar::GetFlagInfo("video_mode_explicit"))
      rex::cvar::SetFlagByName("video_mode_explicit", "true");

    // Ultrawide / FOV experiment (env only): force a non-16:9 guest video mode
    // so the game reports a wider display, to see whether its 3D field of view
    // follows the display aspect (native ultrawide) rather than staying 16:9.
    // Overrides the 16:9 clamp above and matches the internal render size to it.
    if (const char* fv = std::getenv("NG2_FORCE_VMODE")) {
      int fw = 0, fh = 0;
      if (std::sscanf(fv, "%dx%d", &fw, &fh) == 2 && fw > 1 && fh > 1) {
        // Only the reported display aspect - NOT the render surface, which is
        // 16:9-bound in this title (forcing it wide breaks EDRAM resolves). The
        // question this answers: does the 3D FOV follow the display aspect?
        REXCVAR_SET(video_mode_width, fw);
        REXCVAR_SET(video_mode_height, fh);
        REXLOG_INFO("Display: NG2_FORCE_VMODE {}x{} (aspect/FOV test)", fw, fh);
      }
    }

    // The title paces itself off the reported display refresh rate, so this is
    // the frame-rate control. Clamped to 30-144 rather than unlocked: the game
    // ties logic to the display rate and misbehaves outside that band.
    REXCVAR_SET(video_mode_refresh_rate, double(settings_.fps));

    // The internal render size patch. These are our own cvars, read by the
    // midasm hooks in patch_hooks.cpp; 0 means "leave the game's 1120x584
    // alone", which is why they are only written when the patch is on.
    if (settings_.internal_720p) {
      REXCVAR_SET(ng2_render_width, 1280);
      REXCVAR_SET(ng2_render_height, 720);
    }
    REXCVAR_SET(ng2_video_mode, settings_.video_mode);

    // vsync, the internal render scale and everything else the GPU plugin owns
    // are applied by ApplyTuning() through the config loader - see ng2_tuning.h
    // for why setting them from here cannot work.

    REXLOG_INFO(
        "Display: window {}x{}, guest display {}x{}, {}x internal scale, {} Hz, "
        "fullscreen={}, vsync={}",
        settings_.window_width, settings_.window_height,
        REXCVAR_GET(video_mode_width), REXCVAR_GET(video_mode_height),
        settings_.resolution_scale, settings_.fps, settings_.fullscreen,
        settings_.vsync);
  }

  // Install downloadable content.
  //
  // NG2's DLC ships as LIVE-signed STFS packages under
  //   DLC/<title id>/<content type>/<40-hex filename>
  // The SDK's ContentManager::InstallContent extracts a package into its own
  // content root and writes the .header file XAM enumerates, which is what
  // makes the extra characters and Mission Mode visible to the game's menus.
  //
  // Packages are matched by their STFS magic rather than by extension - the
  // files have no extension at all. Installing is idempotent, so re-running is
  // harmless; a package that fails is logged and skipped rather than aborting
  // the boot.
  static bool IsStfsPackage(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    if (std::filesystem::file_size(path, ec) < 0x1000) return false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char magic[4] = {};
    const size_t got = std::fread(magic, 1, sizeof(magic), f);
    std::fclose(f);
    if (got != sizeof(magic)) return false;
    return !std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4) ||
           !std::memcmp(magic, "CON ", 4);
  }

  void InstallDlc() {
    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) return;
    auto* content = kernel->content_manager();
    if (!content) return;

    // Whatever the settings menu chose first, then the two conventional
    // spellings beside the executable - the disc rip and its DLC usually live
    // together.
    std::vector<std::filesystem::path> roots{
        settings_.ResolvedDlcPath(),
        rex::filesystem::GetExecutableFolder() / "dlc",
        rex::filesystem::GetExecutableFolder() / "DLC",
    };
    std::error_code ec;
    for (const auto& root : roots) {
      if (!std::filesystem::is_directory(root, ec)) continue;
      std::vector<std::filesystem::path> packages;
      for (auto& e : std::filesystem::recursive_directory_iterator(root, ec))
        if (IsStfsPackage(e.path())) packages.push_back(e.path());
      std::sort(packages.begin(), packages.end());
      REXLOG_INFO("DLC: {} package(s) under {}", packages.size(),
                  root.string());
      for (const auto& pkg : packages) {
        const auto result = content->InstallContent(pkg);
        if (result == 0)
          REXLOG_INFO("DLC: installed {}", pkg.filename().string());
        else
          REXLOG_WARN("DLC: failed to install {} (0x{:08X})",
                      pkg.filename().string(), uint32_t(result));
      }
      return;  // first directory that exists wins
    }
    REXLOG_INFO("DLC: no dlc/ folder beside the executable; skipping");
  }

  // Every controller drives guest user 0.
  //
  // The runtime's default is "device ordinal N feeds guest user N", so a pad
  // that does not enumerate first lands on guest user 1 - and Ninja Gaiden II
  // is single-player: it polls user 0 and nothing else. That one policy
  // produced both of the symptoms that looked unrelated: a pad that moved
  // nothing in game, and a sign-in prompt that could not be satisfied, because
  // the profile the game was asking about belonged to a user index that does
  // not exist.
  //
  // SharedAssignment is the SDK's own answer for this case - "every device
  // feeds guest user 0 ... for single-player titles that only ever poll user
  // 0" - and it must be in place before the guest starts polling.
  void UseOneGuestUser() {
    auto* runtime_ptr = runtime();
    if (runtime_ptr == nullptr)
      return;
    // SetDeviceAssignment is on the concrete input system, not the two-method
    // interface the runtime hands back, so this asks rather than assumes: a
    // different backend simply keeps the default instead of crashing.
    auto* input =
        dynamic_cast<rex::input::InputSystem*>(runtime_ptr->input_system());
    if (input == nullptr) {
      REXLOG_WARN("Input: not the SDL input system; controllers stay on the "
                  "default per-slot assignment");
      return;
    }
    input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());

    // The synthetic pad that dismisses chapter-opening cinematics. Added after
    // the assignment so SharedAssignment picks it up with everything else; it
    // reports nothing at all unless a chapter load has armed it.
    ng2::SetAutoSkipEnabled(settings_.skip_cinematics);
    input->AddDriver(ng2::MakeAutoSkipDriver());
    REXLOG_INFO("Input: every controller drives guest user 0");
  }

  // Keystrokes only reach the game while one of our windows has focus.
  //
  // Checked by process rather than window handle - the SDK does not expose the
  // HWND, and "is the foreground window one of ours" answers the same question.
  //
  // This callback also used to hold input back while a replacement video was
  // drawn over the game. That mode is gone: the game plays its own videos now,
  // so it is the one taking the skip press, which is what should happen.
  void GateInputToForeground() {
    auto* runtime_ptr = runtime();
    auto* input = runtime_ptr ? dynamic_cast<rex::input::InputSystem*>(
                                    runtime_ptr->input_system())
                              : nullptr;
    if (input == nullptr)
      return;
    input->SetActiveCallback([] {
      // Nothing reaches the game while this stage's textures are still being
      // read. Pressing on through would land the player in the stage exactly
      // when the disk is busiest - which is the stutter the warming exists to
      // remove, arrived at by letting them skip the fix.
      //
      // Held at the INPUT layer rather than by drawing a modal, because the
      // screen underneath is the game's own loading screen and covering it
      // would replace something informative with something less so.
      if (ng2::GetWarmState().warming)
        return false;
      return ng2::ThisProcessIsForeground();
    });
    REXLOG_INFO("Input: held while the texture cache warms, and while we are "
                "not the foreground window");
  }

  // A save chosen on the setup screen.
  //
  // It cannot be imported there: that screen runs before the runtime is built,
  // so there is no ContentManager and no signed-in profile to import into. It
  // is queued instead and carried out here, which is after both exist and
  // still before the guest starts looking for saves.
  void ImportQueuedSaves() {
    const auto packages = ng2::TakeQueuedSaveImports();
    if (packages.empty()) return;
    size_t done = 0;
    for (const auto& package : packages) {
      std::string message;
      if (ng2::ImportSave(package, message)) {
        ++done;
        REXLOG_INFO("Save: imported {} - {}", package.filename().string(),
                    message);
      } else {
        // One bad file does not stop the rest: a folder of saves is exactly
        // where a stray or corrupt one turns up.
        REXLOG_WARN("Save: could not import {} - {}",
                    package.filename().string(), message);
      }
    }
    REXLOG_INFO("Save: {} of {} imported", done, packages.size());
  }

  // Commit the low guest pages as readable zeroes.
  //
  // The game keeps 160 slot records at 0x84C23C70 whose data pointers are null
  // until something populates them, and 275+ sites still index off them as
  // `buffer[idTable[type]]`. On hardware those reads land in low memory and
  // come back zero, which every consumer treats as "nothing here"; here the
  // pages are uncommitted, so the same reads fault at guest 0x59BF / 0x4F60.
  //
  // Page 0 is deliberately left out so a genuine null dereference still
  // faults, and the range is mapped read-only so a stray *write* through a
  // null pointer is still caught.
  // Everything the GPU plugin owns, plus the title-specific correctness
  // settings, applied through the cvar config loader so values survive until
  // the plugin registers its cvars. ng2_tuning.h documents each one.
  //
  // This replaces an earlier ApplyGpuSettings() that ran in OnPostSetup and
  // set draw_resolution_scale_x/y directly. That call reported success and did
  // nothing useful: the plugin had already read the scale at GPU init, and
  // writing the settings value back afterwards actively destroyed a scale that
  // had been set on the command line. It is why internal scaling was written
  // off as impossible.
  void ApplyTuning() {
    auto entries = Ng2Tuning::Fixed();
    const auto mappings =
        rex::filesystem::GetExecutableFolder() / "gamecontrollerdb.txt";
    std::error_code ec;
    const bool have_mappings = std::filesystem::exists(mappings, ec);
    for (auto& e : Ng2Tuning::FromSettings(
             settings_, have_mappings ? mappings.string() : std::string())) {
      entries.push_back(std::move(e));
    }
    Ng2Tuning::Apply(
        rex::filesystem::GetExecutableFolder() / "cache" / "ng2_tuning.toml",
        entries);
  }

  // NG2_DUMP_CVARS=<path> writes every registered cvar - name, category, type,
  // current value, default, and any declared allowed values - once the GPU
  // plugin is up and its own cvars exist.
  //
  // This is the only honest way to know what a build can actually do. The
  // plugin does not export accessors for its flags, so they cannot be listed
  // from outside; guessing from the SDK headers is how the settings menu ended
  // up offering FSR and CAS, which this presenter does not implement.
  static void DumpCvars(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Cvar dump: cannot write {}", path.string());
      return;
    }
    auto names = rex::cvar::ListFlags();
    std::sort(names.begin(), names.end());
    out << "# " << names.size() << " registered cvars\n";
    for (const auto& name : names) {
      const auto* info = rex::cvar::GetFlagInfo(name);
      if (info == nullptr)
        continue;
      out << name << "\n  category  " << info->category
          << "\n  value     " << rex::cvar::GetFlagByName(name)
          << "\n  default   " << info->default_value << "\n";
      if (!info->constraints.allowed_values.empty()) {
        out << "  allowed  ";
        for (const auto& v : info->constraints.allowed_values)
          out << " " << v;
        out << "\n";
      }
      if (info->constraints.HasRangeConstraint()) {
        out << "  range     "
            << (info->constraints.min ? std::to_string(*info->constraints.min) : "-")
            << " .. "
            << (info->constraints.max ? std::to_string(*info->constraints.max) : "-")
            << "\n";
      }
    }
    REXLOG_INFO("Cvar dump: {} cvars -> {}", names.size(), path.string());
  }

  // Logging is up by here, so anything noticed during OnConfigurePaths gets
  // said now.
  void OnPostInitLogging() override {
    if (!detect_gpu_.valid)
      return;
    REXLOG_INFO("Hardware detect: {} - dedicated {}, budget {}, in use {}",
                detect_gpu_.name, ng2::FormatGiB(detect_gpu_.dedicated_bytes),
                ng2::FormatGiB(detect_gpu_.budget_bytes),
                ng2::FormatGiB(detect_gpu_.used_bytes));
    REXLOG_INFO("Hardware detect (first run): {}", detect_report_.summary);
    if (!detect_report_.caveat.empty())
      REXLOG_WARN("Hardware detect: {}", detect_report_.caveat);
  }

  // The same bundle the settings button writes, from the environment.
  //
  // The button is in a menu, and the reports worth having most are from people
  // whose game never reaches one. This runs during startup instead, so a
  // launch that dies later still leaves something to send.
  void MaybeWriteDiagnostics() {
    const char* want = std::getenv("NG2_DIAGNOSTICS");
    if (!want || !*want || *want == '0')
      return;
    const auto result = ng2::WriteDiagnostics(
        Ng2Settings::Path(), rex::filesystem::GetExecutableFolder() / "logs");
    if (result.ok)
      REXLOG_INFO("Diagnostics: NG2_DIAGNOSTICS wrote {}", result.file.string());
    else
      REXLOG_ERROR("Diagnostics: {}", result.error);
  }

  void OnPostSetup() override {
    MaybeWriteDiagnostics();
    ArmQuitSeam();

    // Self-update. Wire the module to this install, register the clean-exit that
    // lets the staged updater replace our files, and - if the player has left
    // the setting on - start a background check. The check never blocks the
    // boot; the prompt appears only if a newer version actually exists.
    {
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      ng2::update::Init(NG2_VERSION, folder, folder / "ng2.exe");
      ng2::update::SetApplyHandler([this] {
        app_context().CallInUIThreadDeferred([this] { ExitForUpdate(); });
      });
      if (settings_.check_for_updates) {
        REXLOG_INFO("Update: checking for a newer release (have v{})", NG2_VERSION);
        ng2::update::CheckAsync();
      } else {
        REXLOG_INFO("Update: on-launch check is off");
      }
    }

    // [diag] Opt-in now, and that is the whole point.
    //
    // These two shipped ENABLED in v0.5.0-v0.5.2 and the oscillation scan
    // crashed the game roughly 90 seconds after launch, every launch: it blindly
    // memcpy'd ~502 MB of guest physical memory into a host vector and faulted
    // on the first page the host had not committed. Every crash dump had
    // byte-identical registers because its bounds are hardcoded constants, which
    // is what finally identified it - the fault looked like a guest bug for
    // hours because the attract demo happened to be on screen when the timer
    // elapsed. The profiler is harmless but spams [gprof] into every log.
    //
    // They earned their keep - the oscillation scan is what caught the ledge
    // bug - so they are kept and gated rather than deleted. F7 still runs one
    // scan on demand.
    if (const char* d = std::getenv("NG2_DIAG"); d && *d) {
      REXLOG_WARN("NG2_DIAG is set - starting the guest profiler and the "
                  "oscillation scan loop. These are debugging tools.");
      ng2::StartGuestProfiler();
      ng2::StartOscillationLoop();
    }

    // NG2_FIND_PROJ: one-shot hunt for the perspective projection matrix and the
    // guest function that builds it, for the FOV / ultrawide work. Automated:
    // pair it with NG2_QUIT_AFTER so a scripted run boots, captures, and exits.
    if (const char* fp = std::getenv("NG2_FIND_PROJ"); fp && *fp) {
      REXLOG_WARN("NG2_FIND_PROJ is set - hunting the projection matrix and its "
                  "builder. Debugging tool.");
      ng2::FindProjection();
    }


    if (const char* dump = std::getenv("NG2_DUMP_CVARS"); dump && *dump)
      DumpCvars(dump);

    // Read back what the plugin actually took, now that its cvars exist. This
    // is the only honest check that the deferred config reached it.
    REXLOG_INFO("GPU: internal scale {}x{}, render target path '{}', "
                "clear_memory_page_state={}",
                rex::cvar::GetFlagByName("draw_resolution_scale_x"),
                rex::cvar::GetFlagByName("draw_resolution_scale_y"),
                rex::cvar::GetFlagByName("render_target_path_d3d12"),
                rex::cvar::GetFlagByName("clear_memory_page_state"));
    // The window exists by now, so the comfort settings can go straight on it.
    if (auto* w = window()) {
      // Auto-hide only takes effect in the kAutoHidden visibility mode, so arm
      // the mode here too, not just the delay (see ApplyLiveSettings).
      if (settings_.cursor_hide_seconds > 0) {
        w->SetCursorAutoHideDelayMs(
            static_cast<uint32_t>(settings_.cursor_hide_seconds) * 1000u);
        w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
      } else {
        w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      }
    }

    InstallDlc();
    ImportQueuedSaves();

    UseOneGuestUser();
    GateInputToForeground();

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
      return;

    AllocateSkipPath(memory);

    constexpr uint32_t kLowBase = 0x00000000;  // EXPERIMENT: include page 0
    constexpr uint32_t kLowSize = 0x00040000;  // through 0x3FFFF

    auto* heap = memory->LookupHeap(kLowBase);
    if (!heap) {
      REXLOG_WARN("No heap covers guest 0x{:08X}; low-memory reads will fault",
                  kLowBase);
      return;
    }
    // Commit read+write first: Zero() writes through the guest mapping, so a
    // read-only range faults on our own memset before the game ever runs.
    const bool ok = heap->AllocFixed(
        kLowBase, kLowSize, 0,
        rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
        rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
    if (ok) {
      memory->Zero(kLowBase, kLowSize);
      uint32_t old_protect = 0;
      heap->Protect(kLowBase, kLowSize, rex::memory::kMemoryProtectRead,
                    &old_protect);
      REXLOG_INFO("Committed guest 0x{:08X}-0x{:08X} as read-only zeroes",
                  kLowBase, kLowBase + kLowSize - 1);
    } else {
      REXLOG_WARN("AllocFixed failed for guest 0x{:08X}+0x{:X}", kLowBase,
                  kLowSize);
    }
  }

 private:
  // One guest page holding a path that will never resolve, for the skip-videos
  // patch to point the file wrapper at. Allocated rather than borrowed from
  // some existing string: a real string in .rdata could one day name a real
  // file, and this has to be guaranteed to fail.
  void AllocateSkipPath(rex::memory::Memory* memory) {
    constexpr uint32_t kBase = 0x00040000;  // just above the low-memory commit
    constexpr uint32_t kSize = 0x1000;
    auto* heap = memory->LookupHeap(kBase);
    if (!heap)
      return;
    if (!heap->AllocFixed(kBase, kSize, 0,
                          rex::memory::kMemoryAllocationReserve |
                              rex::memory::kMemoryAllocationCommit,
                          rex::memory::kMemoryProtectRead |
                              rex::memory::kMemoryProtectWrite)) {
      REXLOG_WARN("Skip-videos: could not allocate the placeholder path");
      return;
    }
    memory->Zero(kBase, kSize);
    static constexpr char kPath[] = "game:\__video_skipped__.wmv";
    auto* dest = memory->TranslateVirtual<char*>(kBase);
    if (dest == nullptr)
      return;
    std::memcpy(dest, kPath, sizeof(kPath));
    g_ng2_skip_path_va = kBase;
    REXLOG_INFO("Skip-videos: placeholder path at guest 0x{:08X}", kBase);
  }

  // Repaint the UI while the setup screen is up.
  //
  // Nothing else is driving the presenter at this point: the guest has not
  // been created, so no frame is ever presented, and the SDK only paints the
  // overlay when something asks it to. Without this the setup screen paints
  // once and then freezes - it looks fine in a screenshot and is completely
  // dead to the mouse, because ImGui only consumes its queued input inside a
  // draw. re:Blue needs the same pump for the same reason.
  //
  // The ticks have to come from a second thread: a UI-thread tick that
  // re-enqueues itself never lets the pending-function queue drain, which
  // starves SDL's own event handling and kills the window's close button.
  void StartUiPump() {
    if (ui_pump_thread_.joinable())
      return;
    ui_pump_stop_.store(false, std::memory_order_release);
    ui_pump_pending_.store(false, std::memory_order_release);
    ui_pump_thread_ = std::thread([this] {
      while (!ui_pump_stop_.load(std::memory_order_acquire)) {
        // One repaint in flight at a time. A modal file dialog blocks the UI
        // thread for as long as it is open, and without this the whole time
        // would come back as a queue full of stale repaint requests.
        if (!ui_pump_pending_.exchange(true, std::memory_order_acq_rel)) {
          app_context().CallInUIThreadDeferred([this] {
            if (auto* w = window())
              w->RequestPresenterUIPaintFromUIThread();
            ui_pump_pending_.store(false, std::memory_order_release);
          });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
    });
  }

  void StopUiPump() {
    ui_pump_stop_.store(true, std::memory_order_release);
    if (ui_pump_thread_.joinable())
      ui_pump_thread_.join();
  }

  std::thread ui_pump_thread_;
  std::atomic<bool> ui_pump_stop_{false};
  std::atomic<bool> ui_pump_pending_{false};

  // The texture dump/upscale run, owned by the app so it survives the settings
  // menu being closed. Declared before overlay_ (which borrows &tex_job_) so it
  // is destroyed after it; ~TextureJob cancels and joins the worker on exit.
  ng2::TextureJob tex_job_;

  // The three settings surfaces. Each is owned here and closed by destroying
  // it: ImGuiDialog::Close() does `delete this`, so a dialog must never be
  // both unique_ptr-owned and Close()d.
  std::unique_ptr<ng2::SetupScreen> setup_screen_;
  std::unique_ptr<ng2::SettingsOverlay> overlay_;
  std::unique_ptr<rex::ui::SettingsDialog> advanced_;
  std::unique_ptr<ng2::TextureNotifyOverlay> tex_notify_;
  std::unique_ptr<ng2::WarmOverlay> warm_overlay_;
  std::unique_ptr<ng2::update::UpdateOverlay> update_overlay_;
  std::unique_ptr<ng2::PerfHudOverlay> perf_hud_;

  // What the first-run hardware detection saw, reported once logging exists.
  ng2::GpuInfo detect_gpu_;
  ng2::DetectedSettings detect_report_;
  bool shutting_down_ = false;

  // Watches for the guest terminating the title. See OnPostLaunchModule.
  std::thread title_watcher_;
  std::atomic<bool> title_watcher_stop_{false};
  uint32_t main_guest_thread_id_ = 0;
};
