// Diagnostic midasm hook bodies. Declared in config/hooks/diagnostics.toml.
//
// These only log. Delete the TOML entries (and this file from CMakeLists) once
// the startup crash in init phase 2 is understood.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <cmath>
#include <tlhelp32.h>
#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <array>

#include "ng2_autoskip.h"
#include "ng2_chapter.h"

#include <rex/cvar.h>
#include "third_party/renderdoc_app.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

// Diagnostic trace for the chapter 12->13 clear-detector. Instrumented call
// sites in generated/ (local/diag/patch_clear.py) call this. Ids >= 10 carry a
// per-frame value and are deduped (only log on change) so they cannot flood;
// ids < 10 are rare events and log every call. Remove with patch_clear.py --revert.
extern "C" void ng2_diag_xtrace(int id, unsigned val) {
  if (id < 0 || id >= 128) return;
  if (id >= 10) {
    static unsigned last[128];
    static bool seen[128];
    if (seen[id] && last[id] == val) return;
    seen[id] = true;
    last[id] = val;
  }
  REXLOG_INFO("[xtrace] pt {} = 0x{:08X}", id, val);
}

namespace {

// Read one big-endian guest word, or report failure. Never dereferences a null
// or obviously bogus address - this runs on the guest thread during a crash
// path, so it must not fault itself.
bool ReadGuestU32(uint32_t va, uint32_t& out) {
  if (va < 0x1000 || (va & 3))
    return false;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory)
    return false;
  auto* p = memory->TranslateVirtual<const uint8_t*>(va);
  if (!p)
    return false;
  out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  return true;
}

int g_enter_count = 0;

// Read a NUL-terminated guest string, bounded.
std::string ReadGuestString(uint32_t va, size_t limit = 128) {
  std::string out;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || va < 0x1000)
    return out;
  auto* p = memory->TranslateVirtual<const char*>(va);
  if (!p)
    return out;
  for (size_t i = 0; i < limit && p[i]; ++i)
    out.push_back(p[i]);
  return out;
}

int g_open_count = 0;
int g_frame = 0;

// The game resolves its 1280x720 k_8_8_8_8 framebuffer alternately to these
// two physical addresses (seen in the GPU trace).
constexpr uint32_t kResolveA = 0x1F045000;
constexpr uint32_t kResolveB = 0x1F3DD000;
constexpr int kFbW = 1280, kFbH = 720;

void DumpPhysical(uint32_t phys, const char* path) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  auto* p = memory->TranslatePhysical<const uint8_t*>(phys);
  if (!p) return;
  if (FILE* f = std::fopen(path, "wb")) {
    std::fwrite(p, 1, size_t(kFbW) * kFbH * 4, f);
    std::fclose(f);
  }
}
int g_switch_count = 0;
uint32_t g_switch_targets[8] = {};
int g_switch_distinct = 0;


}  // namespace

// The generated dispatch externs these with C++ linkage, so no extern "C".


// [diag] Windowed guest profiler for the ledge jump-loop and the black screen
// on "Quit Game". Reports the hottest guest functions per 5-second window;
// windowed rather than cumulative, because a cumulative histogram is dominated
// by ordinary play and hides exactly the state we are trying to see.
namespace {

std::atomic<bool> g_prof_started{false};

uint32_t ProfLookup(const void* rip) {
  using Fn = uint32_t(__cdecl*)(const void*);
  static Fn fn = [] {
    HMODULE m = GetModuleHandleA("rexruntime.dll");
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, "Ng2LookupGuestByHostC")) : nullptr;
  }();
  return fn ? fn(rip) : 0;
}

void GuestProfilerThread() {
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  std::vector<std::pair<DWORD, HANDLE>> threads;
  std::unordered_map<uint32_t, int> window;
  int ticks = 0, rescan = 100000, total = 0;
  REXLOG_INFO("[gprof] guest profiler running");
  while (true) {
    if (++rescan >= 3000) {
      rescan = 0;
      for (auto& t : threads) CloseHandle(t.second);
      threads.clear();
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
          do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                                  te.th32ThreadID);
            if (h) threads.push_back({te.th32ThreadID, h});
          } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
      }
    }
    for (auto& t : threads) {
      if (SuspendThread(t.second) == DWORD(-1)) continue;
      CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
      uint64_t rip = 0;
      if (GetThreadContext(t.second, &c)) rip = c.Rip;
      ResumeThread(t.second);
      if (!rip) continue;
      // NO filtering. Excluding the known spin loops made the profile go flat
      // during the fault with nothing above 5%, which is the signature of the
      // time having gone somewhere excluded. If the game parks in a spin loop
      // while the character jumps on its own, that IS the finding.
      const uint32_t g = ProfLookup(reinterpret_cast<const void*>(rip));
      if (!g) continue;
      ++window[g]; ++total;
    }
    Sleep(1);
    if (++ticks >= 5000) {
      ticks = 0;
      std::vector<std::pair<int, uint32_t>> top;
      for (auto& kv : window) top.push_back({kv.second, kv.first});
      std::sort(top.rbegin(), top.rend());
      std::string line;
      for (size_t i = 0; i < top.size() && i < 12; ++i)
        line += fmt::format("{}0x{:08X}:{:.1f}%", i ? "  " : "", top[i].second,
                            100.0 * top[i].first / (total ? total : 1));
      REXLOG_INFO("[gprof] {} samples: {}", total, line);
      window.clear(); total = 0;
    }
  }
}

}  // namespace

namespace ng2 {
void StartGuestProfiler() {
  bool expected = false;
  if (g_prof_started.compare_exchange_strong(expected, true))
    std::thread(GuestProfilerThread).detach();
}
}  // namespace ng2



// ---------------------------------------------------------------------------
// [diag] Who writes the oscillating value?
//
// The scan found a float sweeping 0 -> ~99 and resetting several times a
// second while the character is stuck in an animation with no input. That is
// the animation's own progress, so whatever writes it is the state machine
// that will not leave the state. Trapping writes to it names that code - the
// same route that found the video decoder, which worked when reasoning did not.
//
// The page is marked read-only, the first write faults, the handler records the
// guest function and lets it through. Re-armed a few times to collect the
// distinct writers rather than only the first one to get there.
namespace {

uint32_t g_wt_target = 0;
PVOID g_wt_handle = nullptr;
uint64_t g_wt_host = 0;
uint64_t g_wt_alias[4] = {};
int g_wt_alias_count = 0;
uint32_t g_wt_seen[16] = {};
int g_wt_count = 0;

uint32_t WtLookup(const void* rip) {
  using Fn = uint32_t(__cdecl*)(const void*);
  static Fn fn = [] {
    HMODULE m = GetModuleHandleA("rexruntime.dll");
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, "Ng2LookupGuestByHostC")) : nullptr;
  }();
  return fn ? fn(rip) : 0;
}

