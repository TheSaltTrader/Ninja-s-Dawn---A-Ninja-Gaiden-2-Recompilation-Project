# vendor_rtc_d3d12.py: EDRAM port phase 2 - vendor the SDK's D3D12 render-target cache and the three rex::ui::d3d12
# helpers it uses into wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/, from rexglue-src 23ace0b, with SYSTEMATIC
# renames only (documented in ORIGIN.txt):
#   namespace rex::graphics::d3d12  -> rex::graphics::ngpu_d3d12   (the plugin's classes are never referenced)
#   namespace rex::ui::d3d12        -> rex::ui::ngpu_d3d12
#   plugin headers (command_processor, deferred_command_list, shared_memory, texture_cache, d3d12_provider)
#     -> "rtc_d3d12/facade.h": NATIVE classes with the same names in the renamed namespaces, built on plume's device
#        and the native command list
#   REXCVAR_DEFINE_* -> accessor-only definitions reading the plugin's registered value
# plus the bytecode headers the cache includes, copied verbatim. Re-run on any re-vendor.
import re, subprocess, os
SRC = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/rexglue-src"
DST = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12"
COMMIT = "23ace0b"
os.makedirs(os.path.join(DST, "bytecode"), exist_ok=True)
FILES = {
    "include/rex/graphics/d3d12/render_target_cache.h": "render_target_cache.h",
    "src/graphics/d3d12/render_target_cache.cpp": "render_target_cache.cpp",
    "include/rex/ui/d3d12/d3d12_cpu_descriptor_pool.h": "d3d12_cpu_descriptor_pool.h",
    "src/ui/d3d12/d3d12_cpu_descriptor_pool.cpp": "d3d12_cpu_descriptor_pool.cpp",
    "include/rex/ui/d3d12/d3d12_upload_buffer_pool.h": "d3d12_upload_buffer_pool.h",
    "src/ui/d3d12/d3d12_upload_buffer_pool.cpp": "d3d12_upload_buffer_pool.cpp",
    "include/rex/ui/d3d12/d3d12_util.h": "d3d12_util.h",
    "src/ui/d3d12/d3d12_util.cpp": "d3d12_util.cpp",
    "include/rex/graphics/d3d12/deferred_command_list.h": "deferred_command_list.h",
    "src/graphics/d3d12/deferred_command_list.cpp": "deferred_command_list.cpp",
    "include/rex/graphics/shared_memory.h": "shared_memory_base.h",
    "src/graphics/shared_memory.cpp": "shared_memory_base.cpp",
    "include/rex/graphics/d3d12/shared_memory.h": "shared_memory.h",
    "src/graphics/d3d12/shared_memory.cpp": "shared_memory.cpp",
    "include/rex/graphics/pipeline/texture/cache.h": "texture_cache_base.h",
    "src/graphics/pipeline/texture/cache.cpp": "texture_cache_base.cpp",
    "src/graphics/pipeline/texture/conversion.cpp": "texture_conversion.cpp",
    "src/graphics/pipeline/texture/extent.cpp": "texture_extent.cpp",
    "src/graphics/pipeline/texture/info.cpp": "texture_info.cpp",
    "src/graphics/pipeline/texture/info_formats.cpp": "texture_info_formats.cpp",
    "src/graphics/pipeline/texture/util.cpp": "texture_util.cpp",
    "include/rex/graphics/d3d12/texture_cache.h": "texture_cache.h",
    "src/graphics/d3d12/texture_cache.cpp": "texture_cache.cpp",
    "include/rex/graphics/d3d12/command_processor.h": "command_processor.h",
    "src/graphics/d3d12/command_processor.cpp": "command_processor.cpp",
    "include/rex/graphics/d3d12/pipeline_cache.h": "pipeline_cache.h",
    "src/graphics/d3d12/pipeline_cache.cpp": "pipeline_cache.cpp",
    "include/rex/graphics/d3d12/primitive_processor.h": "primitive_processor.h",
    "src/graphics/d3d12/primitive_processor.cpp": "primitive_processor.cpp",
    "include/rex/graphics/primitive_processor.h": "primitive_processor_base.h",
    "src/graphics/primitive_processor.cpp": "primitive_processor_base.cpp",
    "include/rex/graphics/d3d12/shader.h": "shader.h",
    "src/graphics/d3d12/shader.cpp": "shader.cpp",
    "include/rex/graphics/command_processor.h": "cp_base.h",
    "src/graphics/command_processor.cpp": "cp_base.cpp",
    "src/graphics/sampler_info.cpp": "sampler_info.cpp",
    "src/graphics/flags.cpp": "flags.cpp",
    "include/rex/ui/d3d12/d3d12_descriptor_heap_pool.h": "d3d12_descriptor_heap_pool.h",
    "src/ui/d3d12/d3d12_descriptor_heap_pool.cpp": "d3d12_descriptor_heap_pool.cpp",
}
INCLUDES = {
    "<rex/graphics/d3d12/render_target_cache.h>": '"rtc_d3d12/render_target_cache.h"',
    "<rex/graphics/d3d12/command_processor.h>": '"rtc_d3d12/command_processor.h"',
    "<rex/graphics/command_processor.h>": '"rtc_d3d12/cp_base.h"',
    "<rex/graphics/d3d12/graphics_system.h>": '"rtc_d3d12/graphics_system_standin.h"',
    "<rex/graphics/graphics_system.h>": '"rtc_d3d12/graphics_system_standin.h"',
    "<rex/ui/d3d12/d3d12_presenter.h>": '"rtc_d3d12/graphics_system_standin.h"',
    "<rex/graphics/d3d12/pipeline_cache.h>": '"rtc_d3d12/pipeline_cache.h"',
    "<rex/graphics/d3d12/primitive_processor.h>": '"rtc_d3d12/primitive_processor.h"',
    "<rex/graphics/primitive_processor.h>": '"rtc_d3d12/primitive_processor_base.h"',
    "<rex/graphics/d3d12/shader.h>": '"rtc_d3d12/shader.h"',
    "<rex/ui/d3d12/d3d12_descriptor_heap_pool.h>": '"rtc_d3d12/d3d12_descriptor_heap_pool.h"',
    "<rex/graphics/d3d12/deferred_command_list.h>": '"rtc_d3d12/deferred_command_list.h"',
    "<rex/graphics/d3d12/shared_memory.h>": '"rtc_d3d12/shared_memory.h"',
    "<rex/graphics/d3d12/texture_cache.h>": '"rtc_d3d12/texture_cache.h"',
    "<rex/graphics/shared_memory.h>": '"rtc_d3d12/shared_memory_base.h"',
    "<rex/graphics/pipeline/texture/cache.h>": '"rtc_d3d12/texture_cache_base.h"',
    "<rex/ui/d3d12/d3d12_provider.h>": '"rtc_d3d12/facade.h"',
    "<rex/ui/d3d12/d3d12_cpu_descriptor_pool.h>": '"rtc_d3d12/d3d12_cpu_descriptor_pool.h"',
    "<rex/ui/d3d12/d3d12_upload_buffer_pool.h>": '"rtc_d3d12/d3d12_upload_buffer_pool.h"',
    "<rex/ui/d3d12/d3d12_util.h>": '"rtc_d3d12/d3d12_util.h"',
}
cvar_bool = re.compile(r'REXCVAR_DEFINE_BOOL\(\s*([a-z0-9_]+)\s*,\s*(true|false)\s*,.*?"\s*\);', re.S)
cvar_str = re.compile(r'REXCVAR_DEFINE_STRING\(\s*([a-z0-9_]+)\s*,\s*("[^"]*")\s*,.*?"\s*\);', re.S)
cvar_int = re.compile(r'REXCVAR_DEFINE_INT32\(\s*([a-z0-9_]+)\s*,\s*(-?[0-9]+)\s*,.*?"\s*\);', re.S)
# NATIVE_FORCED: switches the transplanted backend must NOT take from the plugin. Every GPU->CPU readback lands data in
# GUEST memory, which the plugin (still running) owns - the native backend never writes guest memory.
NATIVE_FORCED = {"readback_resolve": '"none"', "readback_memexport": "false", "readback_memexport_fast": "false",
                 "d3d12_readback_memexport": "false", "d3d12_readback_resolve": "false", "readback_resolve_on_demand": "false",
                 "readback_resolve_mirror_unscaled": "false", "clear_memory_page_state": "false",
                 # the offload switch itself: the NATIVE backend must never skip its own work
                 "gpu_offload_to_native": "false"}
