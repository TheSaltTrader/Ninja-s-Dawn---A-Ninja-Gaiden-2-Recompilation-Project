// P2 ATTRIBUTION CENSUS, guest side. See ng2_p2_census.h.
#include "ng2_p2_census.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "ng2_ngpu_bridge.h"   // [p3 draw] FrontEndDraw / FrontEndSwap
#include "ng2_native_gs.h"     // [gs] the game's own graphics system

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
  uint32_t args[8];   // r3..r10 at entry (source census)
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

// [p3 fe] FULL-NATIVE P3 STAGE 1, VALIDATION (NG2_P3FE=<sample every N draws>, needs NG2_P2 on; Fable's design
// 8a64512, ported): a front end on the GAME thread. KICK MODE (the fork plugin's RexNgpuSetKickCallback, fired at
// the guest's CP_RB_WPTR write on the guest thread, before the worker is signalled): the hardware ring is decoded
// from the front end's own read index to the kicked write index, INDIRECT_BUFFERs expanded where they execute, bin
// predication applied as the CP does - the execution order with exact packet boundaries, no cursor semantics (NG2's
// 980 template draws per frame arrive through IBs, which a per-call decode never saw). Fallback without the export:
// the per-thread cursor high-water mark (FeAdvance). DRAW packets are sampled BY ADDRESS ((addr >> 2) % N == 0) so
// every execution of a sampled draw is snapshotted, FIFO per address; when the bridge executes that packet
// (BridgeDraw, RexNgpuDraw::packet_addr), the plugin's register file is compared register by register.
bool g_fe = false;
uint32_t g_fe_every = 64;
uint32_t g_fe_regs[0x5000];
uint32_t g_fe_src[0x5000];   // for LOAD_ALU_CONSTANT-loaded registers: the guest physical address it came from (0 = packet)
struct FeSnap { uint32_t frame; uint32_t r2[0x400]; uint32_t r4[0x928]; uint32_t src4[0x928]; };
// Keyed by (packet address, execution ordinal within the frame): both sides count an address's executions since
// their own last swap, so a snapshot is paired with the same execution whatever either side did before the
// other registered (a per-address FIFO kept the bridge's pre-registration executions at its front for good).
std::unordered_map<uint64_t, std::deque<FeSnap*>> g_fe_snaps;
uint32_t g_fe_frame = 0;                              // XE_SWAP packets the front end decoded
std::unordered_map<uint32_t, uint32_t> g_fe_ord;      // front end: address -> executions this frame (guest thread)
std::unordered_map<uint32_t, uint32_t> g_br_ord;      // bridge: the same, reset at BridgeSwap (GPU thread)
std::atomic<uint64_t> g_fe_expired{0};
std::atomic<uint64_t> g_fe_calls{0}, g_fe_skipped{0}, g_fe_draws{0}, g_fe_compared{0}, g_fe_unmatched{0};
std::atomic<uint64_t> g_fe_seen{0};   // bridge draws that reached BridgeDraw (matched or not)
std::atomic<uint32_t> g_bridge_frame{0};   // the bridge's swap count, stamped on p3_bridge.bin records
uint32_t g_fe_mis[0x5000];
std::atomic<uint64_t> g_fe_ops[128];
void FeGateReport();   // [walk gate] below: packets with no front-end handling, early stops
uint64_t g_fe_mis_memsrc = 0, g_fe_mis_memsrc_now_bridge = 0, g_fe_mis_packet = 0;
std::mutex g_fe_mu;
thread_local int t_fe_depth = 0;
uint64_t g_fe_bin_mask = ~0ull, g_fe_bin_select = ~0ull;
std::atomic<uint64_t> g_fe_predicated_skips{0};
std::atomic<bool> g_fe_kick_mode{false};
std::atomic<uint64_t> g_fe_kicks{0};
// Early stops of a decode window, by reason and place (ring / inside an INDIRECT_BUFFER): a draw the front end
// never sees shifts that address's FIFO by one execution for good (leg ng2_076: 15.64 M seen vs 15.72 M bridge
// draws, and the template draw's constants paired one animation step apart).
std::atomic<uint64_t> g_fe_stop_unfilled_ring{0}, g_fe_stop_unfilled_ib{0}, g_fe_stop_short_ring{0}, g_fe_stop_short_ib{0}, g_fe_ib_unreadable{0}, g_fe_ib_deep{0};
std::atomic<int> g_fe_stop_logged{0};
std::atomic<uint64_t> g_fe_ring_resets{0}, g_fe_resyncs{0};
// [p5] SIDE-EFFECT CENSUS, front-end side (NG2_P5=1): what the front end would do for each side-effect packet
// it decodes -> p5_fe.bin {frame, kind, addr, value}; kinds as the plugin's (fork command_processor.cpp ng2_p5):
// 1 MEM_WRITE, 2/3 COND_WRITE memory/register, 4/5 EVENT_WRITE_SHD literal/counter, 6 EXT, 7 ZPD, 8 REG_TO_MEM,
// 9 INTERRUPT, 10 REG_RMW, 11 read-pointer write-back, 12 XE_SWAP, 13 WAIT_REG_MEM.
bool g_p5 = false;
FILE* g_p5_f = nullptr;
uint64_t g_p5_n = 0;
uint32_t g_fe_swap_counter = 0;   // XE_SWAP packets decoded since start (the plugin's counter_)
// [p5 exec] NG2_P5EXEC=1: the side effects of each kick, pushed to the plugin's executor at the kick's end.
bool g_p5exec = false;
std::vector<uint32_t> g_px;
std::mutex g_kick_mu;
using PushFn = void (*)(const uint32_t*, uint32_t);
PushFn g_push = nullptr;
std::atomic<uint64_t> g_px_recs{0}, g_px_regs{0}, g_px_waits{0}, g_px_unhandled{0};
inline void Px(uint32_t k, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
  g_px.push_back(k); g_px.push_back(a); g_px.push_back(b); g_px.push_back(c);
  g_px_recs.fetch_add(1, std::memory_order_relaxed);
}
inline uint32_t FeGpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return _byteswap_ulong(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}
void P5Note(uint32_t kind, uint32_t addr, uint32_t value);
// [p3 draw] NG2_P3DRAW=1: the front end DRAWS (Fable's design (a)): the shaders it tracks from IM_LOAD /
// IM_LOAD_IMMEDIATE, a dirty bitmap of the registers it wrote since the last draw, and at each DRAW packet the
// bridge's FrontEndDraw with its own register file; at XE_SWAP the bridge's FrontEndSwap. All on the guest
// thread; the plugin's callbacks are compare-only then.
bool g_p3draw = false;
bool g_fe_compare = true;   // per-address ordinals and snapshots for the plugin compare; off in production
uint32_t g_fe_vs = 0, g_fe_vs_dwords = 0, g_fe_ps = 0, g_fe_ps_dwords = 0;
bool g_fe_vs_inline = false, g_fe_ps_inline = false;
std::vector<uint8_t> g_fe_imm_vs, g_fe_imm_ps;
uint64_t g_fe_dirty[(0x5000 + 63) / 64];
std::atomic<uint64_t> g_fe_draws_issued{0}, g_fe_swaps_issued{0};
inline void Px(uint32_t k, uint32_t a, uint32_t b, uint32_t c);
extern bool g_p5exec;
extern std::atomic<uint64_t> g_px_regs;
inline void FeSet(uint32_t reg, uint32_t v) {
  g_fe_regs[reg] = v;
  g_fe_src[reg] = 0;
  g_fe_dirty[reg >> 6] |= uint64_t(1) << (reg & 63);
  if (g_p5exec && reg >= 0x1921 && reg <= 0x1927)   // [gs] the gamma port: the backend keeps the ramp itself
    ::ng2::ngpu::FrontEndRegisterNow(reg, v);
  if (g_p5exec && !((reg >= 0x2000 && reg < 0x2400) || (reg >= 0x4000 && reg < 0x4928))) {
    Px(7, reg, v, 0);   // [p5 exec] outside the draw ranges: the plugin's register file keeps it (scratch, gamma port)
    g_px_regs.fetch_add(1, std::memory_order_relaxed);
  }
}
// [p3 src] SOURCE CENSUS (NG2_P3SRC=<guest frame>): every hooked call's r3-r10 plus 256 bytes behind each
// pointer-like argument, and every draw the front end decodes in that frame with its register file and the IB
// packet that led to it - joined offline (tools/native_gpu/p3_sources.py) to find each per-draw register's source.
bool g_src = false;
uint32_t g_src_frame = 0, g_src_calls = 0, g_src_draws = 0;
FILE* g_src_f = nullptr;
FILE* g_src_fe_f = nullptr;
std::mutex g_src_mu;
thread_local uint32_t t_fe_issuer = 0;   // the INDIRECT_BUFFER packet's address the current decode was entered through
bool SafeCopy(uint8_t* dst, const uint8_t* src, uint32_t n) {
  __try {
    std::memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
uint32_t g_fe_stuck_pos = 0xFFFFFFFFu, g_fe_stuck_n = 0;

// Decodes [body, body + n) whose first byte lives at guest physical phys_base; returns the bytes consumed (a packet
// that runs past the end, or an unfilled 0xFFFFFFFF header, stops the decode there - the next call resumes).
uint32_t FeDecode(const uint8_t* body, uint32_t n, uint32_t phys_base) {
  auto be = [&](uint32_t i) {
    const uint8_t* p = body + i * 4;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  const uint32_t w = n / 4;
  auto* ks = rex::system::kernel_state();
  uint32_t i = 0;
  while (i < w) {
    const uint32_t h = be(i);
    // 0xFFFFFFFF is an UNFILLED header: the library reserves a packet, writes its payload and patches the header
    // afterwards (Fable FE_F3: a UP draw's vertices decoded as packets). Stop here; resume once the header is real.
    if (h == 0xFFFFFFFFu) {
      (t_fe_depth ? g_fe_stop_unfilled_ib : g_fe_stop_unfilled_ring).fetch_add(1, std::memory_order_relaxed);
      if (g_fe_stop_logged.fetch_add(1) < 6)
        REXLOG_INFO("[p3fe] decode stopped at an unfilled header: {} at {:08X} (+{} of {} dwords)", t_fe_depth ? "IB" : "ring",
                    phys_base + i * 4, i, w);
      break;
    }
    const uint32_t t = h >> 30;
    const uint32_t need = t == 0 ? ((h >> 16) & 0x3FFF) + 2 : t == 1 ? 3 : t == 2 ? 1 : ((h >> 16) & 0x3FFF) + 2;
    if (i + need > w) {   // a packet that runs past what has been written yet: stop BEFORE it
      (t_fe_depth ? g_fe_stop_short_ib : g_fe_stop_short_ring).fetch_add(1, std::memory_order_relaxed);
      if (g_fe_stop_logged.fetch_add(1) < 6) {
        std::string ctx;
        for (uint32_t k = (i >= 12 ? i - 12 : 0); k < w && k < i + 8; ++k) ctx += fmt::format("{}{:08X}", k == i ? " |" : " ", be(k));
        REXLOG_INFO("[p3fe] decode stopped before a packet past the end: {} at {:08X} header {:08X} needs {} of {} left; dwords:{}",
                    t_fe_depth ? "IB" : "ring", phys_base + i * 4, h, need, w - i, ctx);
      }
      break;
    }
    if (t == 0) {
      const uint32_t base = h & 0x7FFF, cnt = ((h >> 16) & 0x3FFF) + 1, one = (h >> 15) & 1;
      for (uint32_t k = 0; k < cnt && i + 1 + k < w; ++k) {
        const uint32_t reg = one ? base : base + k;
        if (reg < 0x5000) FeSet(reg, be(i + 1 + k));
      }
      i += 1 + cnt;
    } else if (t == 1) {
      if (i + 2 < w) { FeSet(h & 0x7FF, be(i + 1)); FeSet((h >> 11) & 0x7FF, be(i + 2)); }
      i += 3;
    } else if (t == 2) {
      ++i;
    } else {
      const uint32_t op = (h >> 8) & 0x7F, cnt = ((h >> 16) & 0x3FFF) + 1;
      g_fe_ops[op].fetch_add(1, std::memory_order_relaxed);
      if (op == 0x64) { ++g_fe_frame; g_fe_ord.clear(); }   // XE_SWAP: a new frame for the ordinals
      if (op == 0x64 && g_fe_frame % 1800 == 0) FeGateReport();   // every ~30 s at 60 fps
      // [p5] the side-effect packets: recorded as the plugin would perform them; register effects executed.
      if (!((h & 1) && (g_fe_bin_mask & g_fe_bin_select) == 0)) {
        auto* ksm = ks ? ks->memory() : nullptr;
        if (op == 0x3D && cnt >= 2) {                                 // MEM_WRITE
          const uint32_t a0 = be(i + 1);
          for (uint32_t k = 0; k + 1 < cnt; ++k) { P5Note(1, a0 + 4 * k, be(i + 2 + k)); if (g_p5exec) Px(1, a0 + 4 * k, be(i + 2 + k)); }
        } else if (op == 0x45 && cnt >= 6) {                          // COND_WRITE
          const uint32_t wi = be(i + 1), poll = be(i + 2), ref = be(i + 3), mask = be(i + 4), wa = be(i + 5), wd = be(i + 6);
          uint32_t v = 0;
          if (wi & 0x10) {
            const uint8_t* pp = ksm ? ksm->TranslatePhysical<const uint8_t*>(poll & ~3u) : nullptr;
            uint32_t raw = 0;
            if (pp) std::memcpy(&raw, pp, 4);
            v = FeGpuSwap(raw, poll & 3);
          } else {
            v = poll < 0x5000 ? g_fe_regs[poll] : 0;
          }
          bool m = false;
          switch (wi & 7) {
            case 1: m = (v & mask) < ref; break;
            case 2: m = (v & mask) <= ref; break;
            case 3: m = (v & mask) == ref; break;
            case 4: m = (v & mask) != ref; break;
            case 5: m = (v & mask) >= ref; break;
            case 6: m = (v & mask) > ref; break;
            case 7: m = true; break;
            default: break;
          }
          if (m) {
            P5Note((wi & 0x100) ? 2 : 3, wa, wd);
            if (g_p5exec && (wi & 0x100)) Px(1, wa, wd);   // a register write goes through FeSet below
            if (!(wi & 0x100) && wa < 0x5000) FeSet(wa, wd);
          }
        } else if (op == 0x46 && cnt >= 1) {                          // EVENT_WRITE: the initiator writeback only
          FeSet(0x21F9, be(i + 1) & 0x3F);                           // VGT_EVENT_INITIATOR, as the plugin writes it
        } else if (op == 0x58 && cnt >= 3) {                          // EVENT_WRITE_SHD
          FeSet(0x21F9, be(i + 1) & 0x3F);                           // VGT_EVENT_INITIATOR
          const uint32_t ini = be(i + 1);
          P5Note((ini >> 31) ? 5 : 4, be(i + 2), (ini >> 31) ? g_fe_swap_counter : be(i + 3));
          if (g_p5exec) { if (ini >> 31) Px(2, be(i + 2)); else Px(1, be(i + 2), be(i + 3)); }
        } else if (op == 0x5A && cnt >= 2) {                          // EVENT_WRITE_EXT
          FeSet(0x21F9, be(i + 1) & 0x3F);                           // VGT_EVENT_INITIATOR
          P5Note(6, be(i + 2), 0);
          if (g_p5exec) Px(9, be(i + 2));   // screen extents: the executor writes the plugin's fixed full-screen box
        } else if (op == 0x5B && cnt >= 1) {                          // EVENT_WRITE_ZPD
          FeSet(0x21F9, be(i + 1) & 0x3F);                           // VGT_EVENT_INITIATOR
          P5Note(7, g_fe_regs[0x2325], be(i + 1));
          // Occlusion query: the executor fakes the result as the plugin does (a finished query reports samples
          // passed). Plugin parity only: NG2's census has no ZPD or EXT packets.
          if (g_p5exec) Px(8, g_fe_regs[0x2325]);   // RB_SAMPLE_COUNT_ADDR as of this packet
        } else if (op == 0x3E && cnt >= 2) {                          // REG_TO_MEM
          const uint32_t r = be(i + 1);
          P5Note(8, be(i + 2), r < 0x5000 ? g_fe_regs[r] : 0);
          if (g_p5exec) Px(1, be(i + 2), r < 0x5000 ? g_fe_regs[r] : 0);
        } else if (op == 0x54 && cnt >= 1) {                          // INTERRUPT
          P5Note(9, 0, be(i + 1));
          if (g_p5exec) Px(4, be(i + 1));
        } else if (op == 0x21 && cnt >= 3) {                          // REG_RMW
          const uint32_t info = be(i + 1), am = be(i + 2), om = be(i + 3), r = info & 0x1FFF;
          uint32_t v = r < 0x5000 ? g_fe_regs[r] : 0;
          v &= ((info >> 31) & 1) ? ((am & 0x1FFF) < 0x5000 ? g_fe_regs[am & 0x1FFF] : 0) : am;
          v |= ((info >> 30) & 1) ? ((om & 0x1FFF) < 0x5000 ? g_fe_regs[om & 0x1FFF] : 0) : om;
          P5Note(10, r, v);
          if (r < 0x5000) FeSet(r, v);
        } else if (op == 0x64 && cnt >= 4) {                          // XE_SWAP
          P5Note(12, be(i + 2), g_fe_swap_counter);
          if (g_p5exec) Px(5, be(i + 2), be(i + 3), be(i + 4));
          ++g_fe_swap_counter;
        } else if (op == 0x3C && cnt >= 3) {                          // WAIT_REG_MEM
          P5Note(13, be(i + 2), be(i + 3));
          if (g_p5exec && cnt >= 4 && (be(i + 1) & 0x10)) {   // memory waits: the executor honours them in order
            Px(6, be(i + 1), be(i + 2), be(i + 3));
            Px(0, be(i + 4));
            g_px_waits.fetch_add(1, std::memory_order_relaxed);
          }
          // [p5] would this wait block if the front end executed it now (at the kick)? kind 14 = NOT satisfied at
          // decode (addr = poll, value = what the poll reads now): the cut-over must honour these on its executor.
          if (cnt >= 4) {
            const uint32_t wi = be(i + 1), poll = be(i + 2), ref = be(i + 3), mask = be(i + 4);
            uint32_t v = 0;
            if (wi & 0x10) {
              const uint8_t* pp = ksm ? ksm->TranslatePhysical<const uint8_t*>(poll & ~3u) : nullptr;
              uint32_t raw = 0;
              if (pp) std::memcpy(&raw, pp, 4);
              v = FeGpuSwap(raw, poll & 3);
            } else {
              v = poll < 0x5000 ? g_fe_regs[poll] : 0;
            }
            bool m = false;
            switch (wi & 7) {
              case 1: m = (v & mask) < ref; break;
              case 2: m = (v & mask) <= ref; break;
              case 3: m = (v & mask) == ref; break;
              case 4: m = (v & mask) != ref; break;
              case 5: m = (v & mask) >= ref; break;
              case 6: m = (v & mask) > ref; break;
              case 7: m = true; break;
              default: break;
            }
            if (!m) P5Note(14, poll, v);
          }
        }
      }
      // Predicated tiling, as the GPU (and the plugin) apply it: SET_BIN_MASK/SELECT LO/HI keep the bin state; a
      // predicated packet (header bit 0) runs only when mask & select overlap. (NG2 never tiles; kept for parity.)
      if ((op == 0x50 || op == 0x51) && i + 2 < w) {
        (op == 0x50 ? g_fe_bin_mask : g_fe_bin_select) = (uint64_t(be(i + 1)) << 32) | be(i + 2);
        i += 1 + cnt;
        continue;
      }
      if (op >= 0x60 && op <= 0x63 && i + 1 < w) {
        const uint64_t v = be(i + 1);
        uint64_t& tgt = op <= 0x61 ? g_fe_bin_mask : g_fe_bin_select;
        tgt = (op & 1) ? ((tgt & 0xFFFFFFFFull) | (v << 32)) : ((tgt & ~0xFFFFFFFFull) | v);
        i += 1 + cnt;
        continue;
      }
      if ((h & 1) && (g_fe_bin_mask & g_fe_bin_select) == 0) {
        g_fe_predicated_skips.fetch_add(1, std::memory_order_relaxed);
        i += 1 + cnt;
        continue;
      }
      static const uint32_t kSpace[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (op == 0x2D && i + 1 < w) {                       // SET_CONSTANT
        const uint32_t d = be(i + 1), idx = d & 0x7FF, typ = (d >> 16) & 0xFF;
        if (typ < 5)
          for (uint32_t k = 0; k + 1 < cnt && i + 2 + k < w; ++k) {
            const uint32_t reg = kSpace[typ] + idx + k;
            if (reg < 0x5000) FeSet(reg, be(i + 2 + k));
          }
      } else if (op == 0x2F && i + 3 < w && ks && ks->memory()) {   // LOAD_ALU_CONSTANT: from guest memory
        const uint32_t addr = be(i + 1) & 0x3FFFFFFF, d = be(i + 2), size = be(i + 3) & 0xFFF;
        const uint32_t idx = d & 0x7FF, typ = (d >> 16) & 0xFF;
        const uint8_t* src = ks->memory()->TranslatePhysical<const uint8_t*>(addr);
        if (src && typ < 5)
          for (uint32_t k = 0; k < size; ++k) {
            const uint32_t reg = kSpace[typ] + idx + k;
            const uint8_t* p = src + k * 4;
            if (reg < 0x5000) {
              FeSet(reg, (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
              g_fe_src[reg] = addr + k * 4;
            }
          }
      } else if (op == 0x3F && t_fe_depth >= 4) {
        g_fe_ib_deep.fetch_add(1, std::memory_order_relaxed);
      } else if (op == 0x3F && i + 2 < w && ks && ks->memory() && t_fe_depth < 4) {   // INDIRECT_BUFFER: execute it now
        const uint32_t ib = be(i + 1) & 0x1FFFFFFFu, ibn = be(i + 2) & 0xFFFFF;
        const uint8_t* src = ks->memory()->TranslatePhysical<const uint8_t*>(ib);
        if (src && ibn) {
          const uint32_t saved_issuer = t_fe_issuer;
          t_fe_issuer = (phys_base + i * 4) & 0x1FFFFFFFu;   // this IB packet
          ++t_fe_depth;
          FeDecode(src, ibn * 4, ib);
          --t_fe_depth;
          t_fe_issuer = saved_issuer;
        } else {
          g_fe_ib_unreadable.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (op == 0x27 && cnt >= 2) {                            // IM_LOAD: shader at an address
        const uint32_t addr_type = be(i + 1), dwords = be(i + 2) & 0xFFFF;
        if ((addr_type & 3) == 0) { g_fe_vs = addr_type & ~3u; g_fe_vs_dwords = dwords; g_fe_vs_inline = false; }
        else { g_fe_ps = addr_type & ~3u; g_fe_ps_dwords = dwords; g_fe_ps_inline = false; }
      } else if (op == 0x2B && cnt >= 2) {                            // IM_LOAD_IMMEDIATE: shader in the packet
        const bool ps = (be(i + 1) & 3) != 0;
        const uint32_t dwords = std::min<uint32_t>(be(i + 2) & 0xFFFF, cnt - 2);
        std::vector<uint8_t>& dst = ps ? g_fe_imm_ps : g_fe_imm_vs;
        dst.assign(body + (i + 3) * 4, body + (i + 3 + dwords) * 4);   // raw big-endian bytes as the guest wrote them
        (ps ? g_fe_ps_inline : g_fe_vs_inline) = true;
        (ps ? g_fe_ps_dwords : g_fe_vs_dwords) = dwords;
      } else if (op == 0x64 && cnt >= 4 && g_p3draw) {                // XE_SWAP: magic, front buffer, width, height
        ::ng2::ngpu::FrontEndSwap(be(i + 2), be(i + 3), be(i + 4), g_fe_regs, g_fe_dirty);
        g_fe_swaps_issued.fetch_add(1, std::memory_order_relaxed);
      } else if (op == 0x22 || op == 0x36) {                         // DRAW_INDX / DRAW_INDX_2
        g_fe_draws.fetch_add(1, std::memory_order_relaxed);
        if (g_p3draw) {
          ::ng2::ngpu::FeDrawInfo d{};
          if (op == 0x22) {   // viz query token, initiator, then for a DMA source the index base and size
            d.draw_initiator = cnt >= 2 ? be(i + 2) : 0;
            if (((d.draw_initiator >> 6) & 3) == 0 && cnt >= 4) { d.index_addr = be(i + 3); d.index_size = be(i + 4); }
          } else {
            d.draw_initiator = be(i + 1);
          }
          d.vs_addr = g_fe_vs; d.vs_dwords = g_fe_vs_dwords; d.ps_addr = g_fe_ps; d.ps_dwords = g_fe_ps_dwords;
          d.vs_inline = g_fe_vs_inline; d.ps_inline = g_fe_ps_inline;
          d.vs_code = g_fe_imm_vs.empty() ? nullptr : g_fe_imm_vs.data();
          d.ps_code = g_fe_imm_ps.empty() ? nullptr : g_fe_imm_ps.data();
          d.vs_code_dwords = uint32_t(g_fe_imm_vs.size() / 4); d.ps_code_dwords = uint32_t(g_fe_imm_ps.size() / 4);
          ::ng2::ngpu::FrontEndDraw(g_fe_regs, g_fe_dirty, d);
          g_fe_draws_issued.fetch_add(1, std::memory_order_relaxed);
        }
        const uint32_t addr = (phys_base + i * 4) & 0x1FFFFFFFu;
        const uint32_t ord = g_fe_compare ? g_fe_ord[addr]++ : 0;
        if (g_src && g_frame.load(std::memory_order_relaxed) == g_src_frame) {   // [p3 src] every draw of the frame
          std::lock_guard<std::mutex> lock(g_src_mu);
          if (!g_src_fe_f) g_src_fe_f = std::fopen("p3_fe.bin", "wb");
          if (g_src_fe_f && g_src_draws < 20000) {
            const uint32_t hdr[4] = {addr, t_fe_issuer, ord, g_fe_frame};
            std::fwrite(hdr, sizeof(hdr), 1, g_src_fe_f);
            std::fwrite(g_fe_regs + 0x2000, 4, 0x400, g_src_fe_f);
            std::fwrite(g_fe_regs + 0x4000, 4, 0x928, g_src_fe_f);
            ++g_src_draws;
          }
        }
        if (g_fe_compare && ((addr >> 2) % g_fe_every) == 0) {   // sampled BY ADDRESS, so every execution of it is snapshotted
          auto* s = new FeSnap;
          s->frame = g_fe_frame;
          std::memcpy(s->r2, g_fe_regs + 0x2000, sizeof(s->r2));
          std::memcpy(s->r4, g_fe_regs + 0x4000, sizeof(s->r4));
          std::memcpy(s->src4, g_fe_src + 0x4000, sizeof(s->src4));
          std::lock_guard<std::mutex> lock(g_fe_mu);
          if (g_fe_snaps.size() > 8192) {   // stale entries (the bridge never reached them): drop all
            for (auto& kv : g_fe_snaps) for (auto* p : kv.second) delete p;
            g_fe_snaps.clear();
          }
          auto& q = g_fe_snaps[(uint64_t(addr) << 32) | ord];
          q.push_back(s);
          if (q.size() > 8) { delete q.front(); q.pop_front(); }
        }
      }
      i += 1 + cnt;
    }
  }
  return i * 4;
}

// KICK MODE: the hardware ring, from the front end's own read index to the kicked write index, on the guest thread
// that kicked. The ring wraps; a packet may straddle the end, so the new dwords are copied out contiguously.
uint32_t g_fe_rptr = 0;
std::vector<uint8_t> g_fe_ringcopy;
void FeKick(uint32_t ring_ptr, uint32_t ring_bytes, uint32_t wptr) {
  if (!g_fe) return;
  std::lock_guard<std::mutex> kick_lock(g_kick_mu);   // kicks from different guest threads decode one at a time
  if (wptr == 0xFFFFFFFFu) {   // the ring was (re)initialised: both hardware pointers are 0 again
    g_fe_rptr = 0;
    g_fe_stuck_pos = 0xFFFFFFFFu;
    g_fe_stuck_n = 0;
    if (g_fe_ring_resets.fetch_add(1, std::memory_order_relaxed) < 4)
      REXLOG_INFO("[p3fe] ring reset: {:08X} x{} - the front end restarts its read index", ring_ptr, ring_bytes);
    return;
  }
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory() || !ring_bytes) return;
  const uint8_t* ring = ks->memory()->TranslatePhysical<const uint8_t*>(ring_ptr);
  if (!ring) return;
  const uint32_t nd = ring_bytes / 4;
  wptr %= nd;
  g_fe_kicks.fetch_add(1, std::memory_order_relaxed);
  if (g_fe_rptr == wptr) return;
  g_fe_ringcopy.clear();
  for (uint32_t k = g_fe_rptr; k != wptr; k = (k + 1) % nd)
    g_fe_ringcopy.insert(g_fe_ringcopy.end(), ring + k * 4, ring + k * 4 + 4);
  const uint32_t base = ring_ptr + g_fe_rptr * 4;   // exact for draws before a wrap; ring-resident draws are rare
  const uint32_t used = FeDecode(g_fe_ringcopy.data(), uint32_t(g_fe_ringcopy.size()), base);
  if (g_p5) P5Note(11, 0, (g_fe_rptr + used / 4) % nd);   // [p5] where the front end's read index lands
  if (g_p5exec && g_push) {
    Px(3, (g_fe_rptr + used / 4) % nd);   // the read pointer the game polls; the plugin stamps the ring epoch
    g_push(g_px.data(), uint32_t(g_px.size() / 4));
    g_px.clear();
  }
  if (used == 0 && !g_fe_ringcopy.empty()) {   // no progress: stuck on a dword that is not a header
    if (g_fe_stuck_pos == g_fe_rptr) {
      if (++g_fe_stuck_n >= 16) {
        g_fe_resyncs.fetch_add(1, std::memory_order_relaxed);
        REXLOG_INFO("[p3fe] stuck at ring index {} for {} kicks: resynchronising at the write index {}", g_fe_rptr,
                    g_fe_stuck_n, wptr);
        g_fe_rptr = wptr;
        g_fe_stuck_n = 0;
        return;
      }
    } else {
      g_fe_stuck_pos = g_fe_rptr;
      g_fe_stuck_n = 1;
    }
  } else {
    g_fe_stuck_n = 0;
  }
  g_fe_rptr = (g_fe_rptr + used / 4) % nd;
  g_fe_calls.fetch_add(1, std::memory_order_relaxed);
}

// FALLBACK (no kick export): a per-thread cursor high-water mark advanced at every Enter and Exit; the bytes the
// cursor moved over since the previous event, [hwm + 4, cur + 4) (the cursor names the last dword written), decoded
// in cursor order. A backward jump or a gap over 64 KB is a buffer switch: skipped, the mark reset.
thread_local uint32_t t_fe_hwm = 0;
void FeAdvance(uint32_t cur) {
  if (!g_fe || g_fe_kick_mode.load(std::memory_order_relaxed) || !cur) return;
  if (!t_fe_hwm) { t_fe_hwm = cur; return; }
  if (cur == t_fe_hwm) return;
  if (cur < t_fe_hwm || cur - t_fe_hwm > 65536u) {
    g_fe_skipped.fetch_add(1, std::memory_order_relaxed);
    t_fe_hwm = cur;
    return;
  }
  auto* ks = rex::system::kernel_state();
  const uint8_t* q = ks && ks->memory() ? ks->memory()->TranslateVirtual<const uint8_t*>(t_fe_hwm + 4) : nullptr;
  if (q) {
    const uint32_t used = FeDecode(q, cur - t_fe_hwm, t_fe_hwm + 4);
    t_fe_hwm += used;   // an incomplete last packet waits for the next event
    g_fe_calls.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_fe_skipped.fetch_add(1, std::memory_order_relaxed);
    t_fe_hwm = cur;
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
  if (const char* m = std::getenv("NG2_P3SRC"); m && *m) {
    g_src_frame = uint32_t(std::strtoul(m, nullptr, 0));
    g_src = true;
    REXLOG_INFO("[p3src] source census ON: guest frame {} -> p3_src.bin (calls: args + samples) and p3_fe.bin (draws)", g_src_frame);
  }
  if (const char* m = std::getenv("NG2_P5"); m && *m && *m != '0') {
    g_p5 = true;
    REXLOG_INFO("[p5] side-effect census ON (front-end side) -> p5_fe.bin; the plugin writes p5_plugin.bin");
  }
  if (const char* m = std::getenv("NG2_P3DRAW"); m && *m && *m != '0') {
    g_p3draw = true;
    REXLOG_INFO("[p3draw] THE FRONT END DRAWS: the guest thread records every draw it decodes and swaps at XE_SWAP; the plugin's callbacks are compare-only");
  }
  if (const char* m = std::getenv("NG2_P3FE"); m && *m) {
    g_fe_every = std::max<uint32_t>(1, uint32_t(std::strtoul(m, nullptr, 0)));
    g_fe = true;
    REXLOG_INFO("[p3fe] guest-thread front end ON (validation): every {}th draw compared with the bridge", g_fe_every);
  }
  if (g_fe) {
    using SetKickFn = void (*)(void (*)(uint32_t, uint32_t, uint32_t));
    HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
    auto f = m ? reinterpret_cast<SetKickFn>(GetProcAddress(m, "RexNgpuSetKickCallback")) : nullptr;
    if (f) { f(&FeKick); g_fe_kick_mode = true; }
    if (f) {
      if (const char* x = std::getenv("NG2_P5EXEC"); x && *x && *x != '0') {
        auto setx = reinterpret_cast<void (*)(int)>(GetProcAddress(m, "RexNgpuSetExecMode"));
        g_push = reinterpret_cast<PushFn>(GetProcAddress(m, "RexNgpuPushSideEffects"));
        if (setx && g_push && g_p3draw) {
          g_p5exec = true;
          setx(1);
          REXLOG_INFO("[p5x] EXECUTOR MODE: the plugin no longer parses the ring; the front end hands it each kick's side effects");
        } else {
          REXLOG_INFO("[p5x] NG2_P5EXEC refused: {}", !g_p3draw ? "needs NG2_P3DRAW=1" : "the plugin has no executor exports");
        }
      }
    }
    REXLOG_INFO("[p3fe] kick callback {}", f ? "registered - the hardware ring drives the front end (execution order)"
                                            : "MISSING - the cursor high-water mark drives it");
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

void P5Note(uint32_t kind, uint32_t addr, uint32_t value) {
  if (!g_p5) return;
  const uint32_t f = g_frame.load(std::memory_order_relaxed);
  if (f + 1 < g_start || f > g_start + g_count) return;
  if (!g_p5_f) g_p5_f = std::fopen("p5_fe.bin", "wb");
  if (!g_p5_f) { g_p5 = false; return; }
  const uint32_t rec[4] = {f, kind, addr, value};
  std::fwrite(rec, sizeof(rec), 1, g_p5_f);
  ++g_p5_n;
}

// [walk gate] (user 2026-09-27: "walk in every stage to test to see if we are missing any gpu calls" before
// rexgpu-xenos.dll goes). Every type-3 packet the front end has NO handling for, with counts since start - the
// plugin acts on some of them (VIZ_QUERY 0x23, INDIRECT_BUFFER_PFD 0x37, INVALIDATE_STATE 0x3B, EVENT_WRITE 0x46,
// ME_INIT 0x48, SET_CONSTANT2 0x55, SET_SHADER_CONSTANTS 0x56, CONTEXT_UPDATE 0x5E, ...), so a non-empty list
// names what the own graphics system would drop - plus the decode early stops, which drop draws too.
void FeGateReport() {
  // Handled, or a no-op in the plugin as well: 0x3B INVALIDATE_STATE (the plugin reads the mask, its call is
  // commented out), 0x48 ME_INIT (stored in a buffer nothing reads) - walk 2026-09-27, Chapter 6.
  static const uint8_t kHandled[] = {0x10, 0x21, 0x22, 0x26, 0x27, 0x2B, 0x2D, 0x2F, 0x36, 0x3B, 0x3C, 0x3D, 0x3E,
                                     0x3F, 0x45, 0x46, 0x48, 0x50, 0x51, 0x54, 0x58, 0x5A, 0x5B, 0x60, 0x61, 0x62,
                                     0x63, 0x64};
  std::string missing;
  for (int o = 0; o < 128; ++o) {
    const uint64_t v = g_fe_ops[o].load(std::memory_order_relaxed);
    if (!v) continue;
    bool handled = false;
    for (uint8_t h : kHandled) handled |= (h == o);
    if (!handled) missing += fmt::format(" {:02X}:{}", o, v);
  }
  REXLOG_INFO("[walk] frame {}: packets with no front-end handling:{} | early stops: ib unreadable {}, ib too deep {}, "
              "short ring {} ib {}, resyncs {}, ring resets {}",
              g_fe_frame, missing.empty() ? std::string(" none") : missing, g_fe_ib_unreadable.load(),
              g_fe_ib_deep.load(), g_fe_stop_short_ring.load(), g_fe_stop_short_ib.load(), g_fe_resyncs.load(),
              g_fe_ring_resets.load());
}

void FeReport(const char* why) {
  if (g_p5exec)
    REXLOG_INFO("[p5x] front end handed the executor {} records ({} register writes, {} memory waits); {} side effects it cannot hand (EXT/ZPD)",
                g_px_recs.load(), g_px_regs.load(), g_px_waits.load(), g_px_unhandled.load());
  std::vector<std::pair<uint32_t, uint32_t>> top;
  uint32_t regs_bad = 0;
  for (uint32_t r = 0; r < 0x5000; ++r)
    if (g_fe_mis[r] && !(r >= 0x21F9 && r <= 0x21FC)) { ++regs_bad; top.push_back({g_fe_mis[r], r}); }   // 21F9-21FC: the draw packet's own
  std::sort(top.rbegin(), top.rend());
  std::string s2;
  for (size_t k = 0; k < top.size() && k < 24; ++k) s2 += fmt::format(" {:04X}:{}", top[k].second, top[k].first);
  std::string ops;
  for (int o = 0; o < 128; ++o)
    if (const uint64_t v = g_fe_ops[o].load()) ops += fmt::format(" {:02X}:{}", o, v);
  size_t pending = 0;
  { std::lock_guard<std::mutex> lock(g_fe_mu); for (auto& kv : g_fe_snaps) pending += kv.second.size(); }
  REXLOG_INFO("[p3fe] {} ({}): bridge draws seen {}, compared {}, without a snapshot {}; front end {} kicks / {} decode windows, "
              "skipped {}, saw {} draws, predicated skips {}, {} snapshots pending; constant mismatches {} from memory loads "
              "({} whose source now holds the bridge value), {} from packets; {} registers ever differ (21F9-21FC excluded), "
              "worst:{} | early stops: unfilled ring {} ib {}, short ring {} ib {}, ib unreadable {}, ib too deep {}; ring resets {}, resyncs {}, expired snapshots {}, fe frames {} | type-3 ops:{}",
              why, g_fe_kick_mode.load() ? "kick mode" : "cursor mode", g_fe_seen.load(), g_fe_compared.load(),
              g_fe_unmatched.load(), g_fe_kicks.load(), g_fe_calls.load(), g_fe_skipped.load(), g_fe_draws.load(),
              g_fe_predicated_skips.load(), pending, g_fe_mis_memsrc, g_fe_mis_memsrc_now_bridge, g_fe_mis_packet,
              regs_bad, s2, g_fe_stop_unfilled_ring.load(), g_fe_stop_unfilled_ib.load(), g_fe_stop_short_ring.load(),
              g_fe_stop_short_ib.load(), g_fe_ib_unreadable.load(), g_fe_ib_deep.load(), g_fe_ring_resets.load(),
              g_fe_resyncs.load(), g_fe_expired.load(), g_fe_frame, ops);
}

void Write() {
  if (g_fe) FeReport("census window end");
  if (g_p5_f) {
    std::fclose(g_p5_f);
    g_p5_f = nullptr;
    REXLOG_INFO("[p5] wrote p5_fe.bin: {} side effects", g_p5_n);
    g_p5 = false;   // closed for good: a later note must not reopen (and truncate) the file
  }
  if (g_src) {
    std::lock_guard<std::mutex> lock2(g_src_mu);
    if (g_src_f) { std::fclose(g_src_f); g_src_f = nullptr; }
    if (g_src_fe_f) { std::fclose(g_src_fe_f); g_src_fe_f = nullptr; }
    REXLOG_INFO("[p3src] wrote p3_src.bin ({} calls) and p3_fe.bin ({} draws) for guest frame {}", g_src_calls, g_src_draws, g_src_frame);
  }
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

void Enter(int hook, uint32_t r3, bool lib, const uint32_t* args8) {
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
  if (args8) std::memcpy(o.args, args8, sizeof(o.args)); else std::memset(o.args, 0, sizeof(o.args));
  if (!ReadGuest(dev + g_cursor_off, &o.cur_in)) { o.cur_in = 0; g_noread.fetch_add(1, std::memory_order_relaxed); }
  if (g_fe) FeAdvance(o.cur_in);
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
  if (g_fe) FeAdvance(r.cur_out);
  if (!ReadGuest(dev + g_cursor2_off, &r.cur2_out)) r.cur2_out = 0;
  // [p3 fe] decode what this call wrote into the front end's register file (every call, from the device on).
  // Only at the OUTERMOST exit on this thread: an outer call's range contains its inner calls' bytes, so decoding at
  // every exit would apply them twice (and count their draws twice).
  std::lock_guard<std::mutex> lock(g_mu);
  if (recording) g_recs.push_back(r);
  // [p3 src] the call's arguments and what they point at, in the census frame.
  if (g_src && r.frame == g_src_frame && g_src_calls < 6000) {
    std::lock_guard<std::mutex> lock2(g_src_mu);
    if (!g_src_f) g_src_f = std::fopen("p3_src.bin", "wb");
    if (g_src_f) {
      auto* ks = rex::system::kernel_state();
      static uint8_t sample[8][256];
      uint32_t which[8], n = 0;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t a = o.args[k];
        const bool ptr_like = (a >= 0x40000000u && a < 0x90000000u) || (a >= 0xA0000000u && a < 0xE1000000u);
        if (!ptr_like || !ks || !ks->memory()) continue;
        const uint8_t* p = ks->memory()->TranslateVirtual<const uint8_t*>(a & ~3u);
        // The runtime's guest access-violation handler runs before a structured-exception guard here (leg ng2_081
        // died reading 0x81000008), so the host pages are checked first: committed and readable, both pages the
        // 256 bytes may span.
        auto readable = [](const void* q) {
          MEMORY_BASIC_INFORMATION mbi;
          if (!VirtualQuery(q, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
          const DWORD pr = mbi.Protect & 0xFF;
          return pr == PAGE_READONLY || pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE;
        };
        if (p && readable(p) && readable(p + 255) && SafeCopy(sample[n], p, 256)) which[n++] = k;
      }
      const uint32_t hdr[13] = {uint32_t(o.hook), r.tid, r.cur_in, r.cur_out, o.args[0], o.args[1], o.args[2], o.args[3],
                                o.args[4], o.args[5], o.args[6], o.args[7], n};
      std::fwrite(hdr, sizeof(hdr), 1, g_src_f);
      for (uint32_t s = 0; s < n; ++s) {
        const uint32_t sh[2] = {which[s], o.args[which[s]] & ~3u};
        std::fwrite(sh, sizeof(sh), 1, g_src_f);
        std::fwrite(sample[s], 1, 256, g_src_f);
      }
      ++g_src_calls;
    }
  }
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
      // Format v3: the bytes are [cur_in + 4, cur_out + 4) - the cursor names the last dword written (see the
      // front end above); p3_flush2.py reads them with --base_shift 1.
      if (r.cur_out > r.cur_in && r.cur_out - r.cur_in <= 16384u)
        q = ks->memory()->TranslateVirtual<const uint8_t*>(r.cur_in + 4);
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
    if (g_fe_seen.fetch_add(1, std::memory_order_relaxed) % 20000 == 19999) FeReport("every 20000 bridge draws");
    FeSnap* s = nullptr;
    const uint32_t a = packet_addr & 0x1FFFFFFFu;
    const uint32_t ord = g_br_ord[a]++;
    {
      std::lock_guard<std::mutex> lock(g_fe_mu);
      auto it = g_fe_snaps.find((uint64_t(a) << 32) | ord);
      if (it != g_fe_snaps.end()) {
        while (!it->second.empty() && it->second.front()->frame + 3 < g_fe_frame) {   // stale: expired
          delete it->second.front();
          it->second.pop_front();
          g_fe_expired.fetch_add(1, std::memory_order_relaxed);
        }
        if (!it->second.empty()) { s = it->second.front(); it->second.pop_front(); }
        if (it->second.empty()) g_fe_snaps.erase(it);
      }
    }
    if (!s) {
      g_fe_unmatched.fetch_add(1, std::memory_order_relaxed);
    } else {
      for (uint32_t k = 0; k < 0x400; ++k)
        if (s->r2[k] != regs[0x2000 + k]) ++g_fe_mis[0x2000 + k];
      for (uint32_t k = 0; k < 0x928; ++k)
        if (s->r4[k] != regs[0x4000 + k]) {
          ++g_fe_mis[0x4000 + k];
          // Timing test: a memory-loaded constant whose SOURCE now holds the bridge's value = the plugin read the
          // memory later than the front end (the game had rewritten it in between).
          if (s->src4[k]) {
            ++g_fe_mis_memsrc;
            auto* ks = rex::system::kernel_state();
            const uint8_t* p = ks && ks->memory() ? ks->memory()->TranslatePhysical<const uint8_t*>(s->src4[k]) : nullptr;
            if (p && ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3])) == regs[0x4000 + k])
              ++g_fe_mis_memsrc_now_bridge;
          } else {
            ++g_fe_mis_packet;
          }
        }
      // The first compared draws in full: which registers differ, with both values (front end / bridge).
      // ... and four more once the warm-up is over (leg ng2_075: the first four differed only in 21F9-21FC while the
      // steady state had 362 registers differing - the values are the evidence).
      static std::atomic<int> dumped{0};
      static std::atomic<int> dumped_late{0};
      const bool late = g_fe_compared.load(std::memory_order_relaxed) > 100000 && dumped_late.load() < 4;
      if (dumped.fetch_add(1) < 4 || (late && dumped_late.fetch_add(1) < 4)) {
        std::string d;
        int shown = 0;
        for (uint32_t k = 0; k < 0x400 && shown < 40; ++k)
          if (s->r2[k] != regs[0x2000 + k]) { d += fmt::format(" {:04X}:{:08X}/{:08X}", 0x2000 + k, s->r2[k], regs[0x2000 + k]); ++shown; }
        for (uint32_t k = 0; k < 0x928 && shown < 60; ++k)
          if (s->r4[k] != regs[0x4000 + k]) { d += fmt::format(" {:04X}:{:08X}/{:08X}", 0x4000 + k, s->r4[k], regs[0x4000 + k]); ++shown; }
        REXLOG_INFO("[p3fe] draw at {:08X}: front-end/bridge differing registers:{}", packet_addr, d);
      }
      delete s;
      const uint64_t c = g_fe_compared.fetch_add(1, std::memory_order_relaxed) + 1;
      // Steady state: the counts restart after the first 100,000 comparisons (start-up, before the front end has seen
      // the state the game set before its first kick, is not the question).
      if (c == 100000) {
        std::memset(g_fe_mis, 0, sizeof(g_fe_mis));
        g_fe_mis_memsrc = g_fe_mis_memsrc_now_bridge = g_fe_mis_packet = 0;
        REXLOG_INFO("[p3fe] warm-up over: mismatch counts restart");
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
  const uint32_t bf = g_bridge_frame.load(std::memory_order_relaxed);   // format v3: the frame stamp
  std::fwrite(&bf, 4, 1, g_p3_bridge_f);
  std::fwrite(regs + 0x2000, 4, 0x400, g_p3_bridge_f);
  std::fwrite(regs + 0x4000, 4, 0x928, g_p3_bridge_f);
  if (++g_p3_bridge % 64 == 0) std::fflush(g_p3_bridge_f);
  if (g_p3_bridge == 4 * g_p3_max) {
    std::fclose(g_p3_bridge_f);
    g_p3_bridge_f = nullptr;
    REXLOG_INFO("[p3] wrote p3_bridge.bin: {} bridge draws inside recorded call ranges", g_p3_bridge);
  }
}

void BridgeSwap() {
  g_bridge_frame.fetch_add(1, std::memory_order_relaxed);
  g_br_ord.clear();   // the bridge's per-address execution ordinals restart with its frame
}

bool FrontEndDraws() { return g_p3draw; }

// PRODUCTION SWITCH (NG2_NATIVE_FE=1): the native front end without the census - the kick callback drives it,
// it draws on the guest thread, and the plugin's command processor only executes its side effects.
void NativeKick(uint32_t ring_ptr, uint32_t ring_bytes, uint32_t wptr) { FeKick(ring_ptr, ring_bytes, wptr); }

bool StartNativeFrontEnd() {
  if (ng2::gs::Active()) {   // the game's own graphics system: no plugin, it kicks the front end directly
    if (g_fe && g_p5exec) return true;
    g_fe_compare = false;
    g_p3draw = true;
    g_push = &ng2::gs::PushSideEffects;
    g_p5exec = true;
    g_fe = true;
    g_fe_kick_mode = true;
    REXLOG_INFO("[native] NATIVE FRONT END ON (own graphics system): kicks decoded and drawn on the game's thread, "
                "side effects executed by the game's executor thread - rexgpu-xenos is not the graphics system");
    return true;
  }
  const char* e = std::getenv("NG2_NATIVE_FE");
  if (!e || !*e || *e == '0') return false;
  if (g_fe && g_p5exec) return true;   // the census path already started it
  HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
  using SetKickFn = void (*)(void (*)(uint32_t, uint32_t, uint32_t));
  auto kick = m ? reinterpret_cast<SetKickFn>(GetProcAddress(m, "RexNgpuSetKickCallback")) : nullptr;
  auto setx = m ? reinterpret_cast<void (*)(int)>(GetProcAddress(m, "RexNgpuSetExecMode")) : nullptr;
  auto push = m ? reinterpret_cast<PushFn>(GetProcAddress(m, "RexNgpuPushSideEffects")) : nullptr;
  if (!kick || !setx || !push) {
    REXLOG_INFO("[native] NG2_NATIVE_FE refused: the plugin lacks {}", !kick ? "RexNgpuSetKickCallback"
                : !setx ? "RexNgpuSetExecMode" : "RexNgpuPushSideEffects");
    return false;
  }
  g_fe_compare = false;
  g_p3draw = true;
  g_push = push;
  g_p5exec = true;
  g_fe = true;
  kick(&FeKick);
  g_fe_kick_mode = true;
  setx(1);
  REXLOG_INFO("[native] NATIVE FRONT END ON: the game's kicks are decoded on its own thread, drawn there, and the "
              "plugin only executes the side effects (fences, interrupts, the swap)");
  return true;
}
void FrontEndCounts(uint64_t& draws, uint64_t& swaps) { draws = g_fe_draws_issued.load(); swaps = g_fe_swaps_issued.load(); }

}  // namespace ng2::p2

extern "C" void ng2_p2_exit(int hook) { ng2::p2::Exit(hook); }