// A HARDWARE watchpoint, not a guarded page.
//
// Page protection was the wrong instrument: the value shares its 4 KB page with
// plenty of other traffic, so nearly every fault came from an unrelated write,
// and letting that one through disarmed the trap before ours was ever touched.
// The debug registers watch exactly four bytes and fire for nothing else.
//   DR0  = the address
//   DR7  = L0 enabled, R/W0 = 01 (write), LEN0 = 11 (4 bytes)  -> 0xD0001
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
    return EXCEPTION_CONTINUE_SEARCH;
  if (!(info->ContextRecord->Dr6 & 0xF))
    return EXCEPTION_CONTINUE_SEARCH;
  info->ContextRecord->Dr6 = 0;

  const void* rip = reinterpret_cast<const void*>(info->ContextRecord->Rip);
  const uint32_t fn = WtLookup(rip);
  for (int i = 0; i < g_wt_count; ++i)
    if (g_wt_seen[i] == fn) return EXCEPTION_CONTINUE_EXECUTION;
  if (g_wt_count < 16) {
    g_wt_seen[g_wt_count++] = fn;
    void* frames[18] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 18, frames, nullptr);
    std::string chain;
    uint32_t last = 0;
    for (USHORT i = 0; i < n; ++i) {
      const uint32_t g = WtLookup(frames[i]);
      if (g && g != last) {
        chain += fmt::format("{}0x{:08X}", chain.empty() ? "" : " <- ", g);
        last = g;
      }
    }
    REXLOG_INFO("[animwriter] #{} guest_fn=0x{:08X} wrote 0x{:08X}  chain: {}",
                g_wt_count, fn, g_wt_target, chain);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ApplyWatchToAllThreads() {
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  int applied = 0;
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
      HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                            FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) != DWORD(-1)) {
        CONTEXT c;
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
          // One debug register per alias: R/W = 01 (write), LEN = 11 (4 bytes).
          uint64_t dr7 = 0;
          for (int k = 0; k < g_wt_alias_count && k < 4; ++k) {
            (&c.Dr0)[k == 3 ? 3 : k] = g_wt_alias[k];
            dr7 |= (1ull << (k * 2));                   // Lk
            dr7 |= (0b01ull << (16 + k * 4));           // write
            dr7 |= (0b11ull << (18 + k * 4));           // 4 bytes
          }
          c.Dr7 = dr7;
          c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
          if (SetThreadContext(h, &c)) ++applied;
        }
        ResumeThread(h);
      }
      CloseHandle(h);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  if (g_wt_count == 0) REXLOG_INFO("[animwriter] watchpoint on {} threads", applied);
}

void ArmWriteTrap(uint32_t guest_phys) {
  if (g_wt_target == guest_phys && g_wt_handle) { ApplyWatchToAllThreads(); return; }
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const uint32_t pa = guest_phys & ~3u;

  // Watch the address the GUEST actually writes through, not the one we read.
  //
  // Guest memory is mapped at more than one host address: a physical alias
  // (what the scanner reads) and a virtual alias per guest mapping. A hardware
  // watchpoint is a HOST address, so watching the physical alias catches
  // nothing when the write goes through the virtual one - which is exactly what
  // happened here, and is the same trap the video hunt hit earlier tonight.
  //
  // Rather than guess the mapping, try every plausible guest VA for this
  // physical page and keep the ones whose memory reads back identical. Those
  // are aliases of the same bytes. Up to four get a debug register.
  auto* phys_host = memory->TranslatePhysical<uint8_t*>(pa);
  if (!phys_host) return;
  uint32_t ref = 0;
  std::memcpy(&ref, phys_host, 4);

  g_wt_alias_count = 0;
  const uint32_t bases[] = {0x00000000u, 0x40000000u, 0x80000000u,
                            0xA0000000u, 0xC0000000u, 0xE0000000u};
  for (uint32_t b : bases) {
    if (g_wt_alias_count >= 4) break;
    auto* h = memory->TranslateVirtual<uint8_t*>(b | pa);
    if (!h) continue;

    // NEVER dereference a translated pointer without checking it is mapped.
    // TranslateVirtual is just base + offset and returns a pointer for guest
    // addresses that were never committed; reading one of those killed the
    // game outright ("Unhandled guest access violation: read of guest
    // 0x116CAA8C"). VirtualQuery first, and only then touch it.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(h, &mbi, sizeof(mbi))) continue;
    if (mbi.State != MEM_COMMIT) continue;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) continue;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) continue;

    uint32_t v = 0;
    std::memcpy(&v, h, 4);
    if (v != ref) continue;                       // not the same bytes
    const uint64_t hv = reinterpret_cast<uint64_t>(h);
    bool dup = false;
    for (int i = 0; i < g_wt_alias_count; ++i)
      if (g_wt_alias[i] == hv) dup = true;
    if (dup) continue;
    g_wt_alias[g_wt_alias_count++] = hv;
    REXLOG_INFO("[animwriter]   alias: guest VA 0x{:08X} -> host {:016X}", b | pa, hv);
  }
  // Always include the physical alias as a fallback.
  if (g_wt_alias_count < 4) {
    const uint64_t hv = reinterpret_cast<uint64_t>(phys_host);
    bool dup = false;
    for (int i = 0; i < g_wt_alias_count; ++i)
      if (g_wt_alias[i] == hv) dup = true;
    if (!dup) g_wt_alias[g_wt_alias_count++] = hv;
  }

  g_wt_host = g_wt_alias_count ? g_wt_alias[0] : 0;
  g_wt_target = guest_phys;
  if (!g_wt_handle) g_wt_handle = AddVectoredExceptionHandler(1, WatchHandler);
  REXLOG_INFO("[animwriter] watching 0x{:08X} through {} host alias(es)",
              guest_phys, g_wt_alias_count);
  ApplyWatchToAllThreads();
}

}  // namespace

