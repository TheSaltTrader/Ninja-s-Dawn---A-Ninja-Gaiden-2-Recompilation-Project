// Persistent PC settings for the NG2 recompilation.
//
// Stored as a flat key=value file next to the executable so it can be edited by
// hand or written by the settings menu. Everything here is applied before the
// guest launches; the fields marked "restart" only take effect on the next run
// because the window and the guest video mode are created during startup.
//
// This file owns the *player's* choices. Two other config files exist and are
// deliberately separate:
//
//   cache/ng2_tuning.toml  derived, rewritten every launch from these values
//                          plus the fixed correctness settings (ng2_tuning.h).
//                          Never edit it; it is the transport into the GPU
//                          plugin's cvars, not a place to keep anything.
//   ng2.toml               the SDK's own cvar config, written by the raw cvar
//                          browser on F4. Anything set there is applied before
//                          this file is even read.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>

struct Ng2Settings {
  // --- Display (restart) -------------------------------------------------
  int window_width = 1280;
  int window_height = 720;
  bool fullscreen = false;

  // Which display to open on, 0 = the primary. The runtime has always had a
  // `monitor` cvar and nothing drove it, so the window went wherever Windows
  // put it - which on a two-screen setup is not where the size was chosen for.
  int monitor = 0;
  int fps = 60;              // 30..144, drives the guest video mode
  bool vsync = true;

  // --- Graphics ----------------------------------------------------------
  // True internal render scale: the guest's own framebuffer is rendered at
  // this multiple and downsampled, i.e. supersampling rather than upscaling.
  // 1..6. The plugin accepts up to 7 and says so if asked for more; the menu
  // stopped at 3 for no reason anyone measured, which on a card that can
  // afford 4x is image quality left on the table.
  int resolution_scale = 1;

  // The Xenia community render-size patch: the title renders internally at
  // 1120x584 and scales up. Raising it to 1280x720 removes that scale.
  // Implemented as midasm hooks in patch_hooks.cpp, not a memory patch, since
  // the values are immediates the recompiler has already turned into C++
  // constants.
  bool internal_720p = false;

  // The community Chapter 12 crash workaround (Gliniak, via Xenia's patch file
  // for this title). Off by default: its author is explicit that it causes
  // problems outside that chapter, so it is a "turn on when you get stuck"
  // setting rather than something to leave enabled.
  bool chapter12_workaround = false;

  // How the pre-rendered videos are handled. 1, decoding them ourselves, is
  // simply what the port does now, so the menu does not ask: the alternatives
  // are the corrupted picture (0) and no picture at all (2). It stays here as
  // a way out for anyone who wants one, and a video with no prepared file
  // falls back to the game's own playback on its own.
  // 0 = the game decodes its own videos, which is what this port does now.
  //
  // The v0.5.0 codegen fix (shared_vector_registers) repaired the guest's
  // decoder, which is why v0.5.2 deleted the converted videos and the install
  // step that made them: playback is native and there is nothing to replace.
  //
  // This briefly defaulted to 1 on the strength of a run that black-screened at
  // the intro. That was misdiagnosed - the same failure appears with mode 1,
  // and its actual trigger is dumping and the texture pack running TOGETHER,
  // which puts a file write and a file stat on the GPU thread for every texture
  // the decoder creates and starves the ring buffer. Mode 0 plays the intro and
  // reaches the title screen normally.
  //
  // 0 lets the game play them, 2 skips them entirely. 1 used to replace the
  // picture from files converted at install time, which existed only to work
  // around the broken guest decoder; that decoder is fixed, so 1 is gone and
  // an old config carrying it is folded back to 0 by Apply().
  int video_mode = 0;

  // How much host memory the GPU plugin may hold textures in, in megabytes -
  // 0 keeps the plugin's own 384 MB soft / 768 MB hard defaults, which are
  // console-era numbers on a modern card. Larger means fewer evictions and
  // re-uploads: fewer hitches while streaming, not a sharper picture.
  int texture_cache_mb = 0;