def forced(kind, name, dflt):
    v = NATIVE_FORCED.get(name)
    if v is None:
        return None
    reader = ('::fable2::ngpu::xlat::PluginString("%s", %s)' % (name, dflt)) if kind == "std::string" else ('::fable2::ngpu::xlat::PluginBool("%s", %s)' % (name, dflt))
    fv = ("std::string(%s)" % v) if kind == "std::string" else v
    return ('%s& FLAGS_%s_storage_() { static %s s = ::fable2::ngpu::rtc::NativeOwnsGuestMemory() ? %s : %s; return s; }'
            '   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)' % (kind, name, kind, reader, fv))
# SOURCE_PATCHES (surgical, each one documented): (vendored file, old text, new text, expected count).
SOURCE_PATCHES = [
    # The fork's CPU-readback data providers hook GUEST memory; the native backend never touches guest memory.
    ("command_processor.cpp", "memory_->RegisterPhysicalMemoryDataProvider(ResolveDataProviderThunk, this)",
     "::fable2::ngpu::rtc::GuestDataProviderRegister(memory_, ResolveDataProviderThunk, this) /* NATIVE PATCH */", 1),
    ("command_processor.cpp", "memory_->DisablePhysicalMemoryDataProviders(",
     "::fable2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ ", None),
    ("command_processor.cpp", "memory_->UnregisterPhysicalMemoryDataProvider(resolve_data_provider_handle_);",
     "::fable2::ngpu::rtc::GuestDataProviderUnregister(memory_, resolve_data_provider_handle_);   // NATIVE PATCH", 1),
    # IssueSwap: the native renderer presents; the plugin's gamma / FXAA pass writes into a NATIVE guest-output texture.
    ("command_processor.cpp", """  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }
""", "  // NATIVE PATCH: no presenter check - NativeRefreshGuestOutput (below) stands in for the presenter.\n", 1),
    ("command_processor.cpp", "presenter->RefreshGuestOutput(", "::fable2::ngpu::rtc::NativeRefreshGuestOutput(", 1),
    # The readback LANDING (a copy into guest memory) checks the physical heap first; no heap = it never lands. A
    # second layer behind the forced-off readback cvars.
    ("command_processor.cpp", "memory::BaseHeap* heap = memory_->physical_heap();",
     "memory::BaseHeap* heap = ::fable2::ngpu::rtc::GuestPhysicalHeap(memory_);   // NATIVE PATCH: null unless the native backend owns guest memory", 1),
    # The native replay driver calls the protected entry points the PM4 parser would (SetupContext, WriteRegister,
    # LoadShader, IssueDraw, IssueCopy, submission control) and sets the active shaders.
    ("command_processor.h", "class D3D12CommandProcessor : public CommandProcessor {\n public:",
     "class D3D12CommandProcessor : public CommandProcessor {\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\n public:", 1),
    # The base class's gamma tables are private; the driver copies the plugin's tables in at each swap.
    ("cp_base.h", "class CommandProcessor {\n public:",
     "class CommandProcessor {\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\n public:", 1),
    ("cp_base.h", "namespace rex::graphics {\n\nclass GraphicsSystem;",
     "namespace fable2::ngpu::backend { class Driver; }   // NATIVE PATCH\n\nnamespace rex::graphics {\n\nclass GraphicsSystem;", 1),
    # RenderDoc detection (debug-marker auto-enable) needs renderdoc_app.h, which the native build does not carry:
    # markers follow the plugin's gpu_debug_markers flag only.
    ("flags.cpp", "#include <rex/ui/renderdoc_api.h>\n", "// NATIVE PATCH: <rex/ui/renderdoc_api.h> not included (no RenderDoc detection)\n", 1),
    ("flags.cpp", """    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }""", "    }   // NATIVE PATCH: no RenderDoc detection", 1),
]
def replace_cvars(t, kind_macro, make):
    """Replace every KIND_MACRO(name, default, ...); by make(name, default). Scans to the MATCHING close paren,
    skipping string and char literals, so descriptions containing ');' or ',' cannot derail it."""
    out, i, n = [], 0, 0
    key = kind_macro + "("
    while True:
        j = t.find(key, i)
        if j < 0:
            out.append(t[i:]); break
        # only a definition at the start of a line (not inside a comment or another macro)
        line_start = t.rfind("\n", 0, j) + 1
        if t[line_start:j].strip():
            out.append(t[i:j + len(key)]); i = j + len(key); continue
        k, depth, args, cur = j + len(key), 1, [], []
        while depth:
            ch = t[k]
            if ch in "\"'":
                q = ch; cur.append(ch); k += 1
                while t[k] != q:
                    if t[k] == "\\":
                        cur.append(t[k]); k += 1
                    cur.append(t[k]); k += 1
                cur.append(q); k += 1; continue
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if not depth:
                    break
            elif ch == "," and depth == 1:
                args.append("".join(cur).strip()); cur = []; k += 1; continue
            cur.append(ch); k += 1
        args.append("".join(cur).strip())
        k += 1
        # chained builder calls (.range(...), .lifecycle(...)) belong to the definition too
        while True:
            m = k
            while t[m] in " \t\r\n":
                m += 1
            if t[m] != ".":
                break
            m = t.index("(", m) + 1
            d = 1
            while d:
                if t[m] in "\"'":
                    q = t[m]; m += 1
                    while t[m] != q:
                        m += 2 if t[m] == "\\" else 1
                elif t[m] == "(":
                    d += 1
                elif t[m] == ")":
                    d -= 1
                m += 1
            k = m
        while t[k] in " \t":
            k += 1
        if t[k] == ";":
            k += 1
        out.append(t[i:j]); out.append(make(args[0], args[1])); n += 1
        i = k
    return "".join(out), n