// ---------------------------------------------------------------------------
// [diag] Find the value that is oscillating.
//
// Sampling is exhausted: 97.2% of wall-clock is the fibre switch, identically
// during normal play and during the fault, so the profile cannot separate them.
// But the symptom itself is a measurement - the character bobs up and down
// several times a second with no input, so SOME float in guest memory is
// reversing direction at that rate. Find it and the physics code can be caught
// by trapping writes to it, exactly as the video hunt caught its decoder.
//
// Two phases, because holding 512 MB of samples per tick is not necessary:
//   1. one full copy, 120 ms apart, keeps only addresses that CHANGED and hold
//      a plausible world coordinate - that is a few thousand, not millions.
//   2. those are then watched over 10 ticks and ranked by how many times they
//      REVERSE DIRECTION. A position bobbing in water turns over constantly; a
//      timer or counter climbs and never turns.
namespace {

// Which 4 KB pages of [dst_span) are readable, so a scan can skip the rest.
// Filled by CopyCommitted and consulted before every later read of `base`.
std::vector<uint8_t> committed;

// memcpy that stops at the edge of what is mapped instead of faulting.
// Uncommitted pages are left zeroed in the destination and marked unreadable.
void CopyCommitted(uint8_t* dst, const uint8_t* src, size_t span) {
  committed.assign((span + 0xFFF) >> 12, 0);
  std::memset(dst, 0, span);
  size_t off = 0;
  while (off < span) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(src + off, &mbi, sizeof(mbi)))
      break;
    const size_t region_off = size_t((const uint8_t*)mbi.BaseAddress - src);
    size_t start = off;
    size_t end = region_off + mbi.RegionSize;
    if (end > span) end = span;
    if (end <= start) break;
    const bool readable =
        mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) &&
        !(mbi.Protect & PAGE_GUARD);
    if (readable) {
      std::memcpy(dst + start, src + start, end - start);
      for (size_t p = start >> 12; p < ((end + 0xFFF) >> 12); ++p)
        committed[p] = 1;
    }
    off = end;
  }
}

bool PlausibleCoord(float v) {
  if (!std::isfinite(v)) return false;
  const float a = std::fabs(v);
  return a > 0.01f && a < 200000.0f;
}

void OscillationScanThread() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const uint32_t lo = 0x00020000u, hi = 0x1F000000u;
  const size_t span = hi - lo;

  auto* base = memory->TranslatePhysical<uint8_t*>(lo);
  if (!base) { REXLOG_WARN("[osc] cannot translate guest memory"); return; }

  // quiet unless it finds something
  std::vector<uint8_t> snap(span);
  // Copy only what the host has actually committed.
  //
  // This used to be one blind memcpy of the whole ~502 MB, and it faulted on
  // the first uncommitted page - killing the game about 90 seconds after every
  // launch in v0.5.0-v0.5.2. TranslatePhysical is base + offset and validates
  // nothing: the guest's physical range is reserved, not committed, so most of
  // it is not readable. Same lesson as reading a guest pointer without checking
  // it is mapped, which crashed a live session earlier in this project.
  CopyCommitted(snap.data(), base, span);
  Sleep(120);

  std::vector<uint32_t> cand;
  for (size_t off = 0; off + 4 <= span; off += 4) {
    float a, b;
    std::memcpy(&a, snap.data() + off, 4);
    if (!committed[off >> 12]) continue;   // never read an uncommitted page
    std::memcpy(&b, base + off, 4);
    if (a == b) continue;
    if (!PlausibleCoord(a) || !PlausibleCoord(b)) continue;
    if (std::fabs(b - a) < 0.001f) continue;
    cand.push_back(uint32_t(lo + off));
    if (cand.size() > 400000) break;
  }
  if (cand.empty()) return;
  if (cand.empty()) return;

  // Phase 2: rank by direction reversals - but a value only counts if it keeps
  // BEHAVING like a coordinate the whole time.
  //
  // The first version ranked by raw amplitude and returned nothing but noise:
  // deltas of 1e38 out of what is evidently stack memory read as floats. A
  // character's height is a small number that moves a little each frame, so
  // every sample must stay plausible, and a single frame must not move it more
  // than a body length. Those two constraints are what separate a position
  // from a scratch buffer that happens to change.
  const int kTicks = 12;
  std::vector<float> prev(cand.size()), last_delta(cand.size(), 0.0f);
  std::vector<int> flips(cand.size(), 0);
  std::vector<float> vmin(cand.size(), 0.0f), vmax(cand.size(), 0.0f);
  std::vector<uint8_t> ok(cand.size(), 1);
  for (size_t i = 0; i < cand.size(); ++i) {
    std::memcpy(&prev[i], memory->TranslatePhysical<uint8_t*>(cand[i]), 4);
    vmin[i] = vmax[i] = prev[i];
  }

  for (int t = 0; t < kTicks; ++t) {
    Sleep(60);
    for (size_t i = 0; i < cand.size(); ++i) {
      if (!ok[i]) continue;
      float v;
      std::memcpy(&v, memory->TranslatePhysical<uint8_t*>(cand[i]), 4);
      if (!PlausibleCoord(v)) { ok[i] = 0; continue; }
      const float d = v - prev[i];
      if (std::fabs(d) > 500.0f) { ok[i] = 0; continue; }  // teleporting: not a position
      if (std::fabs(d) > 0.001f) {
        if (last_delta[i] != 0.0f && ((d > 0) != (last_delta[i] > 0)))
          ++flips[i];
        last_delta[i] = d;
      }
      vmin[i] = (std::min)(vmin[i], v);
      vmax[i] = (std::max)(vmax[i], v);
      prev[i] = v;
    }
  }

  std::vector<size_t> idx(cand.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    if (flips[a] != flips[b]) return flips[a] > flips[b];
    return (vmax[a] - vmin[a]) > (vmax[b] - vmin[b]);
  });

  // A real oscillation has a SPAN: it swings through a range and comes back.
  // Noise flips sign without going anywhere.
  auto vspan = [&](size_t i) { return vmax[i] - vmin[i]; };
  int shown = 0;
  {
    int strong = 0;
    for (size_t i = 0; i < flips.size(); ++i)
      if (ok[i] && flips[i] >= 4 && vspan(i) > 0.05f && vspan(i) < 5000.0f) ++strong;
    if (!strong) return;   // nothing is oscillating; say nothing
    REXLOG_INFO("[osc] {} candidates, {} oscillating like a position - top 25:",
                cand.size(), strong);
  }
  for (size_t k = 0; k < idx.size() && shown < 25; ++k) {
    const size_t i = idx[k];
    if (flips[i] < 4) break;
    if (!ok[i] || vspan(i) <= 0.05f || vspan(i) >= 5000.0f) continue;
    float v;
    std::memcpy(&v, memory->TranslatePhysical<uint8_t*>(cand[i]), 4);
    REXLOG_INFO("[osc]   0x{:08X}  flips={}  range {:.3f}..{:.3f} (span {:.3f})  now {:.3f}",
                cand[i], flips[i], vmin[i], vmax[i], vspan(i), v);
    if (shown == 0 && g_wt_count < 8)
      ArmWriteTrap(cand[i]);   // the strongest oscillator gets the trap
    ++shown;
  }

}

}  // namespace

