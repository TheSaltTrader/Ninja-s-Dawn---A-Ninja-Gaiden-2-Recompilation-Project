// Definitions the vendored SDK shader translator links against that live in
// OTHER plugin translation units (not vendored): two cvars from
// src/graphics/flags.cpp and one constant table from src/graphics/util/draw.cpp
// (rexglue-src c94f5eb, the commit matching the installed SDK headers).
//
// CVAR OWNERSHIP, deliberately: these accessors are NOT registered with
// rex::cvar. The exe's static initialisers run before rexgpu-xenos.dll is
// loaded, so an exe-side REXCVAR_DEFINE would win the name and the plugin's
// registration would be REJECTED - then a runtime `dump_shaders=<dir>` would
// reach this copy and the plugin would silently keep its default (and never
// dump, which is the oracle extractor the DXBC differential depends on). With
// accessor-only definitions the plugin keeps ownership; this module reads its
// own default and dumps through its own cvar (ngpu_shader_diff_dir). The same
// treatment is applied inside the vendored dxbc_translator*.cpp for the three
// cvars they define (dxbc_switch, dxbc_source_map,
// draw_resolution_scaled_texture_offsets). LogCvarOwnership() prints, once,
// the registry's value beside this module's for all five, so a divergence is
// visible rather than inferred.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/util/draw.h>

#include <cstdint>
#include <string>

// dump_shaders / use_fuzzy_alpha_epsilon: defined by the vendored flags.cpp (rtc_d3d12/flags.cpp, 2026-09-26) as
// accessor-only reads of the PLUGIN's registered value - still never registered by this module.
std::string& FLAGS_dump_shaders_storage_();
bool& FLAGS_use_fuzzy_alpha_epsilon_storage_();
bool& FLAGS_dxbc_switch_storage_();
bool& FLAGS_dxbc_source_map_storage_();
bool& FLAGS_draw_resolution_scaled_texture_offsets_storage_();

// (kD3D10StandardSamplePositions4x now comes with the full vendored draw.cpp - draw_util_vendored.cpp, 2026-09-26.)

namespace ng2::ngpu::xlat {
// The PLUGIN's registered value of a boolean cvar (the vendored render-target cache / extent estimator read these
// through accessor-only definitions). Read once per name at first use - the plugin is loaded by then.
bool PluginBool(const char* name, bool fallback) {
  const std::string v = rex::cvar::GetFlagByName(name);
  const bool r = v.empty() ? fallback : (v == "true" || v == "1");
  REXLOG_INFO("[ngpu] vendored SDK cvar {} = {} (plugin registry '{}')", name, r, v);
  return r;
}
std::string PluginString(const char* name, const char* fallback) {
  const std::string v = rex::cvar::GetFlagByName(name);
  REXLOG_INFO("[ngpu] vendored SDK cvar {} = '{}' (plugin registry '{}')", name, v.empty() ? fallback : v, v);
  return v.empty() ? std::string(fallback) : v;
}
int32_t PluginInt(const char* name, int32_t fallback) {
  const std::string v = rex::cvar::GetFlagByName(name);
  int32_t r = fallback;
  if (!v.empty()) { try { r = std::stoi(v); } catch (...) {} }
  REXLOG_INFO("[ngpu] vendored SDK cvar {} = {} (plugin registry '{}')", name, r, v);
  return r;
}
double PluginDouble(const char* name, double fallback) {
  const std::string v = rex::cvar::GetFlagByName(name);
  double r = fallback;
  if (!v.empty()) { try { r = std::stod(v); } catch (...) {} }
  REXLOG_INFO("[ngpu] vendored SDK cvar {} = {} (plugin registry '{}')", name, r, v);
  return r;
}
void LogCvarOwnership() {
  static bool done = false;
  if (done) return;
  done = true;
  auto reg = [](const char* n) { std::string v = rex::cvar::GetFlagByName(n); return v.empty() ? std::string("<unregistered or empty>") : v; };
  REXLOG_INFO("[ngpu] in-app shader translator cvars (registry value | this module's copy): dump_shaders='{}'|'{}' "
              "use_fuzzy_alpha_epsilon={}|{} dxbc_switch={}|{} dxbc_source_map={}|{} draw_resolution_scaled_texture_offsets={}|{} "
              "- the registry entry belongs to the PLUGIN; this module never registers these names, so a runtime change reaches the plugin only",
              reg("dump_shaders"), FLAGS_dump_shaders_storage_(),
              reg("use_fuzzy_alpha_epsilon"), FLAGS_use_fuzzy_alpha_epsilon_storage_(),
              reg("dxbc_switch"), FLAGS_dxbc_switch_storage_(),
              reg("dxbc_source_map"), FLAGS_dxbc_source_map_storage_(),
              reg("draw_resolution_scaled_texture_offsets"), FLAGS_draw_resolution_scaled_texture_offsets_storage_());
}
}  // namespace ng2::ngpu::xlat

// (plugin flags from src/graphics/flags.cpp now come with the vendored rtc_d3d12/flags.cpp)
// (the fork's shared-memory upload flags now come with the vendored rtc_d3d12/command_processor.cpp)
