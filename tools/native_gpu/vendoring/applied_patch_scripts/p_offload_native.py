p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
# 1. forced cvars become conditional: forced value unless the native backend OWNS guest memory (plugin offload on).
old_forced = '''    return '%s& FLAGS_%s_storage_() { static %s s = %s; return s; }   // NATIVE FORCED (never the plugin value): no guest-memory writes' % (kind, name, kind, v)'''
new_forced = '''    reader = ('::fable2::ngpu::xlat::PluginString("%s", %s)' % (name, dflt)) if kind == "std::string" else ('::fable2::ngpu::xlat::PluginBool("%s", %s)' % (name, dflt))
    fv = ("std::string(%s)" % v) if kind == "std::string" else v
    return ('%s& FLAGS_%s_storage_() { static %s s = ::fable2::ngpu::rtc::NativeOwnsGuestMemory() ? %s : %s; return s; }'
            '   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)' % (kind, name, kind, reader, fv))'''
assert s.count(old_forced) == 1
s = s.replace(old_forced, new_forced)
s = s.replace('''                 "readback_resolve_mirror_unscaled": "false", "clear_memory_page_state": "false"}''',
              '''                 "readback_resolve_mirror_unscaled": "false", "clear_memory_page_state": "false",
                 # the offload switch itself: the NATIVE backend must never skip its own work
                 "gpu_offload_to_native": "false"}''')
# 2. the data-provider / physical-heap patches become runtime-bound, ownership-gated calls.
repl = [
    ('"nullptr /* NATIVE PATCH: no guest-memory data provider */", 1),',
     '"::fable2::ngpu::rtc::GuestDataProviderRegister(memory_, ResolveDataProviderThunk, this) /* NATIVE PATCH */", 1),'),
    ('"::fable2::ngpu::rtc::NoGuestDataProviders(/* NATIVE PATCH */ ", None),',
     '"::fable2::ngpu::rtc::GuestDataProvidersDisable(memory_, /* NATIVE PATCH */ ", None),'),
    ('"/* NATIVE PATCH: no guest-memory data provider */", 1),',
     '"::fable2::ngpu::rtc::GuestDataProviderUnregister(memory_, resolve_data_provider_handle_);   // NATIVE PATCH", 1),'),
    ('"memory::BaseHeap* heap = nullptr;   // NATIVE PATCH: the readback landing never writes guest memory", 1),',
     '"memory::BaseHeap* heap = ::fable2::ngpu::rtc::GuestPhysicalHeap(memory_);   // NATIVE PATCH: null unless the native backend owns guest memory", 1),'),
]
for a, b in repl:
    assert s.count(a) == 1, a
    s = s.replace(a, b)
open(p, "w", encoding="utf-8").write(s)

f = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/facade.h"
h = open(f, encoding="utf-8").read()
old = """// NATIVE PATCH targets in the vendored command processor (vendor_rtc_d3d12.py SOURCE_PATCHES).
inline void NoGuestDataProviders(uint32_t, uint32_t) {}"""
new = """// OWNERSHIP OF GUEST MEMORY (T3, 2026-09-26). While the plugin renders, it owns guest memory and the native backend
// never writes it (every readback forced off, the calls below no-ops). With the plugin's gpu_offload_to_native on,
// the plugin skips its own GPU work and the native backend becomes the GPU: its readbacks take the plugin's settings
// and land in guest memory, as the plugin's did. The data-provider / physical-heap API exists only in the fork's
// newer runtime, so it is bound at run time (the exe must still start with the older runtime pair).
bool NativeOwnsGuestMemory();
using GuestDataProviderFn = void (*)(void*, std::unique_lock<std::recursive_mutex>&, uint32_t, uint32_t, bool);
void* GuestDataProviderRegister(rex::memory::Memory* memory, GuestDataProviderFn fn, void* context);
void GuestDataProvidersDisable(rex::memory::Memory* memory, uint32_t address, uint32_t length);
void GuestDataProviderUnregister(rex::memory::Memory* memory, void* handle);
rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory);"""
assert h.count(old) == 1
h = h.replace(old, new)
if "#include <mutex>" not in h:
    h = h.replace("#include <memory>", "#include <memory>\n#include <mutex>", 1)