namespace {
void OscillationLoop() {
  Sleep(20000);
  // Publish the guest->host map for the external analyser.
  {
    using Fn = void(__cdecl*)(const char*);
    HMODULE m = GetModuleHandleA("rexruntime.dll");
    auto fn = m ? reinterpret_cast<Fn>(GetProcAddress(m, "Ng2DumpFunctionTableC")) : nullptr;
    if (fn) {
      // Forward slashes on purpose: Windows accepts them, and a backslash in
      // this literal has now been mangled three times tonight - "\n" and "\f"
      // silently became a newline and a formfeed, fopen failed, and the log
      // still cheerfully said the file was written.
      const char* kPath = "C:/ng2dump/functable.txt";
      fn(kPath);
      REXLOG_INFO("[diag] wrote {} for the external analyser", kPath);
    }
  }  // let the game get past boot and into play
  for (;;) {
    OscillationScanThread();
    for (int k = 0; k < 24 && g_wt_count < 8; ++k) {
      if (g_wt_target && g_wt_count < 8) ApplyWatchToAllThreads();
      Sleep(500);
    }
    Sleep(2000);
  }
}
}  // namespace

// --- Projection-matrix finder (NG2_FIND_PROJ) -------------------------------
//
// Finds the guest's perspective PROJECTION matrix in memory and the function
// that builds it, so the field of view can be hooked for FOV / ultrawide - the
// Fable II method, done IN-PROCESS (no cdb, so no risk of a debug register left
// armed with no handler). A perspective projection is a 4x4 of floats that is
// almost all zeros with a +/-1 in the w-projection slot; among several on
// screen at once (camera, shadows, reflections) the camera's has the largest
// far plane. Once found, the same hardware write trap the oscillation hunt uses
// logs the writing guest function.
namespace {

// Read a big-endian float from guest memory (Xbox 360 is big-endian; NG2's own
// scan tools search for big-endian values).
inline float BeF32(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = _byteswap_ulong(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// True if the 16 floats (row-major) are a D3D perspective projection; fills the
// vertical field of view and the clip planes.
bool PerspectiveFromRowMajor(const float m[16], float& fovy_deg, float& znear,
                             float& zfar) {
  auto z = [](float v) { return std::fabs(v) < 1e-4f; };
  if (!(z(m[1]) && z(m[2]) && z(m[3]) && z(m[4]) && z(m[6]) && z(m[7]) &&
        z(m[8]) && z(m[9]) && z(m[12]) && z(m[13]) && z(m[15])))
    return false;
  if (std::fabs(std::fabs(m[11]) - 1.0f) > 0.01f) return false;  // w-proj = +/-1
  const float xs = m[0], ys = m[5], Q = m[10], zt = m[14];
  if (!(xs > 0.1f && xs < 20.0f) || !(ys > 0.1f && ys < 20.0f)) return false;
  if (!std::isfinite(Q) || !std::isfinite(zt) || std::fabs(Q) < 1e-4f) return false;
  const float zn = -zt / Q;              // LH: Q=zf/(zf-zn), zt=-zn*Q
  const float denom = Q - 1.0f;
  if (std::fabs(denom) < 1e-4f) return false;
  const float zf = zn * Q / denom;
  if (!(zn > 0.001f && zn < 50.0f) || !(zf > zn && zf < 1.0e7f)) return false;
  fovy_deg = 2.0f * std::atan(1.0f / ys) * 57.2957795f;
  if (!(fovy_deg > 10.0f && fovy_deg < 170.0f)) return false;
  znear = zn;
  zfar = zf;
  return true;
}

struct ProjCand {
  uint32_t addr;
  float fovy, zn, zf;
};

void FindProjectionThread() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  REXLOG_INFO("[proj] projection-matrix finder armed (NG2_FIND_PROJ)");
  const uint32_t lo = 0x00020000u, hi = 0x1F000000u;
  const size_t span = hi - lo;
  auto* base = memory->TranslatePhysical<uint8_t*>(lo);
  if (!base) { REXLOG_WARN("[proj] cannot translate guest memory"); return; }
  std::vector<uint8_t> snap(span);

  for (int attempt = 0; attempt < 90; ++attempt) {  // keep trying until a 3D scene
    Sleep(attempt == 0 ? 8000 : 3000);  // let a 3D scene render first
    CopyCommitted(snap.data(), base, span);

    // Row-major or its transpose (column-major storage) match a perspective.
    auto try_mats = [](const float m[16], float& fovy, float& zn, float& zf) {
      if (PerspectiveFromRowMajor(m, fovy, zn, zf)) return true;
      float t[16];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[r * 4 + c] = m[c * 4 + r];
      return PerspectiveFromRowMajor(t, fovy, zn, zf);
    };
    auto near1 = [](float v) { return std::fabs(std::fabs(v) - 1.0f) < 0.01f; };

    std::vector<ProjCand> found;
    for (size_t off = 0; off + 64 <= span; off += 4) {
      if (!committed[off >> 12]) continue;
      // Cheap reject: the w-projection term (row-major m11 at +44, column-major
      // m14 at +56) must be ~+/-1, in either endianness. snap is our own buffer,
      // safe to read past a page edge (uncommitted pages are zero).
      const uint8_t* q = snap.data() + off;
      float r44, r56;
      std::memcpy(&r44, q + 44, 4);
      std::memcpy(&r56, q + 56, 4);
      if (!(near1(r44) || near1(r56) || near1(BeF32(q + 44)) || near1(BeF32(q + 56))))
        continue;
      float mbe[16], mle[16];
      for (int i = 0; i < 16; ++i) {
        mbe[i] = BeF32(q + i * 4);
        std::memcpy(&mle[i], q + i * 4, 4);
      }
      float fovy, zn, zf;
      if (!try_mats(mbe, fovy, zn, zf) && !try_mats(mle, fovy, zn, zf)) continue;
      found.push_back({uint32_t(lo + off), fovy, zn, zf});
      if (found.size() > 4000) break;
    }

    if (found.empty()) {
      // Diagnostic: the strict check matched nothing, so dump the blocks that
      // LOOK like a projection (mostly zeros, one +/-1, a couple of scales),
      // read big-endian, so the real layout is visible in the log and the
      // strict check can be calibrated.
      static int dbg = 0;
      for (size_t off = 0; off + 64 <= span && dbg < 24; off += 4) {
        if (!committed[off >> 12]) continue;
        const uint8_t* q = snap.data() + off;
        float m[16];
        int zeros = 0, ones = 0, scales = 0;
        for (int i = 0; i < 16; ++i) {
          m[i] = BeF32(q + i * 4);
          const float a = std::fabs(m[i]);
          if (!std::isfinite(m[i])) { scales = -100; break; }
          if (a < 1e-4f) ++zeros;
          else if (std::fabs(a - 1.0f) < 0.01f) ++ones;
          else if (a > 0.05f && a < 1.0e6f) ++scales;
        }
        if (zeros >= 8 && ones >= 1 && scales >= 2) {
          REXLOG_INFO("[proj-dbg] 0x{:08X} BE: {:.3f} {:.3f} {:.3f} {:.3f} | "
                      "{:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f} "
                      "| {:.3f} {:.3f} {:.3f} {:.3f}",
                      uint32_t(lo + off), m[0], m[1], m[2], m[3], m[4], m[5],
                      m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
                      m[15]);
          ++dbg;
        }
      }
      REXLOG_INFO("[proj] attempt {}: no perspective matrix yet (in a 3D scene?)",
                  attempt);
      continue;
    }
    std::sort(found.begin(), found.end(),
              [](const ProjCand& a, const ProjCand& b) { return a.zf > b.zf; });
    REXLOG_INFO("[proj] attempt {}: {} perspective matrices; distinct ones:",
                attempt, found.size());
    int shown = 0;
    float last_fov = -1.0f, last_zf = -1.0f;
    for (const auto& c : found) {
      if (std::fabs(c.fovy - last_fov) < 0.2f && std::fabs(c.zf - last_zf) < 1.0f)
        continue;  // collapse identical copies
      last_fov = c.fovy;
      last_zf = c.zf;
      if (shown++ >= 12) break;
      REXLOG_INFO("[proj]   0x{:08X}  fovy={:.1f}deg  near={:.3f}  far={:.1f}",
                  c.addr, c.fovy, c.zn, c.zf);
    }
    const ProjCand cam = found.front();  // biggest far plane = world camera
    REXLOG_INFO("[proj] CAMERA candidate 0x{:08X} (fovy {:.1f}, near {:.3f}, "
                "far {:.1f})", cam.addr, cam.fovy, cam.zn, cam.zf);
    // Arming a hardware write trap on a live 3D scene sets debug registers on
    // every thread; that is the riskier half, so it only runs when explicitly
    // asked. The scan above is pure memory reads and cannot destabilise a stage.
    if (const char* t = std::getenv("NG2_TRAP_PROJ"); t && *t) {
      REXLOG_INFO("[proj] NG2_TRAP_PROJ set - arming a write trap to find the builder");
      ArmWriteTrap(cam.addr);  // watches m[0]; the VEH logs [animwriter] guest_fn
      for (int k = 0; k < 40 && g_wt_count < 4; ++k) {
        if (g_wt_target) ApplyWatchToAllThreads();  // new threads come up unarmed
        Sleep(250);
      }
      REXLOG_INFO("[proj] done: {} writer(s) logged above as [animwriter] guest_fn",
                  g_wt_count);
    } else {
      REXLOG_INFO("[proj] scan only; set NG2_TRAP_PROJ=1 to also trace the builder");
    }
    return;
  }
  REXLOG_WARN("[proj] gave up: no perspective matrix seen");
}

}  // namespace

namespace ng2 {
void ScanOscillating() { std::thread(OscillationScanThread).detach(); }
void FindProjection() { std::thread(FindProjectionThread).detach(); }
void StartOscillationLoop() {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true))
    std::thread(OscillationLoop).detach();
}
}  // namespace ng2

