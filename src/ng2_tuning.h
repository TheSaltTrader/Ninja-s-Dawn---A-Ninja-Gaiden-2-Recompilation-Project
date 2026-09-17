// Ninja Gaiden II specific runtime tuning.
//
// The player never sees a config file. This header holds the settings that
// make *this* title behave, applied before the guest or the GPU plugin start,
// so the shipped build is correct out of the box.
//
// Every entry cites why it is here. Two sources:
//
//   [compat]   Xenia's game-compatibility entry for 544307D5, which carries
//              the labels requires_clear_memory_page_state_true,
//              requires_protect_zero_false and vsync-off-speedup.
//              https://github.com/xenia-project/game-compatibility/issues/296
//   [measured] measured in this project - the number is in the comment.
//
// HOW IT IS APPLIED, AND WHY IT HAS TO BE THIS WAY
//
// The GPU plugin's cvars (draw_resolution_scale, clear_memory_page_state, ...)
// do not exist yet when the app starts: rexgpu-xenos.dll registers them when
// it loads, which is after OnPreSetup and before OnPostSetup. So:
//
//   * SetFlagByName in OnPreSetup fails - the flag is unregistered.
//   * SetFlagByName in OnPostSetup succeeds but is too late - the plugin
//     latched the value at GPU init. This is what made internal resolution
//     scaling look impossible; the app was writing 1 back over the real value
//     after the plugin had already read it.
//
// cvar::LoadConfig is the one path that survives the gap: it *defers* values
// for cvars that are not registered yet ("Config: '{}' deferred (cvar not yet
// registered)") and applies them at registration. So the tuning is written to
// a TOML in the cache root and loaded from OnPreSetup.
//
// Proof the deferral reaches the plugin: launching with --resolution_scale=8
// makes the plugin log "The requested draw resolution scale is not supported
// by the device or the emulator, reducing to 7x7". It read the value.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "ng2_settings.h"

struct Ng2Tuning {
  struct Entry {
    const char* name;
    std::string value;
    const char* why;
  };

  // Fixed settings: correctness for this title, not preferences. These are not
  // exposed in the options UI because there is no sane reason to change them.
  static std::vector<Entry> Fixed() {
    return {
        {"clear_memory_page_state", "true",
         "[compat] requires_clear_memory_page_state_true - refreshes GPU-written "
         "page state; without it Team Ninja titles lose character models"},
        {"protect_zero", "false",
         "[compat] requires_protect_zero_false - the game reads guest 0 during "
         "boot; matches the low-memory commit in OnPostSetup"},
        {"render_target_path_d3d12", "rov",
         "[measured] rov = 3128 EDRAM resolves and 7 pipelines in 30 s, rtv = 22 "
         "and 0. RTV is effectively broken for this title"},
        {"audio_maxqframes", "16",
         "[compat] queued audio frames; 16 is the documented floor and the value "
         "NG2 players use to kill the audio delay"},
        {"d3d12_queue_priority", "1",
         "high priority command queue; 2 would need administrator rights"},
    };
  }

