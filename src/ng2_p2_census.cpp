// P2 ATTRIBUTION CENSUS, guest side. See ng2_p2_census.h.
#include "ng2_p2_census.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace ng2::p2 {
namespace {

struct Rec {
  uint32_t frame;
  uint32_t tid;
  uint16_t hook;
  uint16_t flags;   // bit 0: exit matched a different hook than the stack top (nesting mismatch)
  uint32_t cur_in, cur_out;     // device+cursor at entry / exit (guest virtual addresses as the library holds them)
  uint32_t cur2_in, cur2_out;   // device+cursor2 (the +0x3484 copy)
};
static_assert(sizeof(Rec) == 28, "record layout is the file format");

struct Open {
  uint16_t hook;
  uint32_t cur_in, cur2_in;
};

bool g_on = false;
bool g_discover = false;
uint32_t g_start = 0, g_count = 0;
uint32_t g_cursor_off = 0x30, g_cursor2_off = 0x3484;
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_device{0};
std::mutex g_mu;
std::vector<Rec> g_recs;
bool g_written = false;
// LAST WRITER per 64-byte block of physical memory, kept from the first frame on (not only the packet window):
// the game records small command buffers once and replays them every frame through INDIRECT_BUFFER (Chapter 1:
// 1128 of 1759 draws per frame from a 184-byte buffer recorded before any 60-frame window), so a packet's
// producer is the last hooked call that wrote its address, whenever that was.
struct Writer { uint32_t frame; uint16_t hook; uint16_t pad; };
std::unordered_map<uint32_t, Writer> g_blocks;   // key = physical address >> 6
uint32_t g_discover_hi_logged = 0;
std::atomic<uint32_t> g_discover_logged{0};
std::atomic<uint64_t> g_enters{0}, g_exits{0}, g_mismatch{0}, g_overflow{0}, g_noread{0};
thread_local Open t_stack[32];
thread_local int t_depth = 0;

bool SafeRead32(const uint8_t* p, uint32_t* out) {
  __try {
    *out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ReadGuest(uint32_t va, uint32_t* out) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return false;
  const uint8_t* p = ks->memory()->TranslateVirtual<const uint8_t*>(va);
  if (!p) return false;
  return SafeRead32(p, out);
}

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  const char* e = std::getenv("NG2_P2");
  if (!e || !*e) return;
  unsigned a = 0, b = 0;
  if (std::sscanf(e, "%u:%u", &a, &b) != 2 || !b) {
    REXLOG_INFO("[p2] NG2_P2='{}' is not <first frame>:<frames> - census off", e);
    return;
  }
  g_start = a;
  g_count = b;
  if (const char* c = std::getenv("NG2_P2_CURSOR"); c && *c) g_cursor_off = uint32_t(std::strtoul(c, nullptr, 0));
  if (const char* c = std::getenv("NG2_P2_CURSOR2"); c && *c) g_cursor2_off = uint32_t(std::strtoul(c, nullptr, 0));
  g_discover = std::getenv("NG2_P2_DISCOVER") != nullptr;
  g_recs.reserve(1u << 20);
  g_on = true;
  REXLOG_INFO("[p2] census ON: frames {}..{} (by the swap entry point), cursor device+0x{:X} and +0x{:X}{}", g_start,
              g_start + g_count - 1, g_cursor_off, g_cursor2_off, g_discover ? ", discover mode" : "");
}

bool Recording() {
  const uint32_t f = g_frame.load(std::memory_order_relaxed);
  return f >= g_start && f < g_start + g_count;
}

void Discover(int hook, uint32_t dev) {
  const uint32_t n = g_discover_logged.fetch_add(1);
  if (n >= 48) return;
  char line[600];
  int k = std::snprintf(line, sizeof(line), "[p2] discover hook %d dev %08X:", hook, dev);
  for (uint32_t i = 0; i < 32 && k > 0 && k < int(sizeof(line)) - 12; ++i) {
    uint32_t v = 0;
    const bool ok = ReadGuest(dev + i * 4, &v);
    k += std::snprintf(line + k, sizeof(line) - k, ok ? " %08X" : " ????????", v);
  }
  uint32_t v2 = 0;
  const bool ok2 = ReadGuest(dev + g_cursor2_off, &v2);
  std::snprintf(line + k, sizeof(line) - k, " | +0x%X=%08X%s", g_cursor2_off, v2, ok2 ? "" : "(unreadable)");
  REXLOG_INFO("{}", line);
  if (n < 8) {
    // The second cursor candidates: the +0x3400 area (Fable's +0x3484 copy) - 64 words, for finding NG2's field.
    char hi[800];
    int m = std::snprintf(hi, sizeof(hi), "[p2] discover hook %d dev+0x3400:", hook);
    for (uint32_t i = 0; i < 64 && m > 0 && m < int(sizeof(hi)) - 12; ++i) {
      uint32_t v = 0;
      const bool ok = ReadGuest(dev + 0x3400 + i * 4, &v);
      m += std::snprintf(hi + m, sizeof(hi) - m, ok ? " %08X" : " ????????", v);
    }
    REXLOG_INFO("{}", hi);
  }
}

void Write() {
  if (g_written) return;
  g_written = true;
  std::vector<Rec> recs;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    recs.swap(g_recs);
  }
  FILE* f = std::fopen("p2_guest.bin", "wb");
  if (!f) {
    REXLOG_INFO("[p2] COULD NOT OPEN p2_guest.bin - {} records lost", recs.size());
    return;
  }
  const uint32_t header[8] = {0x3250474E /* 'NGP2' */, 2, uint32_t(sizeof(Rec)), g_start, g_count, g_cursor_off,
                              g_cursor2_off, uint32_t(recs.size())};
  std::fwrite(header, sizeof(header), 1, f);
  std::fwrite(recs.data(), sizeof(Rec), recs.size(), f);
  // Section 2 (format version 2): the LAST WRITER of every 64-byte block any hooked call wrote since the first
  // frame - {u32 block (physical >> 6), u32 frame, u16 hook, u16 0}, after a {u32 'BLK1', u32 count} header.
  std::vector<uint32_t> blocks;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    blocks.reserve(g_blocks.size() * 3);
    for (const auto& kv : g_blocks) {
      blocks.push_back(kv.first);
      blocks.push_back(kv.second.frame);
      blocks.push_back(uint32_t(kv.second.hook));
    }
  }
  const uint32_t bhdr[2] = {0x314B4C42u /* 'BLK1' */, uint32_t(blocks.size() / 3)};
  std::fwrite(bhdr, sizeof(bhdr), 1, f);
  std::fwrite(blocks.data(), sizeof(uint32_t), blocks.size(), f);
  std::fclose(f);
  REXLOG_INFO("[p2] wrote p2_guest.bin: {} call ranges for frames {}..{} (enters {}, exits {}, nesting mismatches {}, "
              "stack overflows {}, unreadable cursors {}); LAST WRITER map {} blocks of 64 bytes since frame 1",
              recs.size(), g_start, g_start + g_count - 1, g_enters.load(), g_exits.load(), g_mismatch.load(),
              g_overflow.load(), g_noread.load(), blocks.size() / 3);
}

}  // namespace