void ng2DiagEnterSub83844348(PPCRegister& r3) {
  ++g_enter_count;
  REXLOG_INFO("[diag] sub_83844348 enter #{}  this=0x{:08X}", g_enter_count,
              r3.u32);
}

void ng2DiagParserEntry(PPCRegister& r24) {
  uint32_t ptr = 0, len = 0;
  const bool ok = ReadGuestU32(r24.u32, ptr) && ReadGuestU32(r24.u32 + 4, len);
  REXLOG_INFO("[diag] parser ENTRY: cursor=0x{:08X} stream={} len={}", r24.u32,
              ok ? fmt::format("0x{:08X}", ptr) : "<unreadable>",
              ok ? fmt::format("{}", len) : "?");
}

void ng2DiagIndirectCallSite(PPCRegister& r30, PPCRegister& r25,
                             PPCRegister& r24) {
  const uint32_t self = r30.u32;

  // sub_83844348 is a byte-stream parser: this->field_18 is a {pointer,length}
  // cursor it reads through. The indirect call below only happens when the
  // parsed index r25 falls outside 0..3, so log the cursor and the bytes it is
  // walking - if those are wrong, the fault is upstream of this function.
  uint32_t cur_ptr = 0, cur_len = 0;
  const bool got_cursor = ReadGuestU32(r24.u32, cur_ptr) &&
                          ReadGuestU32(r24.u32 + 4, cur_len);
  REXLOG_INFO(
      "[diag] parser: r25(index)={} (0x{:08X})  r24(cursor)=0x{:08X}  "
      "stream={} len={}",
      int32_t(r25.u32), r25.u32, r24.u32,
      got_cursor ? fmt::format("0x{:08X}", cur_ptr) : "<unreadable>",
      got_cursor ? fmt::format("{}", cur_len) : "?");
  if (got_cursor) {
    for (uint32_t row = 0; row < 0x20; row += 0x10) {
      uint32_t w[4] = {};
      bool ok = true;
      for (int i = 0; i < 4; ++i)
        ok = ok && ReadGuestU32(cur_ptr + row + i * 4, w[i]);
      if (!ok)
        break;
      REXLOG_INFO("[diag]   stream 0x{:08X}: {:08X} {:08X} {:08X} {:08X}",
                  cur_ptr + row, w[0], w[1], w[2], w[3]);
    }
  }
  uint32_t field0 = 0, target = 0;
  const bool got_field0 = ReadGuestU32(self, field0);
  const bool got_target = got_field0 && ReadGuestU32(field0, target);

  REXLOG_INFO(
      "[diag] indirect call site: this=0x{:08X}  [this+0]={}  [[this+0]]={}",
      self,
      got_field0 ? fmt::format("0x{:08X}", field0) : "<unreadable>",
      got_target ? fmt::format("0x{:08X}", target) : "<unreadable>");

  // [this+0] resolves to this+0x1E0, i.e. a handler struct embedded in the
  // object itself. Dump it: an untouched fill pattern means nothing ever
  // initialised it, whereas plausible values mean we are looking at the wrong
  // field.
  if (got_field0) {
    for (uint32_t row = 0; row < 0x40; row += 0x10) {
      uint32_t w[4] = {};
      bool ok = true;
      for (int i = 0; i < 4; ++i)
        ok = ok && ReadGuestU32(field0 + row + i * 4, w[i]);
      if (!ok)
        break;
      REXLOG_INFO("[diag]   0x{:08X}: {:08X} {:08X} {:08X} {:08X}",
                  field0 + row, w[0], w[1], w[2], w[3]);
    }
  }
}

