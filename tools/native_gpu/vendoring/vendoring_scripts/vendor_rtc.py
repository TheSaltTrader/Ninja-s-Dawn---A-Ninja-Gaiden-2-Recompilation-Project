# vendor_rtc.py: vendor the SDK render-target cache base + draw extent estimator from rexglue-src f495e67 (the commit
# the inline plugin bin_main_head_imm was built from) into wt-fable2-nativegpu/src/native_gpu_xlat, replacing each
# REXCVAR_DEFINE_BOOL with an ACCESSOR-ONLY definition that returns the PLUGIN's registered value (the plugin owns
# the names - see xlat_support.cpp). Re-run on any re-vendor.
import re, subprocess, os
SRC = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/rexglue-src"
DST = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat"
COMMIT = "23ace0b"   # f495e67 + the env-gated owner trace in cache.cpp (identical otherwise)
FILES = {
    "src/graphics/pipeline/render_target/cache.cpp": "rt_cache_vendored.cpp",
    "src/graphics/util/draw_extent_estimator.cpp": "draw_extent_estimator_vendored.cpp",
    "src/graphics/util/draw.cpp": "draw_util_vendored.cpp",
    "src/graphics/pipeline/shader/interpreter.cpp": "shader_interpreter_vendored.cpp",
}
pat = re.compile(r'REXCVAR_DEFINE_BOOL\(\s*([a-z0-9_]+)\s*,\s*(true|false)\s*,.*?\);', re.S)
for src, dst in FILES.items():
    text = subprocess.check_output(["git", "-C", SRC, "show", f"{COMMIT}:{src}"]).decode("utf-8")
    n = 0
    def sub(m):
        global n
        return ("// VENDORED PATCH: accessor-only - the plugin registered '%s'; this reads its value (default %s if unset).\n"
                "bool& FLAGS_%s_storage_() { static bool s = ::fable2::ngpu::xlat::PluginBool(\"%s\", %s); return s; }"
                % (m.group(1), m.group(2), m.group(1), m.group(1), m.group(2)))
    text2, count = pat.subn(sub, text)
    text2 = text2.replace("[owner-trace]", "[owner-trace-native]")
    header = ("// VENDORED VERBATIM from rexglue-src %s:%s (see ORIGIN.txt), except: each REXCVAR_DEFINE_BOOL is replaced by an\n"
              "// accessor-only definition reading the plugin's registered value (%d replaced).\n"
              "namespace fable2::ngpu::xlat { bool PluginBool(const char* name, bool fallback); }\n" % (COMMIT, src, count))
    open(os.path.join(DST, dst), "w", encoding="utf-8", newline="\n").write(header + text2)
    print(dst, "cvars replaced:", count, "lines:", text2.count("\n"))