if "namespace rex::memory { class Memory; class BaseHeap; }" not in h:
    h = h.replace("namespace fable2::ngpu::backend { class Driver; }", "namespace rex::memory { class Memory; class BaseHeap; }\nnamespace fable2::ngpu::backend { class Driver; }", 1)
open(f, "w", encoding="utf-8", newline="\n").write(h)

c = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/facade.cpp"
t = open(c, encoding="utf-8").read()
if "NativeOwnsGuestMemory" not in t:
    t = t.replace("""namespace fable2::ngpu::rtc {
NativeContext& Native() {""", """namespace fable2::ngpu::xlat { bool PluginBool(const char*, bool); }

namespace fable2::ngpu::rtc {
bool NativeOwnsGuestMemory() {
  static const bool owns = ::fable2::ngpu::xlat::PluginBool("gpu_offload_to_native", false);
  return owns;
}
namespace {
HMODULE Runtime() { static HMODULE m = GetModuleHandleA("rexruntime.dll"); return m; }
template <typename T>
T Bind(const char* mangled) {
  HMODULE m = Runtime();
  return m ? reinterpret_cast<T>(GetProcAddress(m, mangled)) : nullptr;
}
}  // namespace
// x64: a non-virtual member function is called like a free function with `this` first.
void* GuestDataProviderRegister(rex::memory::Memory* memory, GuestDataProviderFn fn, void* context) {
  if (!NativeOwnsGuestMemory()) return nullptr;
  using F = void* (*)(rex::memory::Memory*, GuestDataProviderFn, void*);
  static F f = Bind<F>("?RegisterPhysicalMemoryDataProvider@Memory@memory@rex@@QEAAPEAXP6AXPEAXAEAV?$unique_lock@Vrecursive_mutex@std@@@std@@II_N@Z0@Z");
  static bool said = false;
  if (!said) { said = true; REXLOG_INFO("[ngpu] BACKEND owns guest memory: runtime data-provider API {}", f ? "bound" : "MISSING (older runtime) - readbacks land only at their usual points"); }
  return f ? f(memory, fn, context) : nullptr;
}
void GuestDataProvidersDisable(rex::memory::Memory* memory, uint32_t address, uint32_t length) {
  if (!NativeOwnsGuestMemory()) return;
  using F = void (*)(rex::memory::Memory*, uint32_t, uint32_t);
  static F f = Bind<F>("?DisablePhysicalMemoryDataProviders@Memory@memory@rex@@QEAAXII@Z");
  if (f) f(memory, address, length);
}
void GuestDataProviderUnregister(rex::memory::Memory* memory, void* handle) {
  if (!NativeOwnsGuestMemory() || !handle) return;
  using F = void (*)(rex::memory::Memory*, void*);
  static F f = Bind<F>("?UnregisterPhysicalMemoryDataProvider@Memory@memory@rex@@QEAAXPEAX@Z");
  if (f) f(memory, handle);
}
rex::memory::BaseHeap* GuestPhysicalHeap(rex::memory::Memory* memory) {
  if (!NativeOwnsGuestMemory()) return nullptr;
  using F = void* (*)(rex::memory::Memory*);
  static F f = Bind<F>("?GetPhysicalHeap@Memory@memory@rex@@QEAAPEAVVirtualHeap@23@XZ");
  // VirtualHeap derives from BaseHeap first (single inheritance), so the pointer is the BaseHeap.
  return f ? static_cast<rex::memory::BaseHeap*>(f(memory)) : nullptr;
}
NativeContext& Native() {""", 1)
    t = t.replace('#include "rtc_d3d12/facade.h"', '#include "rtc_d3d12/facade.h"\n\n#include <windows.h>', 1)
    open(c, "w", encoding="utf-8", newline="\n").write(t)
print("ok")