// Which chapter is loading, read from the file the game opens for it.
//
// The game announces this itself: loading chapter N opens s_chap_NN.ng2. What
// makes it slightly more than a substring match is that the load is followed
// IMMEDIATELY by an enumeration of every other chapter file, in the same
// millisecond:
//
//   14:04:09  s_chap_12.ng2     <- the chapter actually being loaded
//   14:04:09  s_chap_01 .. 11, 13, 14   <- a scan of the rest
//
// So the answer is the FIRST file of a burst, not the last one seen. Taking the
// last would have reported chapter 14 every single time - and reported it
// confidently, which is worse than not knowing.
std::atomic<int> g_current_chapter{0};

void NoteChapterFile(const std::string& path) {
  const auto at = path.find("s_chap_");
  if (at == std::string::npos)
    return;
  const std::string digits = path.substr(at + 7, 2);
  if (digits.size() < 2 || !isdigit((unsigned char)digits[0]) ||
      !isdigit((unsigned char)digits[1])) {
    return;
  }
  const int chapter = std::atoi(digits.c_str());

  using clock = std::chrono::steady_clock;
  static clock::time_point last{};
  static bool have_last = false;
  const auto now = clock::now();
  const bool new_burst =
      !have_last || std::chrono::duration<double>(now - last).count() > 1.0;
  last = now;
  have_last = true;
  if (!new_burst)
    return;  // still the enumeration that follows the real load

  const int previous = g_current_chapter.exchange(chapter);
  if (previous != chapter) {
    REXLOG_INFO("[diag] chapter {} is loading (from {})", chapter, path);
    // The GPU plugin keeps the texture pack and cannot see app state, so the
    // chapter reaches it the same way every other setting does. It uses this
    // to record which textures a stage actually uses, and to warm those files
    // before the stage starts rather than during it.
    if (rex::cvar::GetFlagInfo("texture_pack_chapter")) {
      rex::cvar::SetFlagByName("texture_pack_chapter", std::to_string(chapter));
    }
  }
}

namespace ng2 {
int CurrentChapter() { return g_current_chapter.load(std::memory_order_relaxed); }
}  // namespace ng2

void ng2DiagFileOpen(PPCRegister& r3) {
  ++g_open_count;
  const std::string path = ReadGuestString(r3.u32);
  REXLOG_INFO("[diag] open #{}: {}", g_open_count, path);

  // A chapter is starting: arm the cinematic auto-skip.
  //
  // This hook already sees every file the guest opens, so no new hook and no
  // reverse engineering is needed to know when a chapter loads - the story data
  // opening IS the event. Arming on the file rather than on a timer is what
  // keeps the synthetic presses off the pad during normal play.
  //
  // The trigger is s_chap_NN.ng2, which is what this title actually calls its
  // per-chapter story data - checked against the disc rather than guessed. An
  // earlier revision armed on "stryd"/"ng2stry", names that appear nowhere in
  // the game data, so the feature could never fire: every part of it was wired
  // correctly to a file that does not exist. None of the files opened during
  // boot match this, which is what keeps the arming to a chapter load.
  if (path.find("s_chap_") != std::string::npos) {
    ng2::ArmAutoSkip();
    NoteChapterFile(path);
  }
}

void ng2DiagContextSwitch(PPCRegister&) {
  ++g_switch_count;
}

// Runs once per frame. REX_NG2_DUMP_EVERY=N dumps the front resolve target
// every N frames (up to REX_NG2_DUMP_MAX files) so one run yields a series
// covering the whole boot, rather than needing a lucky single frame number.
// Trigger a RenderDoc capture on a chosen frame.
//
// The intro video can only be diagnosed by looking at the actual draws, and
// RenderDoc's capture key is not reliable to drive unattended - a synthetic
// key event does not reach the injected hook. The in-application API is
// deterministic instead: when the process is launched under
// `renderdoccmd capture`, renderdoc.dll is already loaded in this process, so
// we just ask it for the API and trigger on the frame we want.
//
// Does nothing at all when not running under RenderDoc.
//   REX_NG2_RDOC_FRAME=<n>   capture this frame (0/unset = never)
void MaybeTriggerRenderDocCapture(int frame) {
  // Time-based rather than frame-based: the frame rate through the front end
  // is not known ahead of time, and the intro is a wall-clock event (it starts
  // roughly 21s after launch and runs for 20s), so seconds are what we can
  // actually aim with.
  //   REX_NG2_RDOC_AT_MS=<ms>  capture the first frame after this point
  static const long long want_ms = [] {
    const char* e = std::getenv("REX_NG2_RDOC_AT_MS");
    return e ? std::atoll(e) : 0LL;
  }();
  if (want_ms <= 0)
    return;
  static const auto t0 = GetTickCount64();
  static bool fired = false;
  if (fired || static_cast<long long>(GetTickCount64() - t0) < want_ms)
    return;
  fired = true;

  static RENDERDOC_API_1_1_2* api = [] () -> RENDERDOC_API_1_1_2* {
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (!mod) {
      REXLOG_WARN("[diag] REX_NG2_RDOC_FRAME set but renderdoc.dll is not "
                  "loaded - launch under 'renderdoccmd capture'");
      return nullptr;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(mod, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_1_2* out = nullptr;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2,
                             reinterpret_cast<void**>(&out))) {
      REXLOG_WARN("[diag] RENDERDOC_GetAPI failed");
      return nullptr;
    }
    return out;
  }();

  if (!api)
    return;
  api->TriggerCapture();
  REXLOG_INFO("[diag] RenderDoc capture triggered at frame {}", frame);
}