void Enter(int hook, uint32_t r3) {
  Init();
  if (!g_on) return;
  const uint32_t dev = g_device.load(std::memory_order_relaxed);
  if (!dev) return;
  (void)r3;   // the device comes from the frame marker; r3 is not the device for every hooked site
  if (g_discover) Discover(hook, dev);
  // Every call from the first frame on feeds the last-writer map; the per-call records only inside the window.
  if (Recording()) g_enters.fetch_add(1, std::memory_order_relaxed);
  if (t_depth >= 32) {
    g_overflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Open& o = t_stack[t_depth++];
  o.hook = uint16_t(hook);
  if (!ReadGuest(dev + g_cursor_off, &o.cur_in)) { o.cur_in = 0; g_noread.fetch_add(1, std::memory_order_relaxed); }
  if (!ReadGuest(dev + g_cursor2_off, &o.cur2_in)) o.cur2_in = 0;
}

void Exit(int hook) {
  if (!g_on || t_depth == 0) return;
  const uint32_t dev = g_device.load(std::memory_order_relaxed);
  // Pop to the matching entry (a tail call or an early return skipped an exit hook: count it, keep going).
  int i = t_depth - 1;
  while (i >= 0 && t_stack[i].hook != uint16_t(hook)) --i;
  if (i < 0) {
    g_mismatch.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint16_t flags = i != t_depth - 1 ? 1 : 0;
  Open o = t_stack[i];
  t_depth = i;
  if (!dev) return;
  const bool recording = Recording();
  if (recording) g_exits.fetch_add(1, std::memory_order_relaxed);
  Rec r;
  r.frame = g_frame.load(std::memory_order_relaxed);
  r.tid = GetCurrentThreadId();
  r.hook = o.hook;
  r.flags = flags;
  r.cur_in = o.cur_in;
  r.cur2_in = o.cur2_in;
  if (!ReadGuest(dev + g_cursor_off, &r.cur_out)) r.cur_out = 0;
  if (!ReadGuest(dev + g_cursor2_off, &r.cur2_out)) r.cur2_out = 0;
  std::lock_guard<std::mutex> lock(g_mu);
  if (recording) g_recs.push_back(r);
  // The last-writer map: every 64-byte block the call's cursor range covers (both fields), from the first frame.
  for (int pass = 0; pass < 2; ++pass) {
    const uint32_t a = pass ? r.cur2_in : r.cur_in, b = pass ? r.cur2_out : r.cur_out;
    if (!a || !b || a == b) continue;
    const uint32_t lo = a & 0x1FFFFFFFu, hi = b & 0x1FFFFFFFu;
    if (hi < lo || hi - lo > (1u << 22)) continue;   // a wrap or a nonsense range (>4 MB): not from this call
    for (uint32_t blk = lo >> 6; blk <= ((hi - 1) >> 6); ++blk) g_blocks[blk] = Writer{r.frame, o.hook, 0};
  }
}

void FrameMarker(uint32_t r3) {
  Init();
  if (!g_on) return;
  if (!g_device.load(std::memory_order_relaxed) && r3 >= 0x40000000u && r3 < 0x90000000u) {
    g_device.store(r3, std::memory_order_relaxed);
    REXLOG_INFO("[p2] device object at {:08X} (the swap entry point's first argument)", r3);
  }
  const uint32_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (f == g_start + g_count) Write();
}

}  // namespace ng2::p2

extern "C" void ng2_p2_exit(int hook) { ng2::p2::Exit(hook); }