  // Texture pack. Dumping writes every unique guest texture under
  // texture_path/dump for an offline tool to upscale; loading feeds the
  // upscaled ones back in from texture_path/pack.
  //
  // Both default off: dumping costs a file write per newly seen texture, and
  // loading means nothing until a pack has been made.
  bool texture_dump = false;
  bool texture_pack = false;

  // How far the offline tool enlarges each texture.
  //
  // 2 by default, and that is a measured choice rather than caution. A 4x
  // replacement is uncompressed RGBA, so it costs about 128x a DXT1 original:
  // 1290 textures came to 5.9 GB, which is past the plugin's MAXIMUM soft cache
  // limit of 4096 MB. The result was 2900 uploads for 1290 textures - every
  // texture loaded twice over as the cache evicted and refetched it - and
  // hitches of 1.5 to 2 seconds. At 2x the same pack is about 1.5 GB and fits
  // with room to spare.
  //
  // 4 and 8 are offered because the ceiling is memory, and memory grows. 8 is
  // for hardware that does not exist yet; it is not clamped away, because
  // guessing what someone else's machine can hold is how a useful option turns
  // into a missing one.
  int texture_scale = 2;

  // Use Real-ESRGAN rather than a plain resize.
  //
  // Off until the upscaler has been downloaded - it is a 43 MB third-party
  // binary under its own licence, deliberately not shipped with the port so
  // that having it on the machine stays the player's decision.
  bool texture_ai = true;

  // How much of the model's detail to lay over the original, 0..1.
  //
  // Not a cross-fade. Real-ESRGAN is photo-trained and denoises hard - on a
  // smoke texture it cut the mean brightness by 40%, because faint wisps
  // against black are exactly what it treats as noise. So only its FINE DETAIL
  // is taken and the tone stays the game's. 0.75 adds definition while leaving
  // the art recognisably the same art.
  float texture_ai_strength = 0.75f;

  // Whether the memory settings have ever been chosen from the hardware.
  //
  // Separate from `configured` on purpose: that one means "the player has
  // picked a game folder", and a settings file can be carried to a different
  // machine with a different card. This asks a question about the HARDWARE, and
  // it is false until something has actually looked.
  bool hardware_detected = false;
  std::string texture_path;

  // -1 = leave the game's own sampler settings alone, 0..4 = force 1x/2x/4x/
  // 8x/16x anisotropic filtering.
  int anisotropic = -1;

  // Post-process antialiasing, applied by the GPU plugin to the swap image.
  // "none", "fxaa" or "fxaa_extreme" - the list the `swap_post_effect` cvar
  // itself declares. This is the only real AA knob the runtime has; the
  // presenter's `present_effect` offers bilinear and nothing else, so there is
  // no upscaling filter to choose and the menu does not pretend otherwise.
  std::string antialias = "none";

  // The presenter's output filter: "bilinear", "cas", "fsr", "fsr2", "fsr3".
  //
  // CAS is FidelityFX Contrast Adaptive Sharpening - the only real sharpening
  // the runtime has. It was long believed absent: this menu carried a comment
  // saying present_effect "declares exactly one value, bilinear", which was
  // true of the STOCK plugin. The SDK's own build gates the spatial CAS/FSR
  // shaders (committed, prebuilt, needing no download) behind the same option
  // as the temporal FSR2/3 path, and that option is off in the shipped build.
  // A source-built runtime declares all five and CAS works.
  //
  // The row is driven by what the running build actually declares, so on a
  // stock plugin it shows one value and says why rather than offering a filter
  // that would do nothing.
  std::string present_effect = "bilinear";

  // Extra CAS sharpening, 0..1, on top of what the filter does by itself.
  float cas_sharpness = 0.0f;