// Dump an arbitrary physical guest range once, at a chosen moment.
//
// The video decoder writes its Y/U/V planes into guest memory and the GPU
// plugin then untiles them into host textures. Dumping the guest side answers
// the only question that matters: did the decoder produce a good picture that
// something later mangles, or was it already wrong in memory?
//
//   REX_NG2_PLANE_ADDR=0BD16000   physical address, hex, no 0x
//   REX_NG2_PLANE_BYTES=311296
//   REX_NG2_PLANE_AT_MS=12000
//   REX_NG2_PLANE_OUT=out/plane.bin
void MaybeDumpGuestPlane() {
  static const uint32_t addr = [] {
    const char* e = std::getenv("REX_NG2_PLANE_ADDR");
    return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
  }();
  static const size_t bytes = [] {
    const char* e = std::getenv("REX_NG2_PLANE_BYTES");
    return e ? size_t(std::strtoul(e, nullptr, 10)) : 0u;
  }();
  static const long long at_ms = [] {
    const char* e = std::getenv("REX_NG2_PLANE_AT_MS");
    return e ? std::atoll(e) : 0LL;
  }();
  if (!addr || !bytes || at_ms <= 0)
    return;
  static const auto t0 = GetTickCount64();
  static bool done = false;
  if (done || static_cast<long long>(GetTickCount64() - t0) < at_ms)
    return;
  done = true;

  auto* memory = REX_KERNEL_MEMORY();
  if (!memory)
    return;
  auto* src = memory->TranslatePhysical<const uint8_t*>(addr);
  if (!src) {
    REXLOG_WARN("[diag] plane dump: physical 0x{:08X} does not translate", addr);
    return;
  }
  const char* out = std::getenv("REX_NG2_PLANE_OUT");
  if (!out)
    out = "out/plane.bin";
  if (FILE* f = std::fopen(out, "wb")) {
    std::fwrite(src, 1, bytes, f);
    std::fclose(f);
    REXLOG_INFO("[diag] dumped {} bytes of guest physical 0x{:08X} to {}",
                bytes, addr, out);
  }
}

// Host-side video injection.
//
// The title's VC-1 decode shaders produce corrupt Y/U/V planes under this GPU
// backend (verified by dumping the planes out of guest memory - they are fully
// written but macroblock-scrambled). Rather than fix a decoder we do not have
// the source to, overwrite its output: the plugin's texture-load path reads
// these planes correctly, so correct planes in memory means a correct picture.
//
// Layout, from a RenderDoc capture of the intro frame - three streams, one per
// panel of the triptych, evenly spaced, each holding V, U and Y:
//
//   stream base  = kInjectBase - n * 0x29F000
//     V at +0x00000   256 pitch, 160x290
//     U at +0x13000   256 pitch, 160x290
//     Y at +0x26000   512 pitch, 320x580
//
//   REX_NG2_INJECT=1              write a test pattern (proves the path)
//   REX_NG2_INJECT_AT_MS=<ms>     start injecting at this point
constexpr uint32_t kInjectStream0 = 0x0BCF0000;  // V of the first stream
constexpr uint32_t kInjectStride  = 0x29F000;
constexpr uint32_t kOffU = 0x13000, kOffY = 0x26000;
constexpr uint32_t kInjectBufStride = 0x6F000;  // triple buffering
constexpr uint32_t kYPitch = 512, kYW = 320, kYH = 580;
constexpr uint32_t kCPitch = 256, kCW = 160, kCH = 290;

void WritePlane(uint32_t phys, uint32_t pitch, uint32_t w, uint32_t h,
                const uint8_t* rows) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  auto* dst = memory->TranslatePhysical<uint8_t*>(phys);
  if (!dst) return;
  for (uint32_t y = 0; y < h; ++y)
    std::memcpy(dst + size_t(y) * pitch, rows + size_t(y) * w, w);
}

// Writing once per guest frame loses the race about half the time: the decode
// is GPU work that lands after the CPU-side hook runs, and there is no guest
// instruction between the decode and the texture-load that reads it. A
// dedicated thread rewriting the planes continuously wins far more often.
void InjectorThread(uint32_t stream);
void RedirectorThread();

void MaybeInjectVideo() {
  static const bool on = [] {
    const char* e = std::getenv("REX_NG2_INJECT");
    return e && std::atoi(e) != 0;
  }();
  static const long long at_ms = [] {
    const char* e = std::getenv("REX_NG2_INJECT_AT_MS");
    return e ? std::atoll(e) : 1000LL;
  }();
  if (!on) return;
  static const auto t0 = GetTickCount64();
  if (static_cast<long long>(GetTickCount64() - t0) < at_ms) return;

  // Each stream is triple buffered - three plane sets 0x6F000 apart, found by
  // scanning guest memory for the luma signature (pitch 512, columns 320-511
  // all zero). The composite samples whichever set the decoder last filled, so
  // every buffer has to be written or the injection loses the race.
  // One writer cycles all nine buffer sets in ~4ms, which only gives a handful
  // of chances per frame to be the last writer before the texture-load reads.
  // One thread per stream cuts the cycle time and raises the odds.
  // Redirect the decode away from the planes we inject into. The resolve
  // destination lives in the GPU command ring (found by scanning for a plane
  // address: 32+ occurrences at a 0xC600 stride around 0x1EEC0BAC). Rewriting
  // it there is a CPU-vs-CPU race against the guest building the buffer, not
  // the CPU-vs-GPU race that capped plain injection at ~58%.
  static std::thread redirector([] { RedirectorThread(); });
  (void)redirector;

  static std::vector<std::thread>* workers = [] {
    auto* v = new std::vector<std::thread>();
    for (uint32_t i = 0; i < 3; ++i)
      v->emplace_back([i] { InjectorThread(i); });
    return v;
  }();
  (void)workers;
}

void InjectStream(uint32_t i, const uint8_t* luma, const uint8_t* chroma) {
  {
    for (uint32_t b = 0; b < 3; ++b) {
      const uint32_t y = (kInjectStream0 + kOffY) - i * kInjectStride
                       - b * kInjectBufStride;
      WritePlane(y,            kYPitch, kYW, kYH, luma);
      WritePlane(y - kOffU,    kCPitch, kCW, kCH, chroma);  // U
      WritePlane(y - kOffY,    kCPitch, kCW, kCH, chroma);  // V
    }
  }
}

// Scan the command ring for any of the nine plane addresses and point the
// resolve at a single sacrificial buffer instead, leaving the other eight for
// the injector to own.
constexpr uint32_t kRingFrom = 0x1EE00000, kRingTo = 0x1F000000;
constexpr uint32_t kSacrificial = 0x0B768000;