  // Settings the player controls, from the settings menu. Everything here is
  // a cvar the GPU plugin or the presenter owns, so it has to travel the same
  // deferred-config route as the fixed settings above.
  static std::vector<Entry> FromSettings(const Ng2Settings& s,
                                         const std::string& mappings_file) {
    std::vector<Entry> out;
    // Three names for one setting. `resolution_scale` is the convenience the
    // command line takes; the plugin itself reads draw_resolution_scale_x/y,
    // and setting only the convenience left the GPU reporting "internal scale
    // 1x1" while the config said 2.
    out.push_back({"resolution_scale", std::to_string(s.resolution_scale),
                   "true internal supersampling, 1-6"});
    out.push_back({"draw_resolution_scale_x", std::to_string(s.resolution_scale),
                   "what the plugin actually reads for horizontal scale"});
    out.push_back({"draw_resolution_scale_y", std::to_string(s.resolution_scale),
                   "what the plugin actually reads for vertical scale"});

    // Texture cache limits. Only written when the player has asked for
    // something other than the plugin's own defaults, so leaving this alone
    // really does leave it alone rather than re-stating 384/768 as if they
    // were a choice.
    if (s.texture_cache_mb > 0) {
      out.push_back({"texture_cache_memory_limit_soft",
                     std::to_string(s.texture_cache_mb / 2),
                     "host memory the GPU may hold textures in"});
      out.push_back({"texture_cache_memory_limit_hard",
                     std::to_string(s.texture_cache_mb),
                     "the point at which it must evict"});
    }
    // [compat] vsync-off-speedup: this title's logic runs off the display
    // rate, so vsync off does not just tear, it changes game speed. Sent as
    // "true" UNCONDITIONALLY rather than from the setting - the setting is no
    // longer offered in the menu, and a stale or hand-edited file must not be
    // able to reintroduce the speed-up through this path.
    out.push_back({"vsync", "true", "[compat] vsync-off-speedup - forced on"});

    // Post-process antialiasing. The GPU plugin applies this to the swap
    // image; `swap_post_effect` declares none / fxaa / fxaa_extreme, which is
    // the whole of the AA this runtime has. There is deliberately no output
    // filter setting: `present_effect` declares exactly one value, bilinear.
    out.push_back({"swap_post_effect", s.antialias,
                   "post-process antialiasing: none, fxaa, fxaa_extreme"});

    out.push_back({"present_dither", s.present_dither ? "true" : "false",
                   "dither the 10bpc output down to 8bpc"});
    out.push_back({"present_letterbox", s.letterbox ? "true" : "false",
                   "keep the guest aspect ratio instead of stretching"});

    // [compat] gpu-driver-issues-nvidia: the plugin's alpha-test epsilon is the
    // fix for that class of flicker, and it has to be sent unconditionally -
    // it was previously trapped inside the anisotropic guard below, which meant
    // ticking the box did nothing unless the player also happened to override
    // anisotropic filtering. A setting the UI offers must be able to arrive.
    out.push_back({"use_fuzzy_alpha_epsilon", s.fuzzy_alpha ? "true" : "false",
                   "[compat] gpu-driver-issues-nvidia - approximate alpha test"});
    out.push_back({"depth_float24_convert_in_pixel_shader",
                   s.accurate_depth ? "true" : "false",
                   "exact float24 depth: costs shader work, buys depth precision"});
    out.push_back({"depth_float24_round", s.accurate_depth ? "true" : "false",
                   "the other half of exact float24 depth"});

    // -1 means "leave the game's samplers alone", which is the default, so it
    // is only sent when the player actually overrode it. This guard covers ONE
    // setting; nothing else belongs inside it.
    if (s.anisotropic >= 0) {
      out.push_back({"anisotropic_override", std::to_string(s.anisotropic),
                     "forced anisotropic filtering level, 0=1x .. 4=16x"});
    }

    // Texture pack. These are GPU PLUGIN cvars, so this file is the only way
    // to deliver them - passing --texture_dump on the command line reaches
    // nothing, which is worth knowing before spending an evening on it.
    if (s.texture_dump && !s.texture_path.empty()) {
      out.push_back({"texture_dump", "true",
                     "write every unique guest texture out for upscaling"});
      out.push_back({"texture_dump_path", (s.ResolvedTexturePath() / "dump").generic_string(),
                     "where dumped textures go"});
    }
    if (s.texture_pack && !s.texture_path.empty()) {
      out.push_back({"texture_pack_path", (s.ResolvedTexturePath() / "pack").generic_string(),
                     "upscaled textures to load instead of the game's own"});
    }

    // Keyboard-as-pad. The driver is in the runtime and defaults to off, so a
    // PC port that never sets this only works with a real controller.
    out.push_back({"mnk_mode", s.keyboard_control ? "true" : "false",
                   "drive the guest pad from the keyboard"});

    // The d-pad on plain arrow keys. The runtime binds it to Shift+Arrow,
    // which is awkward to press and, as it turns out, does not reach the game
    // at all - a bare Shift+Down does not move the main menu cursor, while the
    // unmodified left-stick keys do. Several of this title's menus navigate
    // with the d-pad, so on the default binding they cannot be used from the
    // keyboard. The arrow keys are free: the left stick is on WASD.
    out.push_back({"keybind_dpad_up", "Up", "plain arrows, not Shift+Arrow"});
    out.push_back({"keybind_dpad_down", "Down", "plain arrows"});
    out.push_back({"keybind_dpad_left", "Left", "plain arrows"});
    out.push_back({"keybind_dpad_right", "Right", "plain arrows"});

    if (!mappings_file.empty()) {
      // The runtime resolves this relative to the working directory, so a
      // shortcut that starts the game from anywhere else silently loses every
      // controller mapping. Give it an absolute path.
      out.push_back({"hid_mappings_file", mappings_file,
                     "absolute path - the default is resolved against the CWD"});
    }

    // --- Hidden GPU measurement / readback test levers -----------------------
    //
    // NOT player settings, and off unless an env var is set - so a normal
    // launch sends none of these and the plugin's own defaults stand
    // (readback_memexport=true, readback_resolve=none). This mirrors the
    // project's env-lever precedent (the retired NG2_RESOLVE_AT_LOAD) and exists
    // to make the optimization work MEASURABLE before anything is changed for
    // real, per the rule "measure before you tune".
    //
    //   NG2_DRAW_CENSUS=<seconds>  turn on the plugin's GPU census - it prints
    //       "[gpu] fence waits in <n> s: ..." and the draw/memexport/resolve
    //       tallies every <seconds>, which is how we learn whether THIS title
    //       even pays the readback cost that cost Fable II its town frame rate.
    //   NG2_MEMEXPORT=0|1  force readback_memexport off/on. Fable II's locked-60
    //       win was =0, but it is a GPU-coherency trade-off that has to be
    //       verified per game, so it lives here as a test lever, not a setting.
    //   NG2_READBACK=none|fast|some|full  the readback_resolve mode.
    //
    // Once a value is measured safe AND a win on this title, it graduates to a
    // fixed entry in Fixed() (baked, like the other correctness cvars) rather
    // than staying an env var.
    if (const char* c = std::getenv("NG2_DRAW_CENSUS"); c && *c) {
      out.push_back({"draw_census", c,
                     "[test lever NG2_DRAW_CENSUS] seconds between GPU census reports"});
    }
    if (const char* mx = std::getenv("NG2_MEMEXPORT"); mx && *mx) {
      out.push_back({"readback_memexport", (mx[0] == '0') ? "false" : "true",
                     "[test lever NG2_MEMEXPORT] shader-memexport CPU readback"});
    }
    if (const char* rb = std::getenv("NG2_READBACK"); rb && *rb) {
      out.push_back({"readback_resolve", rb,
                     "[test lever NG2_READBACK] resolve readback: none/fast/some/full"});
    }
    return out;
  }