  // The on-screen readouts, top right. Three switches rather than one, because
  // they answer different questions: fps is "is it smooth", GPU is "is the card
  // the limit", and video memory is "can this machine hold the texture pack".
  // Someone watching for a texture-pack stall wants the last two and not the
  // clutter of the first.
  // The master switch, on F8. Separate from the three below so that turning the
  // readouts off and on again does not lose which ones were chosen - a single
  // combined switch would have to either forget the selection or refuse to be
  // one key.
  // Approximate the alpha test instead of comparing exactly.
  //
  // The plugin's own description names the problem: "prevent flickering on
  // NVIDIA graphics cards". Off by default in the SDK, which is the right
  // default for a runtime that does not know what card it is on - but this port
  // can look, and the machines it runs on are overwhelmingly the ones the flag
  // exists for. Off here too, because it is a change to what the alpha test
  // ACCEPTS and so a correctness trade, not a free win.
  bool fuzzy_alpha = false;

  // Emulate the 360's float24 depth format more exactly.
  //
  // The console's depth buffer is a 24-bit float, which host hardware does not
  // have; the plugin approximates it unless these are on. Exact emulation costs
  // pixel-shader work and buys back depth precision, which is where shadow acne
  // and z-fighting come from. Grouped as one switch because they are two halves
  // of the same behaviour and no useful configuration turns on only one.
  bool accurate_depth = false;

  // Dismiss the in-engine cinematic at the start of a chapter.
  //
  // Armed by the chapter's own story data being opened, and disarmed the moment
  // a real controller is touched - so it cannot still be pressing A once the
  // player has control, which in this game means attacking.
  bool skip_cinematics = false;

  bool hud_enabled = false;

  bool hud_fps = true;
  bool hud_gpu = true;
  bool hud_vram = true;

  // Process CPU across all cores, and a small bar under each readout (frame
  // rate / CPU / GPU / video memory) showing its instantaneous level, in the
  // Fable II style. Both only ever show when the readouts are on (hud_enabled /
  // F8), so an update that adds them changes nothing until the player turns the
  // HUD on.
  bool hud_cpu = true;
  bool hud_ram = true;   // process system-memory (RAM) readout, under CPU
  bool hud_bars = true;

  // The live CPU/GPU/VRAM bars drawn INSIDE the settings, beside the texture
  // switches. Separate from the three above, which are the on-screen readout:
  // they answer different questions. The readout is for playing, these are for
  // deciding - watching the cost move while toggling the pack with F9.
  bool hud_menu_bars = true;

  bool present_dither = false;
  bool letterbox = true;

  // True ultrawide 3D. The guest always renders 16:9; on a wider screen this
  // frame is either pillarboxed (16:9 mode) or filled to the screen width. When
  // it is filled the 3D would stretch, so the GPU plugin widens Ninja Gaiden
  // II's horizontal field of view by exactly the inverse (the ng2_fov_k cvar,
  // applied to each 3D draw's projection column 0) - the two cancel, giving
  // correct proportions with more of the world across the width, no distortion.
  // Vertical FOV, depth, and the 2D screens (chapter cards, menus, videos, HUD)
  // are untouched by the widen. Off by default; toggles live via ApplyFov().
  // The 2D full-screen screens are composed for 16:9 and are stretched by the
  // fill, so 16:9 mode (pillarboxed) stays available for menus and cutscenes.
  bool ultrawide = false;

  // Fine field-of-view multiplier on top of the ultrawide baseline. 1.0 leaves
  // the baseline (correct proportions filling the screen when ultrawide is on,
  // native 16:9 when it is off); higher widens the horizontal FOV further,
  // lower narrows it. Drives ng2_fov_k live from the in-game FOV slider.
  float fov_scale = 1.0f;

  // --- Comfort ------------------------------------------------------------
  // Seconds of mouse stillness over the window before the pointer hides.
  // 0 = never hide. Borrowed from re:Blue, which does the same thing.
  int cursor_hide_seconds = 5;

  // Drive the guest pad from the keyboard and mouse. The runtime has the
  // driver; without this it is off and only a real controller works, which on
  // a PC port is a surprising default.
  bool keyboard_control = false;