void RedirectorThread() {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  std::vector<uint32_t> targets;
  for (uint32_t i = 0; i < 3; ++i)
    for (uint32_t b = 0; b < 3; ++b) {
      const uint32_t y = (kInjectStream0 + kOffY) - i * kInjectStride
                       - b * kInjectBufStride;
      if (y != kSacrificial) targets.push_back(y);
    }
  const uint8_t rep[4] = {uint8_t(kSacrificial >> 24), uint8_t(kSacrificial >> 16),
                          uint8_t(kSacrificial >> 8), uint8_t(kSacrificial)};
  for (;;) {
    for (uint32_t a = kRingFrom; a + 4 <= kRingTo; a += 4) {
      auto* q = memory->TranslatePhysical<uint8_t*>(a);
      if (!q) continue;
      const uint32_t v = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                         (uint32_t(q[2]) << 8) | uint32_t(q[3]);
      for (uint32_t t : targets)
        if (v == t) { std::memcpy(q, rep, 4); break; }
    }
  }
}

void InjectorThread(uint32_t stream) {
  std::vector<uint8_t> luma(size_t(kYW) * kYH);
  for (uint32_t y = 0; y < kYH; ++y)
    std::memset(luma.data() + size_t(y) * kYW, int(y * 255 / (kYH - 1)), kYW);
  const std::vector<uint8_t> chroma(size_t(kCW) * kCH, 128);
  for (;;)
    InjectStream(stream, luma.data(), chroma.data());
}

// Find where a 32-bit big-endian value appears in guest physical memory.
//
// The resolve that writes the video planes takes its destination from
// RB_COPY_DEST_BASE (register 0x2319), which the guest sets through a PM4
// packet it builds in memory. So the plane address must appear somewhere in a
// command buffer - locating it is the first step to redirecting the decode
// away from the planes we want to own.
//
//   REX_NG2_SCAN_VALUE=0BD16000   value to hunt for (hex)
//   REX_NG2_SCAN_FROM=00100000    physical range to search (hex)
//   REX_NG2_SCAN_TO=20000000
//   REX_NG2_SCAN_AT_MS=12000
void MaybeScanForValue() {
  static const uint32_t want = [] {
    const char* e = std::getenv("REX_NG2_SCAN_VALUE");
    return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u;
  }();
  static const long long at_ms = [] {
    const char* e = std::getenv("REX_NG2_SCAN_AT_MS");
    return e ? std::atoll(e) : 0LL;
  }();
  if (!want || at_ms <= 0) return;
  static const auto t0 = GetTickCount64();
  static bool done = false;
  if (done || static_cast<long long>(GetTickCount64() - t0) < at_ms) return;
  done = true;

  const uint32_t from = [] {
    const char* e = std::getenv("REX_NG2_SCAN_FROM");
    return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0x00100000u;
  }();
  const uint32_t to = [] {
    const char* e = std::getenv("REX_NG2_SCAN_TO");
    return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0x20000000u;
  }();

  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  const uint8_t be[4] = {uint8_t(want >> 24), uint8_t(want >> 16),
                         uint8_t(want >> 8), uint8_t(want)};
  int found = 0;
  for (uint32_t a = from; a + 4 <= to && found < 32; a += 4) {
    auto* p = memory->TranslatePhysical<const uint8_t*>(a);
    if (!p) continue;
    if (p[0] == be[0] && p[1] == be[1] && p[2] == be[2] && p[3] == be[3]) {
      REXLOG_INFO("[diag] scan: 0x{:08X} found at physical 0x{:08X}", want, a);
      ++found;
    }
  }
  REXLOG_INFO("[diag] scan complete: {} occurrences of 0x{:08X} in 0x{:08X}-0x{:08X}",
              found, want, from, to);
}

// Report the guest frame rate every few seconds - and, more usefully, how
// EVENLY the frames arrive.
//
// The average alone is close to useless for the thing people actually notice.
// A second holding 59 frames at 16.7ms and one frame at 200ms still reports
// "60 fps", and that single frame is exactly the hitch being complained about.
// Worse, GetTickCount64 - which this used - has a ~15.6ms resolution, so it
// could not have measured a frame time even in principle.
//
// So: steady_clock per frame, and report the median, the 99th percentile, the
// worst frame, and how many frames took more than twice the median. That last
// number is the one to watch when changing the texture cache size: a texture
// evicted and re-uploaded costs one long frame, not a lower average.
void ReportFrameRate() {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_frame_at = clock::now();
  static clock::time_point window_start = last_frame_at;
  // Fixed capacity: this runs per frame and must not allocate. 5s at 300fps.
  static std::array<float, 1536> ms{};
  static size_t count = 0;

  const auto now = clock::now();
  const float dt =
      std::chrono::duration<float, std::milli>(now - last_frame_at).count();
  last_frame_at = now;
  if (count < ms.size())
    ms[count++] = dt;

  if (std::chrono::duration<double>(now - window_start).count() < 5.0)
    return;
  const double secs = std::chrono::duration<double>(now - window_start).count();
  window_start = now;
  if (count < 2) { count = 0; return; }

  std::array<float, 1536> sorted = ms;
  std::sort(sorted.begin(), sorted.begin() + count);
  const float p50 = sorted[count / 2];
  const float p99 = sorted[(count * 99) / 100];
  const float worst = sorted[count - 1];
  int hitches = 0;
  for (size_t i = 0; i < count; ++i)
    if (ms[i] > p50 * 2.0f) ++hitches;

  REXLOG_INFO("[diag] {:.1f} fps ({} frames in {:.1f}s)  frame ms: p50 {:.1f}  "
              "p99 {:.1f}  worst {:.1f}  hitches {}",
              double(count) / secs, count, secs, p50, p99, worst, hitches);
  count = 0;
}

void ng2DiagFrameTick() {
  ++g_frame;
  ReportFrameRate();
  MaybeTriggerRenderDocCapture(g_frame);
  MaybeDumpGuestPlane();
  MaybeScanForValue();
  MaybeInjectVideo();
  static const int every = [] {
    const char* e = std::getenv("REX_NG2_DUMP_EVERY");
    return e ? std::atoi(e) : 0;
  }();
  static const int max_files = [] {
    const char* e = std::getenv("REX_NG2_DUMP_MAX");
    return e ? std::atoi(e) : 12;
  }();
  static int written = 0;
  if (every <= 0 || (g_frame % every) != 0 || written >= max_files)
    return;
  char path[64];
  std::snprintf(path, sizeof(path), "out/f%04d_A.bin", g_frame);
  DumpPhysical(kResolveA, path);
  std::snprintf(path, sizeof(path), "out/f%04d_B.bin", g_frame);
  DumpPhysical(kResolveB, path);
  ++written;
  REXLOG_INFO("[diag] dumped resolve targets at frame {}", g_frame);
}