  /// Write the entries to `path` and load them into the cvar registry.
  /// Returns false if the file could not be written; a parse failure is
  /// reported by the SDK's own log line.
  static bool Apply(const std::filesystem::path& path,
                    const std::vector<Entry>& entries) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
      REXLOG_WARN("Tuning: cannot write {}", path.string());
      return false;
    }
    out << "# Ninja Gaiden II tuning - generated every launch, do not edit.\n"
        << "# Player-facing settings live in the in-game options screen.\n\n";
    for (const auto& e : entries) {
      out << "# " << e.why << "\n";
      out << e.name << " = " << Quote(e.value) << "\n\n";
    }
    out.close();

    rex::cvar::LoadConfig(path);
    for (const auto& e : entries) {
      REXLOG_INFO("Tuning: {} = {} (now {})", e.name, e.value,
                  rex::cvar::GetFlagByName(e.name));
    }
    return true;
  }

 private:
  // Fixed precision rather than the shortest round-trip: these end up in a
  // TOML file a human may read, and "0.200" is easier to recognise than
  // "0.20000000000000001".
  static std::string ToString(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
  }

  // Numbers and booleans go in bare; everything else is a TOML string.
  static std::string Quote(const std::string& v) {
    if (v == "true" || v == "false") return v;
    bool numeric = !v.empty();
    for (char c : v) {
      if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-')
        numeric = false;
    }
    if (numeric) return v;
    std::string escaped;
    for (char c : v) {
      if (c == '\\' || c == '"') escaped += '\\';
      escaped += c;
    }
    return "\"" + escaped + "\"";
  }
};