  // Check GitHub for a newer release when the game starts, and offer to
  // download and install it. On by default: the prompt appears only when there
  // really is a newer version, and the check itself is a background network
  // call that never blocks the boot. Off means the launcher never touches the
  // network - the "Check now" button in the menu still works by hand.
  bool check_for_updates = true;

  // --- Content -----------------------------------------------------------
  // The last disc image chosen in the installer. Kept only so the field is not
  // blank next launch: an empty box beside a 7 GB file that is still sitting
  // there reads as "the ISO is gone".
  std::string iso_path;

  std::string game_path;  // folder containing default.xex; empty = game/
  std::string dlc_path;   // folder of STFS packages; empty = dlc/

  // Set once the user has pressed Play at least once, so the setup screen only
  // interrupts the first run. Hold Shift at launch to get it back.
  bool configured = false;


  static std::filesystem::path Path() {
    return rex::filesystem::GetExecutableFolder() / "ng2_settings.cfg";
  }

  // A path from the settings file, absolute or relative to the executable's
  // folder. Relative is what makes an install portable: a pre-configured
  // folder with game\, dlc\, textures\ and user\ inside it keeps working
  // when it is moved to another drive or PC, with no visit to the setup
  // screen. Before this a relative path was taken against the process's
  // working directory, which is the folder only when launched from it.
  static std::filesystem::path Beside(const std::string& p) {
    std::filesystem::path path(p);
    if (path.is_absolute())
      return path;
    return rex::filesystem::GetExecutableFolder() / path;
  }

  // Where the game data actually comes from: the configured folder, or game/
  // beside the executable.
  std::filesystem::path ResolvedGamePath() const {
    if (!game_path.empty())
      return Beside(game_path);
    return rex::filesystem::GetExecutableFolder() / "game";
  }

  // The texture folder (dump/ and pack/ inside it). Only meaningful when
  // texture_path is set; callers check that first.
  std::filesystem::path ResolvedTexturePath() const { return Beside(texture_path); }

  // Prepared videos, kept with the game data rather than beside the
  // executable, so they travel with an install wherever it is put and are
  // found again without anyone being asked where they went.
  std::filesystem::path ResolvedVideoPath() const {
    return ResolvedGamePath() / "video";
  }

  std::filesystem::path ResolvedDlcPath() const {
    if (!dlc_path.empty())
      return Beside(dlc_path);
    return rex::filesystem::GetExecutableFolder() / "dlc";
  }

  void Load() {
    std::ifstream in(Path());
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string k = line.substr(0, eq);
      std::string v = line.substr(eq + 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      Apply(k, v);
    }
    REXLOG_INFO("Settings: loaded {}", Path().string());
  }

