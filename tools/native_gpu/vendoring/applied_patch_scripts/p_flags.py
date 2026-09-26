p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
if '"src/graphics/flags.cpp"' not in s:
    s = s.replace('    "src/graphics/sampler_info.cpp": "sampler_info.cpp",', '    "src/graphics/sampler_info.cpp": "sampler_info.cpp",\n    "src/graphics/flags.cpp": "flags.cpp",')
open(p, "w", encoding="utf-8").write(s)

x = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/xlat_support.cpp"
t = open(x, encoding="utf-8").read()
t = t.replace('''std::string& FLAGS_dump_shaders_storage_() { static std::string storage; return storage; }
bool& FLAGS_use_fuzzy_alpha_epsilon_storage_() { static bool storage = false; return storage; }
''', '''// dump_shaders / use_fuzzy_alpha_epsilon: defined by the vendored flags.cpp (rtc_d3d12/flags.cpp, 2026-09-26) as
// accessor-only reads of the PLUGIN's registered value - still never registered by this module.
std::string& FLAGS_dump_shaders_storage_();
bool& FLAGS_use_fuzzy_alpha_epsilon_storage_();
''')
a = t.index("// EDRAM PORT (2026-09-26): plugin flags (src/graphics/flags.cpp)")
b = t.index("// (the fork's shared-memory upload flags")
t = t[:a] + "// (plugin flags from src/graphics/flags.cpp now come with the vendored rtc_d3d12/flags.cpp)\n" + t[b:]
open(x, "w", encoding="utf-8").write(t)
print("ok")