bytecode = set()
for src, dst in FILES.items():
    t = subprocess.check_output(["git", "-C", SRC, "show", f"{COMMIT}:{src}"]).decode("utf-8")
    for a, b in INCLUDES.items():
        t = t.replace("#include " + a, "#include " + b)
    t = t.replace("namespace rex::graphics::d3d12", "namespace rex::graphics::ngpu_d3d12")
    t = t.replace("namespace rex::ui::d3d12", "namespace rex::ui::ngpu_d3d12")
    t = re.sub(r'(?<![A-Za-z0-9_:])ui::d3d12::', 'ui::ngpu_d3d12::', t)
    t = re.sub(r'(?<![A-Za-z0-9_])rex::ui::d3d12::', 'rex::ui::ngpu_d3d12::', t)
    nb = ns = ni = 0
    t, nb = replace_cvars(t, "REXCVAR_DEFINE_BOOL", lambda name, dflt: forced("bool", name, dflt) or 'bool& FLAGS_%s_storage_() { static bool s = ::fable2::ngpu::xlat::PluginBool("%s", %s); return s; }' % (name, name, dflt))
    t, ns = replace_cvars(t, "REXCVAR_DEFINE_STRING", lambda name, dflt: forced("std::string", name, dflt) or 'std::string& FLAGS_%s_storage_() { static std::string s = ::fable2::ngpu::xlat::PluginString("%s", %s); return s; }' % (name, name, dflt))
    t, ni = replace_cvars(t, "REXCVAR_DEFINE_INT32", lambda name, dflt: 'int32_t& FLAGS_%s_storage_() { static int32_t s = ::fable2::ngpu::xlat::PluginInt("%s", %s); return s; }' % (name, name, dflt))
    t, nd = replace_cvars(t, "REXCVAR_DEFINE_DOUBLE", lambda name, dflt: 'double& FLAGS_%s_storage_() { static double s = ::fable2::ngpu::xlat::PluginDouble("%s", %s); return s; }' % (name, name, dflt))
    t, nu = replace_cvars(t, "REXCVAR_DEFINE_UINT32", lambda name, dflt: 'uint32_t& FLAGS_%s_storage_() { static uint32_t s = uint32_t(::fable2::ngpu::xlat::PluginInt("%s", int32_t(%s))); return s; }' % (name, name, dflt))
    for (pf, old, new, cnt) in SOURCE_PATCHES:
        if pf == dst:
            c = t.count(old)
            if c == 0 or (cnt is not None and c != cnt):
                raise SystemExit("SOURCE PATCH anchor count %d in %s: %r" % (c, pf, old[:60]))
            t = t.replace(old, new)
    for m in re.finditer(r'#include "\.\./shaders/bytecode/d3d12_5_1/([a-z0-9_]+\.h)"', t):
        bytecode.add(m.group(1))
    t = re.sub(r'#include "\.\./shaders/bytecode/d3d12_5_1/([a-z0-9_]+\.h)"', r'#include "rtc_d3d12/bytecode/\1"', t)
    t = t.replace('#include "thirdparty/dxbc/DXBCChecksum.h"', '#include "thirdparty/dxbc/DXBCChecksum.h"')
    header = ("// VENDORED from rexglue-src %s:%s - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):\n"
              "// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (%d bool, %d string, %d int).\n"
              "#include <string>\n#include <cstdint>\n#include <rex/logging.h>\n"
              "namespace fable2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }\n"
              % (COMMIT, src, nb, ns, ni))
    open(os.path.join(DST, dst), "w", encoding="utf-8", newline="\n").write(header + t)
    print(dst, "cvars", nb, ns, ni)
for b in sorted(bytecode):
    data = subprocess.check_output(["git", "-C", SRC, "show", f"{COMMIT}:src/graphics/shaders/bytecode/d3d12_5_1/{b}"])
    open(os.path.join(DST, "bytecode", b), "wb").write(data)
print("bytecode headers:", len(bytecode))