  void Save() const {
    std::ofstream out(Path(), std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Settings: cannot write {}", Path().string());
      return;
    }
    out << "# ng2 recompilation settings\n"
        << "window_width=" << window_width << "\n"
        << "window_height=" << window_height << "\n"
        << "fullscreen=" << (fullscreen ? 1 : 0) << "\n"
        << "monitor=" << monitor << "\n"
        << "fps=" << fps << "\n"
        << "vsync=" << (vsync ? 1 : 0) << "\n"
        << "resolution_scale=" << resolution_scale << "\n"
        << "internal_720p=" << (internal_720p ? 1 : 0) << "\n"
        << "anisotropic=" << anisotropic << "\n"
        << "texture_cache_mb=" << texture_cache_mb << "\n"
        << "texture_dump=" << (texture_dump ? 1 : 0) << "\n"
        << "texture_pack=" << (texture_pack ? 1 : 0) << "\n"
        << "texture_scale=" << texture_scale << "\n"
        // Without this the detection re-runs every launch and overwrites a
        // choice the player made by hand, which is worse than never detecting.
        << "hardware_detected=" << (hardware_detected ? 1 : 0) << "\n"
        << "texture_path=" << texture_path << "\n"
        << "antialias=" << antialias << "\n"
        << "present_dither=" << (present_dither ? 1 : 0) << "\n"
        << "letterbox=" << (letterbox ? 1 : 0) << "\n"
        << "ultrawide=" << (ultrawide ? 1 : 0) << "\n"
        << "fov_scale=" << fov_scale << "\n"
        << "game_path=" << game_path << "\n"
        << "dlc_path=" << dlc_path << "\n"
        // Read by Apply() but never written back until now, so every change
        // to one of these lasted until the next Save() and then vanished.
        // video_mode is the one that matters: it is what "Skip intro videos"
        // sets, and a setting that forgets itself is worse than no setting.
        << "video_mode=" << video_mode << "\n"
        // Same fault as video_mode above, found the same way: these four were
        // parsed on load and never written on save, so every F8 choice lasted
        // until the next Save() and then went back to the defaults.
        << "hud_enabled=" << (hud_enabled ? 1 : 0) << "\n"
        << "hud_fps=" << (hud_fps ? 1 : 0) << "\n"
        << "hud_gpu=" << (hud_gpu ? 1 : 0) << "\n"
        << "hud_vram=" << (hud_vram ? 1 : 0) << "\n"
        << "hud_cpu=" << (hud_cpu ? 1 : 0) << "\n"
        << "hud_ram=" << (hud_ram ? 1 : 0) << "\n"
        << "hud_bars=" << (hud_bars ? 1 : 0) << "\n"
        << "hud_menu_bars=" << (hud_menu_bars ? 1 : 0) << "\n"
        // Nine more with the same fault, found by the ROUNDTRIP sweep rather
        // than by anyone noticing: each was parsed on load and never written,
        // so it went back to its default at every launch. Between them they
        // cover the AI texture settings, the output filter and its sharpness,
        // both rendering workarounds, the cinematic skip and the remembered
        // disc image - all of them things a player sets once and expects to
        // stay set.
        << "chapter12_workaround=" << (chapter12_workaround ? 1 : 0) << "\n"
        << "texture_ai=" << (texture_ai ? 1 : 0) << "\n"
        << "texture_ai_strength=" << texture_ai_strength << "\n"
        << "present_effect=" << present_effect << "\n"
        << "cas_sharpness=" << cas_sharpness << "\n"
        << "fuzzy_alpha=" << (fuzzy_alpha ? 1 : 0) << "\n"
        << "accurate_depth=" << (accurate_depth ? 1 : 0) << "\n"
        << "skip_cinematics=" << (skip_cinematics ? 1 : 0) << "\n"
        << "iso_path=" << iso_path << "\n"
        << "keyboard_control=" << (keyboard_control ? 1 : 0) << "\n"
        << "check_for_updates=" << (check_for_updates ? 1 : 0) << "\n"
        << "cursor_hide_seconds=" << cursor_hide_seconds << "\n"
        << "configured=" << (configured ? 1 : 0) << "\n";
    REXLOG_INFO("Settings: saved {}", Path().string());
  }

  void Clamp() {
    window_width = std::clamp(window_width, 640, 7680);
    window_height = std::clamp(window_height, 480, 4320);
    fps = std::clamp(fps, 30, 144);
    monitor = std::clamp(monitor, 0, 16);
    resolution_scale = std::clamp(resolution_scale, 1, 8);
    anisotropic = std::clamp(anisotropic, -1, 4);
    texture_cache_mb = std::clamp(texture_cache_mb, 0, 8192);
    // Only the three the tool and the menu actually offer.
    if (texture_scale != 2 && texture_scale != 4 && texture_scale != 8)
      texture_scale = 2;
    texture_ai_strength = std::clamp(texture_ai_strength, 0.0f, 1.0f);
    cas_sharpness = std::clamp(cas_sharpness, 0.0f, 1.0f);
    fov_scale = std::clamp(fov_scale, 0.70f, 1.40f);
    if (antialias.empty())
      antialias = "none";
    cursor_hide_seconds = std::clamp(cursor_hide_seconds, 0, 60);
    video_mode = std::clamp(video_mode, 0, 2);
    if (video_mode == 1)
      video_mode = 0;  // retired overlay mode, see the field's comment
    // Dumping and loading together put the disk on the GPU thread and starve
    // the command stream. The menu makes the pair impossible to select; this
    // makes it impossible to arrive with, from an older config or a hand edit.
    if (texture_dump && texture_pack)
      texture_pack = false;
  }

