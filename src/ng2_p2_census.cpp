// P2 ATTRIBUTION CENSUS, guest side. See ng2_p2_census.h.
#include "ng2_p2_census.h"

#include <windows.h>

#include <algorithm>
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
// [p3 map] REGISTER-MAP DISCOVERY (NG2_P3MAP=<guest frame>:<max calls>, needs NG2_P2 on; ported from Fable's
// fable2_p2_census.cpp, FABLE2_P3MAP): at the exit of each hooked call that wrote packets in that frame, the XDK
// device object (kDevBytes) and the packets THAT call wrote, [cur_in, cur_out) up to 16 KB, go to p3_guest.bin
// (format v2). tools/native_gpu/p3_flush.py decodes each call's packets and votes, per register the flush wrote,
// the device word holding that value - the map from the device object to the register file, free of per-tile
// transforms (NG2 does not tile). P3 stage 1 (Fable plan b9caa8a): a front end on the game thread.
bool g_p3 = false;
uint32_t g_p3_frame = 0, g_p3_max = 0, g_p3_recorded = 0;
FILE* g_p3_guest = nullptr;
constexpr uint32_t kDevBytes = 0x5000;

std::mutex g_p3_mu;
std::vector<std::pair<uint32_t, uint32_t>> g_p3_ranges;   // physical [lo, hi) of the recorded calls
uint32_t g_p3_bridge = 0;
FILE* g_p3_bridge_f = nullptr;

// [p3 fe] FULL-NATIVE P3 STAGE 1, VALIDATION (NG2_P3FE=<sample every N draws>, needs NG2_P2 on; ported from
// Fable's fable2_p2_census.cpp): a front end on the GAME thread. At the outermost exit of every hooked call, the
// packets that call wrote are decoded into a native register file (type 0 / type 1 writes, SET_CONSTANT,
// LOAD_ALU_CONSTANT). At every Nth DRAW packet the file is snapshotted, keyed by the packet's physical address;
// when the bridge executes that packet (BridgeDraw, RexNgpuDraw::packet_addr), the plugin's register file is
// compared register by register. Mismatch counts per register are logged - the gate for letting the front end
// draw is 0 outside the registers a later pass explains.
bool g_fe = false;
uint32_t g_fe_every = 64;
uint32_t g_fe_regs[0x5000];
struct FeSnap { uint32_t r2[0x400]; uint32_t r4[0x928]; };
std::unordered_map<uint32_t, FeSnap*> g_fe_snaps;
std::atomic<uint64_t> g_fe_calls{0}, g_fe_skipped{0}, g_fe_draws{0}, g_fe_compared{0}, g_fe_unmatched{0};
uint32_t g_fe_mis[0x5000];
std::atomic<uint64_t> g_fe_ops[128];
std::mutex g_fe_mu;

