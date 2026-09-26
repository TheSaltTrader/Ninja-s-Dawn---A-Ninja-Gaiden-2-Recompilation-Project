p = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/rexglue-src/src/graphics/command_processor.cpp"
s = open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
old_export = """static const rex::graphics::CommandProcessor* g_ngpu_swap_cp = nullptr;
__declspec(dllexport) bool RexNgpuGetGammaRamp(uint32_t* table_256, uint32_t* pwl_rgb) {
  if (!g_ngpu_swap_cp) return false;
  std::memcpy(table_256, g_ngpu_swap_cp->gamma_ramp_256_entry_table(), 256 * sizeof(uint32_t));
  std::memcpy(pwl_rgb, g_ngpu_swap_cp->gamma_ramp_pwl_rgb(), 128 * 3 * sizeof(uint32_t));
  return true;
}""".replace("\n", nl)
new_export = """// The swap handler (a member, so it may read the protected tables) copies them here just before the callback.
static bool g_ngpu_swap_gamma_valid = false;
static uint32_t g_ngpu_swap_gamma_table[256];
static uint32_t g_ngpu_swap_gamma_pwl[128 * 3];
__declspec(dllexport) bool RexNgpuGetGammaRamp(uint32_t* table_256, uint32_t* pwl_rgb) {
  if (!g_ngpu_swap_gamma_valid) return false;
  std::memcpy(table_256, g_ngpu_swap_gamma_table, sizeof(g_ngpu_swap_gamma_table));
  std::memcpy(pwl_rgb, g_ngpu_swap_gamma_pwl, sizeof(g_ngpu_swap_gamma_pwl));
  return true;
}""".replace("\n", nl)
assert s.count(old_export) == 1
s = s.replace(old_export, new_export)
old_set = "  g_ngpu_swap_cp = this;   // RexNgpuGetGammaRamp reads this instance during the callback" + nl
new_set = ("  if (g_ngpu_swap_cb) {   // RexNgpuGetGammaRamp returns these only during the callback" + nl +
           "    std::memcpy(g_ngpu_swap_gamma_table, gamma_ramp_256_entry_table(), sizeof(g_ngpu_swap_gamma_table));" + nl +
           "    std::memcpy(g_ngpu_swap_gamma_pwl, gamma_ramp_pwl_rgb(), sizeof(g_ngpu_swap_gamma_pwl));" + nl +
           "    g_ngpu_swap_gamma_valid = true;" + nl +
           "  }" + nl)
assert s.count(old_set) == 1
s = s.replace(old_set, new_set)
old_clr = "  g_ngpu_swap_cp = nullptr;   // only valid DURING the callback - a call from anywhere else returns false"
assert s.count(old_clr) == 1
s = s.replace(old_clr, "  g_ngpu_swap_gamma_valid = false;   // only valid DURING the callback - a call from anywhere else returns false")
s = s.replace("// callback (the instance pointer is set just before it and cleared just after).", "// callback (the copies are marked valid just before it and invalid just after).")
open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