 private:
  static bool Truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes";
  }

  void Apply(const std::string& k, const std::string& v) {
    if (k == "window_width") window_width = std::atoi(v.c_str());
    else if (k == "window_height") window_height = std::atoi(v.c_str());
    else if (k == "fullscreen") fullscreen = Truthy(v);
    else if (k == "monitor") monitor = std::atoi(v.c_str());
    else if (k == "fps") fps = std::atoi(v.c_str());
    else if (k == "vsync") vsync = Truthy(v);
    else if (k == "resolution_scale") resolution_scale = std::atoi(v.c_str());
    else if (k == "internal_720p") internal_720p = Truthy(v);
    else if (k == "chapter12_workaround") chapter12_workaround = Truthy(v);
    else if (k == "video_mode") video_mode = std::atoi(v.c_str());
    // Migration: the setting used to be a bool that only meant "skip".
    else if (k == "skip_videos") video_mode = Truthy(v) ? 2 : 0;
    else if (k == "anisotropic") anisotropic = std::atoi(v.c_str());
    else if (k == "texture_cache_mb")
      texture_cache_mb = std::atoi(v.c_str());
    else if (k == "texture_dump") texture_dump = Truthy(v);
    else if (k == "texture_pack") texture_pack = Truthy(v);
    else if (k == "texture_scale") texture_scale = std::atoi(v.c_str());
    else if (k == "texture_ai") texture_ai = Truthy(v);
    else if (k == "texture_ai_strength")
      texture_ai_strength = float(std::atof(v.c_str()));
    else if (k == "hardware_detected") hardware_detected = Truthy(v);
    else if (k == "texture_path") texture_path = v;
    else if (k == "antialias") antialias = v;
    else if (k == "fuzzy_alpha") fuzzy_alpha = Truthy(v);
    else if (k == "accurate_depth") accurate_depth = Truthy(v);
    else if (k == "skip_cinematics") skip_cinematics = Truthy(v);
    else if (k == "hud_enabled") hud_enabled = Truthy(v);
    else if (k == "hud_fps") hud_fps = Truthy(v);
    else if (k == "hud_gpu") hud_gpu = Truthy(v);
    else if (k == "hud_vram") hud_vram = Truthy(v);
    else if (k == "hud_cpu") hud_cpu = Truthy(v);
    else if (k == "hud_ram") hud_ram = Truthy(v);
    else if (k == "hud_bars") hud_bars = Truthy(v);
    else if (k == "hud_graph") hud_bars = Truthy(v);  // migrate the old name
    else if (k == "hud_menu_bars") hud_menu_bars = Truthy(v);
    else if (k == "present_effect") present_effect = v;
    else if (k == "cas_sharpness") cas_sharpness = float(std::atof(v.c_str()));
    else if (k == "present_dither") present_dither = Truthy(v);
    else if (k == "letterbox") letterbox = Truthy(v);
    else if (k == "ultrawide") ultrawide = Truthy(v);
    else if (k == "fov_scale") fov_scale = float(std::atof(v.c_str()));
    else if (k == "cursor_hide_seconds") cursor_hide_seconds = std::atoi(v.c_str());
    else if (k == "keyboard_control") keyboard_control = Truthy(v);
    else if (k == "check_for_updates") check_for_updates = Truthy(v);
    else if (k == "iso_path") iso_path = v;
    else if (k == "game_path") game_path = v;
    else if (k == "dlc_path") dlc_path = v;
    else if (k == "configured") configured = Truthy(v);
  }
};