void FeDecode(const uint8_t* body, uint32_t n, uint32_t phys_base) {
  auto be = [&](uint32_t i) {
    const uint8_t* p = body + i * 4;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  const uint32_t w = n / 4;
  auto* ks = rex::system::kernel_state();
  uint32_t i = 0;
  while (i < w) {
    const uint32_t h = be(i);
    if (h == 0xFFFFFFFFu) { ++i; continue; }   // filler at the start of a call's reserved space
    const uint32_t t = h >> 30;
    if (t == 0) {
      const uint32_t base = h & 0x7FFF, cnt = ((h >> 16) & 0x3FFF) + 1, one = (h >> 15) & 1;
      for (uint32_t k = 0; k < cnt && i + 1 + k < w; ++k) {
        const uint32_t reg = one ? base : base + k;
        if (reg < 0x5000) g_fe_regs[reg] = be(i + 1 + k);
      }
      i += 1 + cnt;
    } else if (t == 1) {
      if (i + 2 < w) { g_fe_regs[h & 0x7FF] = be(i + 1); g_fe_regs[(h >> 11) & 0x7FF] = be(i + 2); }
      i += 3;
    } else if (t == 2) {
      ++i;
    } else {
      const uint32_t op = (h >> 8) & 0x7F, cnt = ((h >> 16) & 0x3FFF) + 1;
      g_fe_ops[op].fetch_add(1, std::memory_order_relaxed);
      static const uint32_t kSpace[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (op == 0x2D && i + 1 < w) {                       // SET_CONSTANT
        const uint32_t d = be(i + 1), idx = d & 0x7FF, typ = (d >> 16) & 0xFF;
        if (typ < 5)
          for (uint32_t k = 0; k + 1 < cnt && i + 2 + k < w; ++k) {
            const uint32_t reg = kSpace[typ] + idx + k;
            if (reg < 0x5000) g_fe_regs[reg] = be(i + 2 + k);
          }
      } else if (op == 0x2F && i + 3 < w && ks && ks->memory()) {   // LOAD_ALU_CONSTANT: from guest memory
        const uint32_t addr = be(i + 1) & 0x3FFFFFFF, d = be(i + 2), size = be(i + 3) & 0xFFF;
        const uint32_t idx = d & 0x7FF, typ = (d >> 16) & 0xFF;
        const uint8_t* src = ks->memory()->TranslatePhysical<const uint8_t*>(addr);
        if (src && typ < 5)
          for (uint32_t k = 0; k < size; ++k) {
            const uint32_t reg = kSpace[typ] + idx + k;
            const uint8_t* p = src + k * 4;
            if (reg < 0x5000)
              g_fe_regs[reg] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
          }
      } else if (op == 0x22 || op == 0x36) {                         // DRAW_INDX / DRAW_INDX_2
        const uint64_t nd = g_fe_draws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nd % g_fe_every == 0) {
          // Snapshot the front end's registers for this draw, keyed by the packet's physical address.
          auto* s = new FeSnap;
          std::memcpy(s->r2, g_fe_regs + 0x2000, sizeof(s->r2));
          std::memcpy(s->r4, g_fe_regs + 0x4000, sizeof(s->r4));
          const uint32_t addr = (phys_base + i * 4) & 0x1FFFFFFFu;
          std::lock_guard<std::mutex> lock(g_fe_mu);
          if (g_fe_snaps.size() > 4096) {   // stale entries (the bridge never reached them): drop all
            for (auto& kv : g_fe_snaps) delete kv.second;
            g_fe_snaps.clear();
          }
          auto& slot = g_fe_snaps[addr];
          delete slot;
          slot = s;
        }
      }
      i += 1 + cnt;
    }
  }
}

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
  if (const char* m = std::getenv("NG2_P3MAP"); m && *m) {
    unsigned f = 0, n = 0;
    if (std::sscanf(m, "%u:%u", &f, &n) == 2 && n) {
      g_p3_frame = f;
      g_p3_max = n;
      g_p3 = true;
      REXLOG_INFO("[p3] register-map discovery ON: guest frame {}, up to {} calls -> p3_guest.bin", f, n);
    }
  }
  if (const char* m = std::getenv("NG2_P3FE"); m && *m) {
    g_fe_every = std::max<uint32_t>(1, uint32_t(std::strtoul(m, nullptr, 0)));
    g_fe = true;
    REXLOG_INFO("[p3fe] guest-thread front end ON (validation): every {}th draw compared with the bridge", g_fe_every);
  }
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

void Enter(int hook, uint32_t r3, bool lib) {
  Init();
  if (!g_on) return;
  uint32_t dev = g_device.load(std::memory_order_relaxed);
  if (!dev) {
    // The first library call with a device-shaped first argument: the write cursor at +0x30 and its limit at
    // +0x38 both point into the CPU's view of physical memory, limit above cursor, within 16 MB. The library
    // writes its persistent packet templates at device creation - before the first swap - so waiting for the
    // frame marker missed them (census 2: 1416 replayed draws/frame with no writer).
    if (!lib || r3 < 0x40000000u || r3 >= 0x90000000u) return;
    uint32_t cur = 0, lim = 0;
    if (!ReadGuest(r3 + g_cursor_off, &cur) || !ReadGuest(r3 + 0x38, &lim)) return;
    const uint32_t hi = cur >> 28;
    if (!(hi >= 0xA && hi <= 0xE) || lim <= cur || lim - cur > 0x1000000u) return;
    g_device.store(r3, std::memory_order_relaxed);
    dev = r3;
    REXLOG_INFO("[p2] device object at {:08X} (first library entry, hook {}; cursor {:08X} limit {:08X})", r3, hook,
                cur, lim);
  }
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
  // [p3 fe] decode what this call wrote into the front end's register file (every call, from the device on).
  // Only at the OUTERMOST exit on this thread: an outer call's range contains its inner calls' bytes, so decoding at
  // every exit would apply them twice (and count their draws twice).
  if (g_fe && t_depth == 0 && r.cur_out > r.cur_in && r.cur_in) {
    const uint32_t len = r.cur_out - r.cur_in;
    auto* ks = rex::system::kernel_state();
    const uint8_t* q = (len <= 65536u && ks && ks->memory()) ? ks->memory()->TranslateVirtual<const uint8_t*>(r.cur_in)
                                                             : nullptr;
    if (q) {
      FeDecode(q, len, r.cur_in);
      g_fe_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_fe_skipped.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (g_fe && t_depth == 0 && r.cur_out != r.cur_in) {
    g_fe_skipped.fetch_add(1, std::memory_order_relaxed);   // a range that crossed a buffer switch
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (recording) g_recs.push_back(r);
  // [p3 map] the device object at the exit of a call that wrote packets, in the discovery frame.
  if (g_p3 && r.frame == g_p3_frame && r.cur_out != r.cur_in && r.cur_in && r.cur_out && g_p3_recorded < g_p3_max) {
    auto* ks = rex::system::kernel_state();
    const uint8_t* p = ks && ks->memory() ? ks->memory()->TranslateVirtual<const uint8_t*>(dev) : nullptr;
    if (!g_p3_guest) g_p3_guest = std::fopen("p3_guest.bin", "wb");
    if (g_p3_guest && p) {
      const uint32_t hdr[5] = {uint32_t(o.hook), r.cur_in, r.cur_out, r.cur2_in, r.cur2_out};
      std::fwrite(hdr, sizeof(hdr), 1, g_p3_guest);
      std::fwrite(p, kDevBytes, 1, g_p3_guest);
      uint32_t n = 0;
      const uint8_t* q = nullptr;
      if (r.cur_out > r.cur_in && r.cur_out - r.cur_in <= 16384u)
        q = ks->memory()->TranslateVirtual<const uint8_t*>(r.cur_in);
      if (q) n = r.cur_out - r.cur_in;
      std::fwrite(&n, 4, 1, g_p3_guest);
      if (n) std::fwrite(q, n, 1, g_p3_guest);
      {
        const uint32_t lo = r.cur_in & 0x1FFFFFFFu, hi = r.cur_out & 0x1FFFFFFFu;
        std::lock_guard<std::mutex> lock3(g_p3_mu);
        if (hi > lo) g_p3_ranges.push_back({lo, hi});
      }
      if (++g_p3_recorded == g_p3_max) {
        std::fclose(g_p3_guest);
        g_p3_guest = nullptr;
        REXLOG_INFO("[p3] wrote p3_guest.bin: {} calls with the device object ({} bytes each)", g_p3_recorded,
                    kDevBytes);
      }
    }
  }
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


void BridgeDraw(uint32_t packet_addr, const uint32_t* regs, uint32_t reg_count) {
  if (g_fe && regs && reg_count >= 0x4928) {
    FeSnap* s = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_fe_mu);
      auto it = g_fe_snaps.find(packet_addr & 0x1FFFFFFFu);
      if (it != g_fe_snaps.end()) {
        s = it->second;
        g_fe_snaps.erase(it);   // one comparison per snapshot
      }
    }
    if (!s) {
      g_fe_unmatched.fetch_add(1, std::memory_order_relaxed);
    } else {
      for (uint32_t k = 0; k < 0x400; ++k)
        if (s->r2[k] != regs[0x2000 + k]) ++g_fe_mis[0x2000 + k];
      for (uint32_t k = 0; k < 0x928; ++k)
        if (s->r4[k] != regs[0x4000 + k]) ++g_fe_mis[0x4000 + k];
      delete s;
      const uint64_t c = g_fe_compared.fetch_add(1, std::memory_order_relaxed) + 1;
      if (c % 500 == 0) {
        std::vector<std::pair<uint32_t, uint32_t>> top;
        uint32_t regs_bad = 0;
        for (uint32_t r = 0; r < 0x5000; ++r)
          if (g_fe_mis[r]) { ++regs_bad; top.push_back({g_fe_mis[r], r}); }
        std::sort(top.rbegin(), top.rend());
        std::string s2;
        for (size_t k = 0; k < top.size() && k < 24; ++k) s2 += fmt::format(" {:04X}:{}", top[k].second, top[k].first);
        std::string ops;
        for (int o = 0; o < 128; ++o)
          if (const uint64_t v = g_fe_ops[o].load()) ops += fmt::format(" {:02X}:{}", o, v);
        REXLOG_INFO("[p3fe] {} draws compared ({} bridge draws had no snapshot); front end decoded {} calls, skipped {} "
                    "ranges, saw {} draws; {} registers ever differ, worst:{} | type-3 ops:{}",
                    c, g_fe_unmatched.load(), g_fe_calls.load(), g_fe_skipped.load(), g_fe_draws.load(), regs_bad,
                    s2, ops);
      }
    }
  }
  if (!g_p3 || !regs || reg_count < 0x4928) return;
  const uint32_t a = packet_addr & 0x1FFFFFFFu;
  std::lock_guard<std::mutex> lock(g_p3_mu);
  if (g_p3_ranges.empty() || g_p3_bridge >= 4 * g_p3_max) return;
  bool hit = false;
  for (const auto& rg : g_p3_ranges) {
    if (a >= rg.first && a < rg.second) { hit = true; break; }
  }
  if (!hit) return;
  if (!g_p3_bridge_f) g_p3_bridge_f = std::fopen("p3_bridge.bin", "wb");
  if (!g_p3_bridge_f) return;
  std::fwrite(&packet_addr, 4, 1, g_p3_bridge_f);
  std::fwrite(regs + 0x2000, 4, 0x400, g_p3_bridge_f);
  std::fwrite(regs + 0x4000, 4, 0x928, g_p3_bridge_f);
  if (++g_p3_bridge % 64 == 0) std::fflush(g_p3_bridge_f);
  if (g_p3_bridge == 4 * g_p3_max) {
    std::fclose(g_p3_bridge_f);
    g_p3_bridge_f = nullptr;
    REXLOG_INFO("[p3] wrote p3_bridge.bin: {} bridge draws inside recorded call ranges", g_p3_bridge);
  }
}

}  // namespace ng2::p2

extern "C" void ng2_p2_exit(int hook) { ng2::p2::Exit(hook); }
