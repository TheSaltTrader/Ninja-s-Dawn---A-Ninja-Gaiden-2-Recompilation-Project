#include "ng2_native_gpu.h"

#include "ng2_plume_renderer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ng2::ngpu {
namespace {

// The hand-off ABI, declared HERE rather than included from the SDK.
//
// Not a workaround for the exe building against the installed headers - it is
// the right shape. The record carries struct_size precisely so a plugin and an
// executable built apart can detect a mismatch, and sharing the header makes
// that field untestable: it would be right by construction, and the first time
// the two really did diverge there would be nothing to catch it. A consumer in
// another codebase would declare it exactly like this, so this one does.
//
// Must match rex/system/gpu_plugin.h. If a field is added there and not here,
// struct_size differs and OnDraw counts a mismatch instead of reading garbage -
// which is the contract working, not failing.
struct GpuDrawRecord {
  uint32_t struct_size;

  uint32_t vgt_draw_initiator;
  uint32_t index_base;
  uint32_t index_size_words;
  uint32_t index_endian;

  uint32_t vs_address, vs_dwords;
  uint32_t ps_address, ps_dwords;
  uint32_t vs_immediate, ps_immediate;

  // The immediate microcode and a per-stage load counter. See constraint 8 in
  // ng2_plume_renderer.h: for an immediate draw the ADDRESS fields above are
  // stale rather than empty, so they cannot be used, and the bytes are the only
  // identity this draw's shader has. Raw guest bytes, big-endian.
  const uint8_t* vs_ucode;
  uint32_t vs_ucode_dwords;
  const uint8_t* ps_ucode;
  uint32_t ps_ucode_dwords;
  uint64_t vs_ucode_serial;
  uint64_t ps_ucode_serial;

  const uint32_t* registers;
  uint32_t register_count;

  uint64_t bin_mask, bin_select;
  uint32_t predicated;

  // This draw's ordinal in the stream, from 1, counted by the plugin at the
  // hand-off site. See OnDraw: checking it is how this stage proves the
  // boundary without a second instrument to compare against.
  uint64_t draw_serial;
};

using GpuDrawFn = void (*)(const GpuDrawRecord*);
using GpuSetDrawCallbackFn = void (*)(GpuDrawFn);
constexpr const char* kSetDrawCallbackSymbol = "rex_gpu_set_draw_callback";

// The frame boundary, on the GPU worker thread - the same thread the draws
// arrive on. Declared locally for the same reason the record is: a consumer in
// another codebase would have to.
using GpuSwapFn = void (*)(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);
using GpuSetSwapCallbackFn = void (*)(GpuSwapFn);
constexpr const char* kSetSwapCallbackSymbol = "rex_gpu_set_swap_callback";

// VGT_DRAW_INITIATOR, decoded from the bit positions rather than from a struct,
// for the same reason: no SDK header dependency on the consumer side.
//   prim_type 0:5   source_select 6:7   index_size bit 11   num_indices 16:31
// source_select: 0 = kDMA (an index buffer), 1 = kImmediate, 2 = kAutoIndex.
constexpr uint32_t kSourceDMA = 0;

// STAGE 1: count only.
//
// The renderer is deliberately not wired up yet. What has to be established
// first is that the exe and the plugin agree - that the export resolves, the
// record crosses the module boundary intact, and the draws arriving here are
// the same draws the plugin executes. A renderer built on an unverified
// hand-off would attribute its own missing geometry to itself.
//
// The plugin's own NGPU_DRAW_SELFTEST already reconciled an internal consumer
// against its census (2,347 draws / 764,123 indices, exact). This is the same
// reconciliation across the DLL boundary, which an internal consumer cannot
// test.
std::atomic<uint64_t> g_draws{0};
std::atomic<uint64_t> g_indices{0};
std::atomic<uint64_t> g_indexed{0};
std::atomic<uint64_t> g_auto{0};
std::atomic<uint64_t> g_bad_size{0};
std::atomic<uint64_t> g_no_regs{0};
std::atomic<uint64_t> g_no_shader{0};

// The serial check. g_last_serial is touched only from the GPU worker thread
// (the callback's thread), so it needs no atomic; the tallies are read by the
// reporter thread and do.
uint64_t g_last_serial = 0;
std::atomic<uint64_t> g_gaps{0};       // records that skipped at least one serial
std::atomic<uint64_t> g_missing{0};    // how many draws those gaps account for
std::atomic<uint64_t> g_repeats{0};    // a serial that did not advance
std::atomic<uint64_t> g_first_gap{0};  // the serial we were at when the first gap appeared

// Stage 2b step 1: index-buffer resolution tallies. Declared here because
// Totals() reports them.
std::atomic<uint64_t> g_idx_virtual_ok{0};
std::atomic<uint64_t> g_idx_physical_ok{0};
std::atomic<uint64_t> g_idx_both{0};
std::atomic<uint64_t> g_idx_neither{0};
std::atomic<uint64_t> g_idx_checked{0};
std::atomic<uint64_t> g_idx_faulted{0};  // both interpretations faulted on read
// PER INTERPRETATION, because "it faulted" without saying WHICH translation
// faulted cannot distinguish "this address is bad" from "one of the two ways of
// reading it is bad" - and that distinction is the whole question here.
std::atomic<uint64_t> g_idx_virt_faulted{0};
std::atomic<uint64_t> g_idx_phys_faulted{0};
// The first draw's inputs, for the report. Logging from the draw callback does
// not reach the file, so what the probe was HANDED has to travel this way.
std::atomic<uint32_t> g_idx_sample_base{0};
std::atomic<uint32_t> g_idx_sample_words{0};
std::atomic<uint32_t> g_idx_sample_endian{0};

// Draws the availability probe would have REFUSED while a usable pipeline
// existed. Declared here, with the other tallies, because Totals() reports it -
// a counter declared below the function that prints it is the third instance of
// that mistake on this project.
std::atomic<uint64_t> g_would_skip{0};

std::atomic<uint64_t> g_ucode_read_ok{0};
std::atomic<uint64_t> g_ucode_read_faulted{0};
std::atomic<uint32_t> g_ucode_first_addr{0};
std::atomic<uint32_t> g_ucode_first_word{0};

// Read once at Start(), not per draw: this is on the per-draw path.
bool g_check_inputs = false;
bool g_check_surface = false;

// THE SURFACE CENSUS.
//
// Xenos register indices, from the SDK's register_table.inc - declared here
// rather than included for the same reason the draw record is: this is a
// consumer in another codebase, and a constant that is right by construction
// cannot catch a divergence.
constexpr uint32_t kRegRbSurfaceInfo = 0x2000;
constexpr uint32_t kRegRbColorInfo = 0x2001;
constexpr uint32_t kRegRbDepthInfo = 0x2002;
constexpr uint32_t kRegPaScWindowScissorTL = 0x2081;
constexpr uint32_t kRegPaScWindowScissorBR = 0x2082;
constexpr uint32_t kRegRbModecontrol = 0x2208;

// THE SCISSOR IS NOT PART OF RENDER-TARGET IDENTITY, and folding it in was a
// design error the overflow counter caught: with tl/br in the key the census
// reported "24 distinct (+399,552 OVERFLOW)" - more draws spilled than the
// whole table held - because the scissor is PER-DRAW state that changes
// constantly while the target underneath does not. Two different things were
// being counted as one, and the row count exploded.
//
// A render target is the surface: pitch, msaa, colour base and format, depth
// base, edram mode. The scissor is kept alongside as a RANGE, so its variation
// is still reported without becoming a row per value.
struct SurfaceKey {
  uint32_t pitch, msaa, color_base, color_format, depth_base, edram_mode;
  bool operator==(const SurfaceKey& o) const {
    return pitch == o.pitch && msaa == o.msaa && color_base == o.color_base &&
           color_format == o.color_format && depth_base == o.depth_base &&
           edram_mode == o.edram_mode;
  }
};

// A FIXED TABLE WITH AN OVERFLOW COUNTER, not a growing map. This runs on the
// draw callback for every draw - 22 million in a four-minute run - so it must
// not allocate. The cap means the census can be INCOMPLETE, and an incomplete
// census that does not say so is the failure this project keeps finding, hence
// g_surf_overflow: distinct surfaces that did not fit are counted, never
// dropped silently.
constexpr int kMaxSurfaces = 192;
std::mutex g_surf_mutex;
SurfaceKey g_surf_keys[kMaxSurfaces];
uint64_t g_surf_draws[kMaxSurfaces] = {};
// The scissor as a RANGE per surface, so per-draw variation stays visible
// without becoming a separate row for every value it takes.
uint32_t g_surf_w_min[kMaxSurfaces] = {}, g_surf_w_max[kMaxSurfaces] = {};
uint32_t g_surf_h_min[kMaxSurfaces] = {}, g_surf_h_max[kMaxSurfaces] = {};
int g_surf_n = 0;
std::atomic<uint64_t> g_surf_overflow{0};
std::atomic<uint64_t> g_surf_no_regs{0};

// THE BIN CENSUS. Same shape as the surface one: a fixed table, an overflow
// counter, and every distinct tuple as its own row rather than a summary.
// THE WINDOW OFFSET AND THE COLOUR SURFACE BELONG IN THIS KEY.
//
// I reported "NG2 does not bin-partition" from mask == select == 0xFFFFFFFF on
// every draw. The sibling project's bins ARE vertical screen tiles - window
// offset Y -512 with scissor Y +512, the second tile of a 1280x720 target - and
// crucially their FOUR zero-offset bins differ in the colour surface they write.
// So the bin field carries at least two orthogonal things, and a uniform MASK
// is consistent with two different worlds:
//
//   one bin covering the frame        -> NG2 genuinely does not tile
//   tiling expressed somewhere else   -> NG2 tiles and I could not see it
//
// A non-zero PA_SC_WINDOW_OFFSET anywhere in a frame settles it whatever the
// masks say, so it goes in the key rather than being assumed constant.
constexpr uint32_t kRegPaScWindowOffset = 0x2080;

struct BinKey {
  uint64_t mask, select;
  uint32_t predicated;
  uint32_t window_offset;   // X bits 14:0, Y bits 30:16, both signed 15-bit
  uint32_t color_base;      // which surface this bin writes
  bool operator==(const BinKey& o) const {
    return mask == o.mask && select == o.select && predicated == o.predicated &&
           window_offset == o.window_offset && color_base == o.color_base;
  }
};
constexpr int kMaxBins = 16;
std::mutex g_bin_mutex;
BinKey g_bin_keys[kMaxBins];
uint64_t g_bin_draws[kMaxBins] = {};
int g_bin_n = 0;
std::atomic<uint64_t> g_bin_overflow{0};

void NoteBin(const GpuDrawRecord* rec) {
  uint32_t wo = 0, cb = 0;
  if (rec->registers && rec->register_count > kRegRbColorInfo) {
    wo = rec->registers[kRegPaScWindowOffset];
    cb = rec->registers[kRegRbColorInfo] & 0xFFF;
  }
  BinKey k{rec->bin_mask, rec->bin_select, rec->predicated, wo, cb};
  std::lock_guard<std::mutex> lock(g_bin_mutex);
  for (int i = 0; i < g_bin_n; ++i) {
    if (g_bin_keys[i] == k) {
      ++g_bin_draws[i];
      return;
    }
  }
  if (g_bin_n >= kMaxBins) {
    g_bin_overflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_bin_keys[g_bin_n] = k;
  g_bin_draws[g_bin_n] = 1;
  ++g_bin_n;
}

std::string BinReport() {
  std::string out;
  std::lock_guard<std::mutex> lock(g_bin_mutex);
  if (!g_bin_n) return out;
  out = fmt::format(" | BINS {} distinct", g_bin_n);
  if (const uint64_t o = g_bin_overflow.load()) out += fmt::format(" (+{} OVERFLOW)", o);
  for (int i = 0; i < g_bin_n; ++i) {
    // The offset DECODED, because 7E000000 is not readable as "Y -512" and
    // whether any of them is non-zero is the entire question.
    const int32_t ox = int32_t(g_bin_keys[i].window_offset << 17) >> 17;
    const int32_t oy = int32_t(g_bin_keys[i].window_offset & 0x7FFF0000) >> 16;
    out += fmt::format("\n    bin mask {:016X} select {:016X} pred {} | offset {:08X}"
                       " (x {} y {}) colour base {} | draws {}",
                       g_bin_keys[i].mask, g_bin_keys[i].select, g_bin_keys[i].predicated,
                       g_bin_keys[i].window_offset, ox, oy, g_bin_keys[i].color_base,
                       g_bin_draws[i]);
  }
  return out;
}

// Declared here because the stream census below uses it and its definition sits
// with the other guest-memory probes further down. The alternative - moving the
// definition up - would separate it from the comment explaining why it needs
// SEH at all.
bool ReadGuestBytes(const uint8_t* p, uint32_t bytes, uint8_t* out);

// THE FETCH SLOTS A PROGRAM USES, decoded from its microcode once and cached.
//
// A vfetch names its constant in dword 0 (const_index bits 20..24,
// const_index_sel bits 25..26), and the slot is const_index * 3 + select. The
// control flow says which 3-dword groups are fetches rather than ALU
// instructions - the same walk const_scan.py does offline, and for the same
// reason: the two share an array and only the exec blocks tell them apart.
struct FetchSlots {
  uint8_t count = 0;
  uint8_t slot[24] = {};
  // The STRIDE each fetch reads with, dword 2 bits 0..7, in dwords. Needed
  // because a fetch constant's declared size is the buffer, not what a draw
  // reads from it, and copying the declared size copies whole buffers where a
  // few elements were wanted.
  uint8_t stride[24] = {};
};

// KEYED BY WHAT THE CONSUMER HAS, not by a content hash it would have to
// compute: a by-pointer program is identified by its address, and an immediate
// one only by the load serial the plugin carries. The renderer owns hashing;
// duplicating it here would be a second key that can disagree with the first,
// which is what constraint 2 is about.
std::mutex g_fetch_mutex;
std::map<uint32_t, FetchSlots> g_fetch_by_address;   // by-pointer vertex shaders
std::atomic<uint64_t> g_fetch_programs{0};
std::atomic<uint64_t> g_fetch_slots_total{0};

// The slots the CURRENT vertex shader fetches from. Touched only on the GPU
// worker thread, so no atomics - the same reasoning as g_last_serial.
FetchSlots g_cur_slots;
uint32_t g_cur_slots_address = 0xFFFFFFFFu;
uint64_t g_cur_slots_serial = 0;

uint32_t BeDword(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void DecodeFetchSlots(const uint8_t* ucode, uint32_t bytes, FetchSlots* out) {
  out->count = 0;
  const uint32_t n = bytes / 4;
  if (n < 3) return;
  uint32_t limit = n;
  for (uint32_t i = 0; i + 3 <= n && i < limit; i += 3) {
    const uint64_t d0 = BeDword(ucode + i * 4);
    const uint64_t d1 = BeDword(ucode + (i + 1) * 4);
    const uint64_t d2 = BeDword(ucode + (i + 2) * 4);
    const uint64_t cf[2] = {d0 | ((d1 & 0xFFFF) << 32), (d1 >> 16) | (d2 << 16)};
    for (int k = 0; k < 2; ++k) {
      const uint32_t op = uint32_t((cf[k] >> 44) & 0xF);
      // exec-like opcodes, the only ones that carry an instruction block
      if (op != 1 && op != 2 && op != 3 && op != 4 && op != 5 && op != 6 &&
          op != 13 && op != 14) {
        continue;
      }
      const uint32_t addr = uint32_t(cf[k] & 0xFFF);
      const uint32_t count = uint32_t((cf[k] >> 12) & 0x7);
      uint32_t seq = uint32_t((cf[k] >> 16) & 0xFFFFFF);
      if (addr) limit = addr * 3 < limit ? addr * 3 : limit;
      for (uint32_t j = 0; j < count; ++j, seq >>= 2) {
        if (!(seq & 1)) continue;                 // bit 0 of each pair: FETCH
        const uint32_t at = (addr + j) * 3;
        if (at + 3 > n) continue;
        const uint32_t w0 = BeDword(ucode + at * 4);
        const uint32_t const_index = (w0 >> 20) & 0x1F;
        const uint32_t select = (w0 >> 25) & 0x3;
        if (select > 2) continue;                 // a group holds three fetches
        const uint8_t slot = uint8_t(const_index * 3 + select);
        const uint32_t w2 = BeDword(ucode + (at + 2) * 4);
        const uint8_t stride = uint8_t(w2 & 0xFF);     // dword 2 bits 0..7, dwords
        bool dup = false;
        for (uint8_t q = 0; q < out->count; ++q) {
          if (out->slot[q] == slot) { dup = true; break; }
        }
        if (!dup && out->count < 24) {
          out->stride[out->count] = stride;
          out->slot[out->count++] = slot;
        }
      }
    }
  }
}

// Called from the registration path, where the microcode is already in hand.
void RememberFetchSlots(uint32_t address, const uint8_t* ucode, uint32_t bytes) {
  FetchSlots fs;
  DecodeFetchSlots(ucode, bytes, &fs);
  std::lock_guard<std::mutex> lock(g_fetch_mutex);
  if (g_fetch_by_address.emplace(address, fs).second) {
    g_fetch_programs.fetch_add(1, std::memory_order_relaxed);
    g_fetch_slots_total.fetch_add(fs.count, std::memory_order_relaxed);
  }
}

// Make the current program's slot list current, decoding only when the program
// actually changes rather than once per draw.
void SelectFetchSlots(const GpuDrawRecord* rec) {
  if (rec->vs_immediate) {
    if (rec->vs_ucode && rec->vs_ucode_dwords &&
        rec->vs_ucode_serial != g_cur_slots_serial) {
      g_cur_slots_serial = rec->vs_ucode_serial;
      g_cur_slots_address = 0xFFFFFFFFu;
      DecodeFetchSlots(rec->vs_ucode, rec->vs_ucode_dwords * 4, &g_cur_slots);
    }
    return;
  }
  if (rec->vs_address == g_cur_slots_address) return;
  g_cur_slots_address = rec->vs_address;
  std::lock_guard<std::mutex> lock(g_fetch_mutex);
  auto it = g_fetch_by_address.find(rec->vs_address);
  // A program whose microcode was never readable has no slot list, and an EMPTY
  // list is the honest answer - it means this draw contributes no streams, not
  // that it contributes all 96.
  g_cur_slots = (it != g_fetch_by_address.end()) ? it->second : FetchSlots{};
}

// THE VERTEX STREAM CENSUS.
//
// 32 fetch slots, 2 dwords each, from SHADER_CONSTANT_FETCH_00_0. A slot whose
// address is zero is unused; the rest name guest memory holding vertex data.
constexpr uint32_t kRegShaderConstantFetch00 = 0x4800;
// NINETY-SIX, NOT THIRTY-TWO. The fetch constant file is 32 groups of 6 dwords,
// and a group holds EITHER one texture fetch (6 dwords) OR three vertex fetches
// of 2 dwords each - which is why the recompiler's stream index is
// 95 - (constIndex * 3 + select). Scanning 32 slots covered a sixth of the file.
constexpr uint32_t kFetchSlots = 96;
// A slot that is not currently a vertex fetch decodes to nonsense, and there is
// nothing in the constants alone that says which is which. So implausible
// results are REJECTED AND COUNTED rather than filtered away silently: a stream
// larger than this is not geometry, it is a texture constant being read as one.
constexpr uint32_t kMaxStreamBytes = 16u << 20;

// THE SIZE IS NOT PART OF THE KEY.
//
// Keying on (address, size) counts DRAW SHAPES, not buffers: one vertex buffer
// read with different lengths by different draws becomes an entry per draw, and
// the working set becomes the number of distinct draws. That is precisely the
// symptom here - 349 "distinct streams" per frame and 3.3 million overflow over
// a run. The sibling project hit it and keys on
// (base << 32) | (stride << 16) | (endian << 8) | kind, handling length with
// `entry->bytes >= len` and rebuilding in place when a request is longer.
//
// This census keys on the ADDRESS and tracks the LARGEST size seen for it,
// which is the buffer the cache would have to hold.
struct StreamKey {
  uint32_t address;   // guest byte address
  uint32_t bytes;     // the largest length any draw has read from it
  // The two rival extents, tracked ALONGSIDE the declared size rather than
  // instead of it, so the three can be compared on one run instead of each
  // needing its own.
  uint32_t count_bytes = 0;   // max over draws of draw_count * stride
  uint32_t safe_bytes = 0;    // max over draws of (max_index + 1) * stride
  bool operator==(const StreamKey& o) const { return address == o.address; }
};

constexpr int kMaxStreams = 512;  // raised: 64 exactly matched the old cap
std::mutex g_stream_mutex;
StreamKey g_stream_keys[kMaxStreams];
uint64_t g_stream_uses[kMaxStreams] = {};
int g_stream_n = 0;
std::atomic<uint64_t> g_stream_overflow{0};
std::atomic<uint64_t> g_stream_readable{0};
std::atomic<uint64_t> g_stream_unreadable{0};
std::atomic<uint64_t> g_stream_bytes{0};
// Slots whose decode is not plausibly geometry - a texture constant read as a
// vertex fetch. Counted, because "we ignored some" and "there were none"
// must not print the same number.
std::atomic<uint64_t> g_stream_rejected{0};
// WHERE THE BYTES COME FROM. The per-frame total read 957 MB against the
// plugin's own ~17 MB/frame, and keying by address alone brought it to 66 MB -
// still high. A histogram attributes the remainder instead of inviting another
// guess at the plausibility bound.
std::atomic<uint64_t> g_size_bucket[8] = {};   // <64B, <256, <1K, <4K, <16K, <64K, <1M, >=1M
std::atomic<uint64_t> g_stream_clamped{0};

// PER FRAME, which is the number that sizes a cache. The run total is
// unbounded - 512 distinct with 3,252,245 overflow - but a cache only has to
// hold what ONE frame references, and whether that is 40 or 4,000 decides
// whether eviction is needed at all. Reset at the frame boundary; the high
// water mark is what the cache must be built for.
// A PER-FRAME SET THAT IS INDEPENDENT OF THE CUMULATIVE TABLE.
//
// Stamping entries of the cumulative table reported "PER FRAME peak 6 streams"
// across millions of draws, and that number is an artefact: the table is
// first-come-first-served and 512 entries deep, so it FILLED DURING BOOT and
// every address gameplay uses overflows before it can be stamped. The per-frame
// figure was measuring which boot-time streams were still being referenced, not
// what a frame needs.
//
// So the per-frame count gets its own structure, cleared every frame and large
// enough not to be the thing being measured: open addressing, power-of-two,
// with a miss counted rather than silently dropped.
constexpr int kFrameSetBits = 13;                 // 8192 slots
constexpr uint32_t kFrameSetMask = (1u << kFrameSetBits) - 1;
uint32_t g_frame_set_addr[1u << kFrameSetBits] = {};
uint32_t g_frame_set_size[1u << kFrameSetBits] = {};
int g_stream_frame_n = 0;
std::atomic<uint64_t> g_frame_set_full{0};

// Returns true if this (address,size) is new THIS FRAME.
bool FrameSetInsert(uint32_t address, uint32_t bytes) {
  uint32_t h = (address * 2654435761u) ^ (bytes * 40503u);
  for (int probe = 0; probe < 64; ++probe) {
    const uint32_t i = (h + uint32_t(probe)) & kFrameSetMask;
    if (!g_frame_set_addr[i]) {
      g_frame_set_addr[i] = address;
      g_frame_set_size[i] = bytes;
      return true;
    }
    if (g_frame_set_addr[i] == address && g_frame_set_size[i] == bytes) return false;
  }
  g_frame_set_full.fetch_add(1, std::memory_order_relaxed);
  return false;
}
std::atomic<uint64_t> g_stream_frame_max{0};
std::atomic<uint64_t> g_stream_frame_bytes_max{0};
// THE MEAN, beside the peak. A peak compared against another project's
// per-frame rate is not a comparison, and the 66 MB vs 17 MB gap may be
// entirely that.
std::atomic<uint64_t> g_stream_frame_bytes_sum{0};
std::atomic<uint64_t> g_stream_frames_counted{0};
uint64_t g_stream_frame_bytes = 0;

// THE LARGEST INDEX A DRAW REFERENCES, cached per index buffer.
//
// Needed because `index_count * stride` is only an upper bound on the bytes a
// draw touches when the indices happen to be dense and ordered. They need not
// be. The only way to know is to look, and looking is affordable exactly once
// per (buffer, length, format) - the same caching argument as the fetch-slot
// decode, which would otherwise have run eight million times.
//
// Returns false if the buffer could not be read at all; the caller must then
// fall back rather than treat 0 as "no indices".
struct IndexKey {
  uint32_t base, words;
  uint8_t fmt32, endian;
  // THE RESTART STATE IS PART OF THE KEY. The reset index and its enable are
  // registers, so two draws over the SAME index buffer can disagree about which
  // values are vertices. Keyed without them, the first draw's answer is served
  // to the second and the cache quietly reports the wrong maximum.
  uint32_t reset_indx;
  uint8_t restart_on;
  bool operator<(const IndexKey& o) const {
    if (base != o.base) return base < o.base;
    if (words != o.words) return words < o.words;
    if (fmt32 != o.fmt32) return fmt32 < o.fmt32;
    if (endian != o.endian) return endian < o.endian;
    if (reset_indx != o.reset_indx) return reset_indx < o.reset_indx;
    return restart_on < o.restart_on;
  }
};

// Register indices, from the engine's own table rather than from memory.
constexpr uint32_t kRegVgtMultiPrimIbResetIndx = 0x2103;
constexpr uint32_t kRegPaSuScModeCntl = 0x2205;

// Only these primitive types can carry a restart index; the engine filters the
// list topologies out explicitly because Vulkan disallows restart on them.
bool PrimitiveTakesRestart(uint32_t prim_type) {
  switch (prim_type) {
    case 0x03:  // kLineStrip
    case 0x05:  // kTriangleFan
    case 0x06:  // kTriangleStrip
    case 0x0C:  // kLineLoop
    case 0x0E:  // kQuadStrip
    case 0x0F:  // kPolygon
    case 0x15:  // k2DLineStrip
    case 0x16:  // k2DTriStrip
      return true;
    default:
      return false;
  }
}

// The guest's endian swap, applied to a value loaded host-native from the
// index buffer. kNone is a real case and must not be treated as "big-endian
// anyway" - that was the bug.
uint32_t GuestSwap32(uint32_t v, uint32_t endian) {
  switch (endian & 3u) {
    case 1:  // k8in16
      return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case 2:  // k8in32
      return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    case 3:  // k16in32
      return (v << 16) | (v >> 16);
    default:
      return v;
  }
}
uint16_t GuestSwap16(uint16_t v, uint32_t endian) {
  return (endian & 3u) == 1u ? uint16_t((v << 8) | (v >> 8)) : v;
}

std::mutex g_maxidx_mutex;
std::map<IndexKey, uint32_t> g_maxidx;
std::atomic<uint64_t> g_maxidx_scans{0};
// kNone / k8in16 / k8in32 / k16in32, counted per SCAN (so per distinct buffer,
// not per draw). Turns "the endian field is probably always 1" into a number.
std::atomic<uint64_t> g_idx_endian[4] = {};
std::atomic<uint64_t> g_maxidx_faults{0};
std::atomic<uint64_t> g_maxidx_capped{0};

// A BOUND ON THE SCAN, so one absurd index_size_words cannot stall the worker
// thread. Counted when it bites: a capped scan yields a LOW max index, which
// would silently look like "count is safe" - the conclusion this census exists
// to test. Capped draws must not be able to vote for that answer.
constexpr uint32_t kMaxIndexScan = 65536;

bool ScanMaxIndex(const GpuDrawRecord* rec, bool fmt32, uint32_t* out) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory || !rec->index_base || !rec->index_size_words) return false;

  // Restart state, from the registers this draw actually carries.
  uint32_t reset_indx = 0;
  bool restart_on = false;
  if (rec->registers && rec->register_count > kRegPaSuScModeCntl) {
    const uint32_t prim_type = rec->vgt_draw_initiator & 0x3Fu;
    restart_on = ((rec->registers[kRegPaSuScModeCntl] >> 21) & 1u) != 0 &&
                 PrimitiveTakesRestart(prim_type);
    if (rec->register_count > kRegVgtMultiPrimIbResetIndx) {
      reset_indx = rec->registers[kRegVgtMultiPrimIbResetIndx] & 0xFFFFFFu;
    }
    // A 16-bit index buffer cannot hold a reset index above 0xFFFF, so the
    // engine treats restart as OFF in that case rather than never matching.
    if (restart_on && !fmt32 && reset_indx > 0xFFFFu) restart_on = false;
  }

  // KEYED ON THE ISSUED COUNT, not the buffer size: draws sharing one index
  // buffer differ precisely in how much of it they read, and keying on the
  // buffer would serve the longest draw's maximum to the shortest.
  const uint32_t issued_key = rec->vgt_draw_initiator >> 16;
  const IndexKey key{rec->index_base,
                     issued_key && issued_key < rec->index_size_words
                         ? issued_key
                         : rec->index_size_words,
                     uint8_t(fmt32 ? 1 : 0),
                     uint8_t(rec->index_endian & 0xFF), reset_indx,
                     uint8_t(restart_on ? 1 : 0)};
  {
    std::lock_guard<std::mutex> lock(g_maxidx_mutex);
    auto it = g_maxidx.find(key);
    if (it != g_maxidx.end()) { *out = it->second; return true; }
  }

  // THE DRAW'S OWN INDEX COUNT, not the bound buffer's size. VGT_DMA_SIZE
  // describes the buffer; VGT_DRAW_INITIATOR.num_indices describes this draw.
  // Several draws share one index buffer, so scanning it whole returns a
  // maximum belonging to some other draw.
  const uint32_t issued = rec->vgt_draw_initiator >> 16;
  uint32_t n = rec->index_size_words;
  if (issued && issued < n) n = issued;
  bool capped = false;
  if (n > kMaxIndexScan) { n = kMaxIndexScan; capped = true; }
  if (!n) return false;
  const uint32_t stride = fmt32 ? 4u : 2u;
  std::vector<uint8_t> buf(size_t(n) * stride);
  bool ok = false;
#if defined(_WIN32)
  __try {
    ok = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(rec->index_base),
                        uint32_t(buf.size()), buf.data());
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
#else
  ok = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(rec->index_base),
                      uint32_t(buf.size()), buf.data());
#endif
  if (!ok) {
    g_maxidx_faults.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  uint32_t hi = 0;
  const uint32_t endian = rec->index_endian;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    if (fmt32) {
      uint32_t raw;
      std::memcpy(&raw, buf.data() + size_t(i) * 4, 4);
      v = GuestSwap32(raw, endian);
    } else {
      uint16_t raw;
      std::memcpy(&raw, buf.data() + size_t(i) * 2, 2);
      v = GuestSwap16(raw, endian);
    }
    // ONLY THE LOW 24 BITS ARE AN INDEX. The hardware ignores the top byte of a
    // 32-bit index - verified on real silicon per the engine's register notes -
    // so junk up there would otherwise yield maxima in the millions and a
    // `safe` extent larger than any buffer the game owns.
    v &= 0xFFFFFFu;
    // The restart sentinel is a REGISTER value under a REGISTER enable, not a
    // constant. Counted as a vertex it inflates the maximum; skipped when
    // restart is off it deflates it. Both directions are wrong and both are
    // silent, so this follows the registers.
    if (restart_on && v == reset_indx) continue;
    if (v > hi) hi = v;
  }
  g_idx_endian[endian & 3u].fetch_add(1, std::memory_order_relaxed);
  if (capped) g_maxidx_capped.fetch_add(1, std::memory_order_relaxed);
  g_maxidx_scans.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_maxidx_mutex);
    g_maxidx[key] = hi;
  }
  *out = hi;
  return true;
}

// THE VERDICT COUNTERS. Each draw votes once.
std::atomic<uint64_t> g_ext_auto{0};        // auto-index: count is exact
std::atomic<uint64_t> g_ext_indexed{0};     // indexed and scanned
std::atomic<uint64_t> g_ext_unscannable{0}; // indexed, index buffer unreadable
std::atomic<uint64_t> g_ext_safe_gt_count{0};   // THE QUESTION: max_index+1 > count
std::atomic<uint64_t> g_ext_count_gt_declared{0};
std::atomic<uint64_t> g_ext_safe_gt_declared{0};

void NoteVertexStreams(const GpuDrawRecord* rec) {
  if (!rec->registers || rec->register_count <= kRegShaderConstantFetch00) return;
  if (!g_cur_slots.count) return;
  const uint32_t* r = rec->registers;

  // The draw's own shape, decoded once for all of this draw's slots.
  //   num_indices   bits 16..31
  //   source_select bits 6..7   (0 = kDMA / indexed, 2 = auto-index)
  //   index format  bit 11      (0 = 16-bit, 1 = 32-bit)
  const uint32_t draw_count = rec->vgt_draw_initiator >> 16;
  const uint32_t source_select = (rec->vgt_draw_initiator >> 6) & 3u;
  const bool fmt32 = ((rec->vgt_draw_initiator >> 11) & 1u) != 0;
  const bool indexed = (source_select == 0) && rec->index_base != 0;
  uint32_t max_index = 0;
  bool have_max = false;
  if (indexed) {
    have_max = ScanMaxIndex(rec, fmt32, &max_index);
    (have_max ? g_ext_indexed : g_ext_unscannable).fetch_add(1, std::memory_order_relaxed);
  } else {
    g_ext_auto.fetch_add(1, std::memory_order_relaxed);
  }
  // vertices touched, under each rule. For an auto-index draw the two agree by
  // construction - which is the control: if they ever disagree there, the
  // decode of the initiator is wrong, not the rule.
  const uint32_t verts_count = draw_count;
  const uint32_t verts_safe = have_max ? (max_index + 1u) : draw_count;
  if (indexed && have_max && verts_safe > verts_count) {
    g_ext_safe_gt_count.fetch_add(1, std::memory_order_relaxed);
  }

  // ONLY THE SLOTS THIS SHADER NAMES. Scanning all 96 cannot work: a group
  // holds either a vertex fetch or part of a texture constant, nothing in the
  // file distinguishes them, and the census said so - 120,530,303 overflowed
  // and 79,822,914 were rejected while "64 distinct" was whichever decodes
  // looked plausible first. The shader's own vfetch instructions are the only
  // thing that knows.
  for (uint8_t si = 0; si < g_cur_slots.count; ++si) {
    const uint32_t slot = g_cur_slots.slot[si];
    // const_index * 6 + select * 2, because the file is 32 groups of 6 dwords
    // holding three 2-dword vertex fetches each.
    const uint32_t base = kRegShaderConstantFetch00 + (slot / 3) * 6 + (slot % 3) * 2;
    if (base + 1 >= rec->register_count) continue;
    const uint32_t d0 = r[base];
    const uint32_t d1 = r[base + 1];
    const uint32_t addr_dwords = d0 >> 2;                 // type:2 then address:30
    // MASK THE SIZE TO ITS 24 BITS. Without the mask the 6 pad bits above it
    // ride along and every stream reads as 268,435,540 bytes - 256 MB apiece,
    // 16 GB of "geometry" in one frame. The absurdity is what exposed it; a
    // plausible wrong number would not have.
    const uint32_t size_dwords = (d1 >> 2) & 0xFFFFFFu;   // endian:2 then size:24
    if (!addr_dwords || !size_dwords) continue;
    StreamKey k{addr_dwords << 2, size_dwords << 2};
    if (k.bytes > kMaxStreamBytes) {
      g_stream_rejected.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    // CLAMP TO WHOLE ELEMENTS. `len = total - total % stride` is the sibling
    // project's rule: a declared size that is not a whole number of elements
    // cannot all be read, and copying it whole is how a stream cache turns into
    // a buffer copier.
    if (g_cur_slots.stride[si] && k.bytes) {
      const uint32_t stride_bytes = uint32_t(g_cur_slots.stride[si]) * 4u;
      const uint32_t rem = k.bytes % stride_bytes;
      if (rem) {
        k.bytes -= rem;
        g_stream_clamped.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // The rival extents for THIS slot, capped by the declared size: a rule may
    // read less than the engine declared, never more. A rule that wants more
    // is a rule that is wrong, and the counters below say how often.
    if (g_cur_slots.stride[si]) {
      const uint64_t sb = uint64_t(g_cur_slots.stride[si]) * 4ull;
      const uint64_t want_count = uint64_t(verts_count) * sb;
      const uint64_t want_safe = uint64_t(verts_safe) * sb;
      if (want_count > k.bytes) g_ext_count_gt_declared.fetch_add(1, std::memory_order_relaxed);
      if (want_safe > k.bytes) g_ext_safe_gt_declared.fetch_add(1, std::memory_order_relaxed);
      k.count_bytes = uint32_t(want_count < k.bytes ? want_count : k.bytes);
      k.safe_bytes = uint32_t(want_safe < k.bytes ? want_safe : k.bytes);
    } else {
      k.count_bytes = k.bytes;
      k.safe_bytes = k.bytes;
    }

    {
      uint32_t b = k.bytes, bucket = 0;
      while (b >= 64 && bucket < 7) { b >>= 2; ++bucket; }
      g_size_bucket[bucket].fetch_add(1, std::memory_order_relaxed);
    }
    {
      // Counted BEFORE the cumulative table, so the per-frame figure is not
      // limited by which streams happened to arrive first in the whole run.
      std::lock_guard<std::mutex> lock(g_stream_mutex);
      if (FrameSetInsert(k.address, 0)) {
        ++g_stream_frame_n;
        g_stream_frame_bytes += k.bytes;
      }
    }

    bool is_new = false;
    {
      std::lock_guard<std::mutex> lock(g_stream_mutex);
      int found = -1;
      for (int j = 0; j < g_stream_n; ++j) {
        if (g_stream_keys[j] == k) { found = j; break; }
      }
      if (found >= 0) {
        ++g_stream_uses[found];
        if (k.bytes > g_stream_keys[found].bytes) g_stream_keys[found].bytes = k.bytes;
        if (k.count_bytes > g_stream_keys[found].count_bytes)
          g_stream_keys[found].count_bytes = k.count_bytes;
        if (k.safe_bytes > g_stream_keys[found].safe_bytes)
          g_stream_keys[found].safe_bytes = k.safe_bytes;
        continue;
      }
      if (g_stream_n >= kMaxStreams) {
        g_stream_overflow.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      g_stream_keys[g_stream_n] = k;
      g_stream_uses[g_stream_n] = 1;
      ++g_stream_n;
      is_new = true;
    }

    // ONCE PER DISTINCT STREAM, outside the lock: is the geometry actually
    // reachable? Constraint 4 was wrong about this for index data and the
    // correction was only found by testing each interpretation separately, so
    // this reads the first bytes rather than assuming they are there. SEH
    // because an address this code cannot vouch for is exactly what it is.
    if (is_new) {
      g_stream_bytes.fetch_add(k.bytes, std::memory_order_relaxed);
      auto* memory = REX_KERNEL_MEMORY();
      if (memory) {
        uint8_t probe[16];
        bool ok = false;
#if defined(_WIN32)
        __try {
          ok = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(k.address),
                              sizeof(probe), probe);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          ok = false;
        }
#endif
        (ok ? g_stream_readable : g_stream_unreadable)
            .fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

std::string StreamReport() {
  std::string out;
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  if (!g_stream_n) return out;
  out = fmt::format(" | VERTEX STREAMS {} distinct, {} KB, readable {} unreadable {},"
                    " rejected {} | {} programs decoded, {} fetch slots"
                    " | PER FRAME peak {} streams, {} KB; MEAN {} KB over {} frames"
                    " | clamped {} | sizes"
                    " <64:{} <256:{} <1K:{} <4K:{} <16K:{} <64K:{} <1M:{} >=1M:{}",
                    g_stream_n, g_stream_bytes.load() / 1024, g_stream_readable.load(),
                    g_stream_unreadable.load(), g_stream_rejected.load(),
                    g_fetch_programs.load(), g_fetch_slots_total.load(),
                    g_stream_frame_max.load(), g_stream_frame_bytes_max.load() / 1024,
                    g_stream_frames_counted.load()
                        ? g_stream_frame_bytes_sum.load() / g_stream_frames_counted.load() / 1024
                        : 0,
                    g_stream_frames_counted.load(),
                    g_stream_clamped.load(),
                    g_size_bucket[0].load(), g_size_bucket[1].load(),
                    g_size_bucket[2].load(), g_size_bucket[3].load(),
                    g_size_bucket[4].load(), g_size_bucket[5].load(),
                    g_size_bucket[6].load(), g_size_bucket[7].load());
  if (const uint64_t o = g_stream_overflow.load()) out += fmt::format(" (+{} OVERFLOW)", o);

  // THE EXTENT COMPARISON, which is what decides the cache's upload rule.
  uint64_t tot_declared = 0, tot_count = 0, tot_safe = 0;
  for (int i = 0; i < g_stream_n; ++i) {
    tot_declared += g_stream_keys[i].bytes;
    tot_count += g_stream_keys[i].count_bytes;
    tot_safe += g_stream_keys[i].safe_bytes;
  }
  out += fmt::format(
      "\n    READ EXTENT over {} streams: declared {} KB | count*stride {} KB"
      " | (maxidx+1)*stride {} KB"
      "\n    draws: auto {} indexed {} unscannable {} | maxidx+1 > count: {}"
      " | count > declared: {} | safe > declared: {}"
      "\n    index scans {} (cached), faults {}, CAPPED {}"
      " | endian none:{} 8in16:{} 8in32:{} 16in32:{}",
      g_stream_n, tot_declared / 1024, tot_count / 1024, tot_safe / 1024,
      g_ext_auto.load(), g_ext_indexed.load(), g_ext_unscannable.load(),
      g_ext_safe_gt_count.load(), g_ext_count_gt_declared.load(),
      g_ext_safe_gt_declared.load(), g_maxidx_scans.load(),
      g_maxidx_faults.load(), g_maxidx_capped.load(),
      g_idx_endian[0].load(), g_idx_endian[1].load(),
      g_idx_endian[2].load(), g_idx_endian[3].load());
  for (int i = 0; i < g_stream_n && i < 6; ++i) {
    out += fmt::format("\n    [{}] {:08X} {} bytes, used {}",
                       i, g_stream_keys[i].address, g_stream_keys[i].bytes, g_stream_uses[i]);
  }
  return out;
}

void NoteSurface(const GpuDrawRecord* rec) {
  // The register file must actually reach the index being read. A record whose
  // register_count is short is a DIFFERENT failure from a surface that varies,
  // and reading past the end would be indistinguishable from either.
  if (!rec->registers || rec->register_count <= kRegRbModecontrol) {
    g_surf_no_regs.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t* r = rec->registers;
  const uint32_t si = r[kRegRbSurfaceInfo];
  const uint32_t ci = r[kRegRbColorInfo];
  const uint32_t di = r[kRegRbDepthInfo];
  const uint32_t tl = r[kRegPaScWindowScissorTL];
  const uint32_t br = r[kRegPaScWindowScissorBR];
  SurfaceKey k{};
  k.pitch = si & 0x3FFF;               // surface_pitch : 14, in pixels
  k.msaa = (si >> 16) & 0x3;           // msaa_samples : 2
  k.color_base = ci & 0xFFF;           // color_base : 11 + bit_11, in tiles
  k.color_format = (ci >> 16) & 0xF;   // ColorRenderTargetFormat : 4
  k.depth_base = di & 0xFFF;
  k.edram_mode = r[kRegRbModecontrol] & 0x7;
  const uint32_t tlx = tl & 0x3FFF, tly = (tl >> 16) & 0x3FFF;
  const uint32_t brx = br & 0x3FFF, bry = (br >> 16) & 0x3FFF;
  const uint32_t w = brx > tlx ? brx - tlx : 0;
  const uint32_t h = bry > tly ? bry - tly : 0;

  // THE CENSUS FIRST, AND ONLY THEN THE RENDERER - and only when something
  // actually changed. Asking the renderer on EVERY draw would take a mutex
  // millions of times per run on the GPU worker thread, contending with the
  // render thread's own creation work, and the premise of this whole file is
  // that the game runs identically whether the native path is on or off. A
  // surface is new, or its extent grows, a few dozen times in a run.
  bool tell_renderer = false;
  {
    std::lock_guard<std::mutex> lock(g_surf_mutex);
    int found = -1;
    for (int i = 0; i < g_surf_n; ++i) {
      if (g_surf_keys[i] == k) {
        found = i;
        break;
      }
    }
    if (found >= 0) {
      ++g_surf_draws[found];
      if (w < g_surf_w_min[found]) g_surf_w_min[found] = w;
      if (h < g_surf_h_min[found]) g_surf_h_min[found] = h;
      // Only a GROWN extent is news for the renderer: a target must be at
      // least as large as anything drawn into it, and shrinking tells it
      // nothing it can act on.
      if (w > g_surf_w_max[found]) {
        g_surf_w_max[found] = w;
        tell_renderer = true;
      }
      if (h > g_surf_h_max[found]) {
        g_surf_h_max[found] = h;
        tell_renderer = true;
      }
    } else if (g_surf_n >= kMaxSurfaces) {
      g_surf_overflow.fetch_add(1, std::memory_order_relaxed);
      return;
    } else {
      g_surf_keys[g_surf_n] = k;
      g_surf_draws[g_surf_n] = 1;
      g_surf_w_min[g_surf_n] = g_surf_w_max[g_surf_n] = w;
      g_surf_h_min[g_surf_n] = g_surf_h_max[g_surf_n] = h;
      ++g_surf_n;
      tell_renderer = true;
    }
  }
  if (!tell_renderer) return;

  // OUTSIDE the census lock, so the two are never held at once and no lock
  // order exists to get wrong later.
  render::SurfaceDesc sd;
  sd.pitch = k.pitch;
  sd.msaa = k.msaa;
  sd.color_base = k.color_base;
  sd.color_format = k.color_format;
  sd.depth_base = k.depth_base;
  sd.edram_mode = k.edram_mode;
  // A scissor of 8192 is the "no scissor" SENTINEL, not an extent. Making an
  // 8192x8192 target out of it would allocate 256 MB for a pass that draws into
  // 1280x720, sixty-one times over. The surface pitch IS the target's width in
  // pixels, so that is the fallback; a height that is only the sentinel leaves
  // the request incomplete and it is not made at all, rather than guessed.
  sd.width = (w && w < 8192) ? w : k.pitch;
  sd.height = (h && h < 8192) ? h : 0;
  if (sd.width && sd.height) render::WantRenderTarget(sd);
}

// The pipeline this draw would need. Separate from NoteSurface because it is
// wanted on EVERY draw, not only when a surface is new: the shader pair changes
// far more often than the target does, and WantPipeline is cheap when the pair
// and format are unchanged.
void NotePipeline(const GpuDrawRecord* rec) {
  if (!rec->registers || rec->register_count <= kRegRbColorInfo) return;
  render::WantPipeline((rec->registers[kRegRbColorInfo] >> 16) & 0xF);
}

std::string SurfaceReport() {
  std::string out;
  std::lock_guard<std::mutex> lock(g_surf_mutex);
  if (!g_surf_n) return out;
  // SCENE-ARRIVAL GUARD. A run that never left the menus has zero indexed
  // draws, and every number taken from it describes a title screen. The
  // sibling session lost a whole A/B to this: their pad script is time-based,
  // so making one leg CHEAPER let the guest run faster, the timed presses
  // missed, and the world never loaded - the harness silently turned "this
  // configuration is faster" into "this configuration measured a different
  // scene". My pad scripts are time-based too, so the guard belongs where the
  // numbers are read, not in the launcher.
  if (!g_indexed.load()) {
    return " | SURFACES: MENU ONLY - 0 indexed draws, this run reached no scene, DO NOT SCORE";
  }
  out = fmt::format(" | SURFACES {} distinct", g_surf_n);
  if (const uint64_t o = g_surf_overflow.load()) out += fmt::format(" (+{} OVERFLOW)", o);
  if (const uint64_t n = g_surf_no_regs.load()) out += fmt::format(" ({} no-regs)", n);
  for (int i = 0; i < g_surf_n; ++i) {
    const SurfaceKey& k = g_surf_keys[i];
    out += fmt::format("\n    [{}] pitch {} msaa {} colour base {} fmt {} depth base {} edram {}"
                       " | scissor {}x{}..{}x{} | draws {}",
                       i, k.pitch, k.msaa, k.color_base, k.color_format, k.depth_base,
                       k.edram_mode, g_surf_w_min[i], g_surf_h_min[i], g_surf_w_max[i],
                       g_surf_h_max[i], g_surf_draws[i]);
  }
  return out;
}

GpuSetDrawCallbackFn g_setter = nullptr;
GpuSetSwapCallbackFn g_swap_setter = nullptr;
std::atomic<bool> g_reporting{false};

// Totals as one line. Shared by the periodic reporter and by Stop().
std::string Totals() {
  // The serial verdict is stated in full either way. "0 gaps" out of a named
  // number of draws is the evidence; a bare count of draws would not be.
  const uint64_t gaps = g_gaps.load(), missing = g_missing.load(), repeats = g_repeats.load();
  std::string serial;
  if (!gaps && !repeats) {
    serial = fmt::format(" | SERIAL UNBROKEN 1..{}", g_last_serial);
  } else {
    serial = fmt::format(" | SERIAL BROKEN: {} gap(s) losing {} draw(s){}{}", gaps, missing,
                         repeats ? fmt::format(", {} repeat(s)", repeats) : "",
                         g_first_gap.load() ? fmt::format(", first at {}", g_first_gap.load()) : "");
  }
  std::string idx;
  if (const uint64_t n = g_idx_checked.load()) {
    // Named as a verdict, not as four counters: which address space the index
    // base lives in is the question, so the line answers it.
    idx = fmt::format(
        " | INDEX BASE of {} draws: {} virtual-only, {} physical-only, {} both, {} neither,"
        " {} FAULTED (virt-fault {}, phys-fault {}) | first sample: base {:08X} words {}"
        " endian {}",
        n, g_idx_virtual_ok.load(), g_idx_physical_ok.load(), g_idx_both.load(),
        g_idx_neither.load(), g_idx_faulted.load(), g_idx_virt_faulted.load(),
        g_idx_phys_faulted.load(), g_idx_sample_base.load(),
        g_idx_sample_words.load(), g_idx_sample_endian.load());
  }
  if (const uint64_t u = g_ucode_read_ok.load() + g_ucode_read_faulted.load()) {
    idx += fmt::format(" | UCODE readable {} of {} (first {:08X} word0 {:08X})",
                       g_ucode_read_ok.load(), u, g_ucode_first_addr.load(),
                       g_ucode_first_word.load());
  }
  const std::string skip = g_would_skip.load()
      ? fmt::format(" | WantShader would REFUSE {} draws that have a pipeline", g_would_skip.load())
      : std::string();
  return fmt::format("{} draws ({} indexed, {} auto), {} indices{}{}{}{}{}{}{}",
                     g_draws.load(), g_indexed.load(), g_auto.load(), g_indices.load(), serial, idx,
                     SurfaceReport() + BinReport() + StreamReport(), skip,
                     g_bad_size.load() ? fmt::format(" | {} ABI MISMATCH", g_bad_size.load()) : "",
                     g_no_regs.load() ? fmt::format(" | {} without registers", g_no_regs.load()) : "",
                     g_no_shader.load() ? fmt::format(" | {} without a shader", g_no_shader.load()) : "");
}

// Report on a timer, not only at shutdown.
//
// Stop() runs on a clean exit and nothing else, and on this branch a run's
// usual ending is a hang in the Chapter 1 intro followed by a kill - so the one
// measurement this stage exists to produce was, in practice, never written. A
// counter you can only read by finishing is not an instrument.
//
// Its own thread rather than a hook in OnDraw: OnDraw is on the GPU worker
// inside packet execution, thousands of times a frame, and this project has
// already spent two builds on the theory that logging from that thread was
// hanging the game. It was not - but the way to keep that answer clean is to
// not log from there at all.
void Reporter(unsigned every_s) {
  while (g_reporting.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    static unsigned t = 0;
    if (++t < every_s) continue;
    t = 0;
    if (g_draws.load()) REXLOG_INFO("[ng2-ngpu] running totals: {}", Totals());
  }
}

// Runs on the GPU worker thread, once per draw, thousands of times a frame.
// Everything here is an atomic increment or a field read; anything heavier
// belongs behind a queue or on another thread.
// STAGE 2b, step 1: can a draw's index buffer be RESOLVED from guest memory?
//
// Before anything is rendered, the inputs have to be reachable. The record
// carries index_base as the packet named it, and which address space that is
// cannot be assumed: object-held addresses on the Fable side are CPU virtual,
// while fetch constants are GPU physical, and index_base arrives from neither -
// it comes straight off the PM4 stream. So both interpretations are tried and
// the data decides, rather than a rule borrowed from the other title.
//
// Plausibility, deliberately weak: NG2's indexed draws are all 16-bit TRI_STRIP
// (from the census), so a real index buffer is a run of 16-bit values that are
// not all zero, not all 0xFFFF, and whose maximum is within a sane vertex count
// for one draw. A wrong translation lands in unrelated memory and fails at
// least one of those. This cannot prove a translation right - only a rendered
// frame does that - but it can prove one WRONG, cheaply, before any renderer
// depends on it.

// Read guest bytes WITHOUT trusting the address.
//
// TranslateVirtual and TranslatePhysical are arithmetic - base + offset - with
// no bounds check and no mapping check, so a bogus index_base yields a wild
// pointer that faults on read. This probe exists precisely BECAUSE the address
// space of index_base is unknown, so by construction it will be handed
// addresses that are wrong in one interpretation or the other. Dereferencing
// them unguarded killed the game: 227,800 draws in, no shutdown, no stall, no
// error - the process simply stopped, and the instrument took the subject with
// it.
//
// Structured exception handling rather than a range check alone, because a
// range check can only reject what it knows about: guest memory is sparsely
// mapped, so an address inside the arena can still be unmapped. This is the one
// place in this file where a fault is an EXPECTED OUTCOME and must be a datum,
// not a crash.
bool ReadGuestBytes(const uint8_t* p, uint32_t bytes, uint8_t* out) {
  if (!p || !bytes) return false;
#if defined(_WIN32)
  __try {
    for (uint32_t i = 0; i < bytes; ++i) out[i] = p[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;  // unmapped or wrong interpretation - a result, not a failure
  }
#else
  std::memcpy(out, p, bytes);
  return true;
#endif
}

bool PlausibleIndexRun(const uint8_t* p, uint32_t count_words, bool big_endian) {
  if (!p) return false;
  const uint32_t n = count_words < 32u ? count_words : 32u;
  if (n < 3) return false;
  uint8_t buf[64];
  if (!ReadGuestBytes(p, n * 2, buf)) return false;
  uint32_t max_index = 0;
  bool all_zero = true, all_ff = true;
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t v = big_endian ? uint16_t((buf[i * 2] << 8) | buf[i * 2 + 1])
                                  : uint16_t((buf[i * 2 + 1] << 8) | buf[i * 2]);
    if (v != 0) all_zero = false;
    if (v != 0xFFFF) all_ff = false;
    if (v > max_index) max_index = v;
  }
  // 65535 is the 16-bit reset index; a buffer that is entirely resets is not
  // geometry. An upper bound of 32k vertices in one draw is generous for NG2,
  // whose largest observed draw is well under that.
  return !all_zero && !all_ff && max_index < 32768u;
}

// One interpretation, one guard, so neither can mask the other. Kept as its
// own function because __try/__except cannot coexist with C++ objects needing
// unwinding in the same frame, and because a fault here must be attributable
// to THIS interpretation rather than to whichever ran first.
// The memory type is taken from the accessor rather than named: rex::Memory is
// an incomplete type in this translation unit, and only the pointer it hands
// back is ever used.
using GuestMemory = decltype(REX_KERNEL_MEMORY());

bool TryIndexInterpretation(GuestMemory memory, bool physical, const GpuDrawRecord* rec,
                            bool big_endian, bool* faulted) {
#if defined(_WIN32)
  __try {
    const uint8_t* p = physical ? memory->TranslatePhysical<const uint8_t*>(rec->index_base)
                                : memory->TranslateVirtual<const uint8_t*>(rec->index_base);
    return PlausibleIndexRun(p, rec->index_size_words, big_endian);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *faulted = true;
    return false;
  }
#else
  const uint8_t* p = physical ? memory->TranslatePhysical<const uint8_t*>(rec->index_base)
                              : memory->TranslateVirtual<const uint8_t*>(rec->index_base);
  return PlausibleIndexRun(p, rec->index_size_words, big_endian);
#endif
}

// IMMEDIATE MICROCODE, REGISTERED ONCE PER LOAD RATHER THAN ONCE PER DRAW.
//
// The bytes only change when IM_LOAD_IMMEDIATE runs, and the record carries a
// per-stage serial that counts those loads. Hashing per draw would run 166,118
// times for vertex and 159,445 for pixel in a single measured frame; keyed on
// the serial it runs once per load.
//
// The serials are touched only from the GPU worker thread - the callback's own
// thread - so they need no atomic, the same reasoning as g_last_serial.
uint64_t g_seen_vs_ucode_serial = 0;
uint64_t g_seen_ps_ucode_serial = 0;
std::atomic<uint64_t> g_immediate_registered{0};
std::atomic<uint64_t> g_immediate_absent{0};

void RegisterImmediate(render::ShaderStage stage, const uint8_t* ucode, uint32_t dwords,
                       uint64_t serial) {
  // A stage flagged immediate with NO bytes is a real outcome, not a no-op: the
  // plugin refuses a program larger than it will carry rather than truncating
  // it, because half a shader hashes to one that does not exist and would read
  // as "unmatched" - sending the reader into the translation pipeline after a
  // shader that was never whole.
  if (!ucode || !dwords) {
    g_immediate_absent.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint64_t& seen = (stage == render::ShaderStage::kVertex) ? g_seen_vs_ucode_serial
                                                           : g_seen_ps_ucode_serial;
  if (serial == seen) return;  // same program as the last draw
  seen = serial;
  // Guest address 0: an immediate program HAS no address, and passing the
  // record's stale one would file it under another shader's identity.
  render::RegisterShaderMicrocode(stage, 0, ucode, dwords * 4, nullptr);
  g_immediate_registered.fetch_add(1, std::memory_order_relaxed);
}

void CheckIndexBuffer(const GpuDrawRecord* rec) {
  if (!rec->index_base || !rec->index_size_words) return;

  // SAY WHAT WE ARE ABOUT TO TOUCH, BEFORE TOUCHING IT.
  //
  // The first version of this probe killed the game on the very first indexed
  // draw - 229,994 draws in, no shutdown, no stall, no error line - and left
  // nothing to diagnose with: the counters showed 1 checked and 0 classified,
  // which says only "it died between those two points". A probe into memory of
  // unknown validity has to log its inputs BEFORE dereferencing them, or its
  // own crash is the one event it cannot report.
  // Recorded, not logged. A REXLOG_INFO placed here produced NOTHING in the log
  // while g_idx_checked proved the function ran 4,679,029 times - logging from
  // inside the plugin's draw callback does not reach the file, for reasons not
  // worth chasing. The reporter thread demonstrably logs, so the sample is
  // stashed and printed from there.
  if (!g_idx_sample_base.load(std::memory_order_relaxed)) {
    g_idx_sample_base.store(rec->index_base, std::memory_order_relaxed);
    g_idx_sample_words.store(rec->index_size_words, std::memory_order_relaxed);
    g_idx_sample_endian.store(rec->index_endian, std::memory_order_relaxed);
  }

  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  g_idx_checked.fetch_add(1, std::memory_order_relaxed);

  // index_endian 2 is the Xenos 8-in-16 swap, i.e. big-endian halfwords.
  const bool big_endian = rec->index_endian != 0;
  // ONE GUARD PER INTERPRETATION. They used to share a single __try with the
  // VIRTUAL translation evaluated first, so a fault there aborted the block
  // and the PHYSICAL interpretation was never evaluated at all - and physical
  // is the one the plugin itself uses successfully (primitive_processor.cpp
  // reads memory_.TranslatePhysical(guest_index_base) with no guard whatever).
  // That structure made one of the two answers unobservable, and it reported
  // 1,889,699 faults of 1,889,700 which was read as "index data is not
  // CPU-readable". An instrument whose own shape can only produce one outcome
  // has not tested the other.
  bool vf = false, pf = false;
  const bool v = TryIndexInterpretation(memory, false, rec, big_endian, &vf);
  const bool p = TryIndexInterpretation(memory, true, rec, big_endian, &pf);
  if (vf) g_idx_virt_faulted.fetch_add(1, std::memory_order_relaxed);
  if (pf) g_idx_phys_faulted.fetch_add(1, std::memory_order_relaxed);
  if (vf && pf) {
    g_idx_faulted.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (v && p) g_idx_both.fetch_add(1, std::memory_order_relaxed);
  else if (v) g_idx_virtual_ok.fetch_add(1, std::memory_order_relaxed);
  else if (p) g_idx_physical_ok.fetch_add(1, std::memory_order_relaxed);
  else g_idx_neither.fetch_add(1, std::memory_order_relaxed);
}

// Fired once per guest frame at the swap packet. Closes the coverage oracle's
// frame: draws handed over against draws the renderer actually issued.
void OnSwap(uint32_t, uint32_t, uint32_t) {
  {
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    if (uint64_t(g_stream_frame_n) > g_stream_frame_max.load(std::memory_order_relaxed)) {
      g_stream_frame_max.store(uint64_t(g_stream_frame_n), std::memory_order_relaxed);
    }
    if (g_stream_frame_bytes > g_stream_frame_bytes_max.load(std::memory_order_relaxed)) {
      g_stream_frame_bytes_max.store(g_stream_frame_bytes, std::memory_order_relaxed);
    }
    g_stream_frame_bytes_sum.fetch_add(g_stream_frame_bytes, std::memory_order_relaxed);
    g_stream_frames_counted.fetch_add(1, std::memory_order_relaxed);
    g_stream_frame_n = 0;
    g_stream_frame_bytes = 0;
    std::memset(g_frame_set_addr, 0, sizeof(g_frame_set_addr));
  }
  render::EndFrame();
}

// STAGE 2b STEP 2: is shader microcode READABLE at its IM_LOAD address?
//
// The gating question, asked before building a translation path on top of the
// answer. Index data turned out not to be CPU-readable at all - those pages
// reach the GPU through shared-memory residency - and 1,889,699 faults were
// spent discovering that. If shader microcode is the same, a registry keyed on
// runtime-read microcode is impossible and the mapping has to come from
// somewhere else entirely.
//
// Reuses the same guarded read, so a fault is a datum rather than a crash.

// Reads the WHOLE program and offers it to the registry. Measured readable:
// 11,095 of 11,095 with zero faults, which is what makes this path possible at
// all - index data, by contrast, is not CPU-readable and faulted 1,889,699
// times out of 1,889,700.
//
// Only the first sighting of an address does the work. A program is thousands
// of draws' worth of the same bytes, and hashing it every draw would put a
// kilobyte-scale read on the per-draw path for no information.
constexpr uint32_t kMaxUcodeBytes = 64u * 1024u;

void ProbeShaderMicrocode(render::ShaderStage stage, uint32_t addr, uint32_t dwords) {
  if (!addr || !dwords) return;
  const uint32_t bytes = dwords * 4u;
  if (bytes > kMaxUcodeBytes) return;

  {
    // Seen-set, so each address is read once rather than once per draw.
    static std::mutex m;
    static std::set<uint64_t> seen;
    const uint64_t key = (uint64_t(stage == render::ShaderStage::kPixel) << 32) | addr;
    std::lock_guard<std::mutex> lock(m);
    if (!seen.insert(key).second) return;
  }

  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return;
  std::vector<uint8_t> buf(bytes);
#if defined(_WIN32)
  bool ok = false;
  __try {
    ok = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(addr), bytes, buf.data());
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
#else
  const bool ok = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(addr), bytes, buf.data());
#endif
  if (!ok) {
    g_ucode_read_faulted.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_ucode_read_ok.fetch_add(1, std::memory_order_relaxed);
  // DECODE THIS PROGRAM'S FETCH SLOTS while its microcode is in hand. Once per
  // address, never per draw - ProbeShaderMicrocode already runs once per
  // address by its own seen-set, so this inherits that and costs nothing on the
  // draw path.
  if (stage == render::ShaderStage::kVertex) {
    RememberFetchSlots(addr, buf.data(), bytes);
  }
  uint32_t expected = 0;
  if (g_ucode_first_addr.compare_exchange_strong(expected, addr, std::memory_order_relaxed)) {
    g_ucode_first_word.store((uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                                 (uint32_t(buf[2]) << 8) | uint32_t(buf[3]),
                             std::memory_order_relaxed);
  }
  // THE LITERAL PREAMBLE TOO, when the registry wants to dump this program.
  //
  // A synthetic container built from code alone loses the shader's literal
  // constants - c252..c255 for a vertex shader - and a program that needs 1.0
  // then computes with 0.0. That RENDERS rather than fails, which is the
  // hardest class to notice, and this project has already been caught by it.
  //
  // 128 bytes is the largest preamble observed across 625 containers
  // (physicalOffset 0, 64 or 128); the offline tool decides how many are real
  // from the microcode's own lowest high-range constant register. Reading
  // BEFORE a valid address can land on an unmapped page, so it goes through
  // the same guard - a fault here costs the preamble, not the program.
  std::vector<uint8_t> pre(128);
  bool have_pre = false;
  if (addr > 128) {
#if defined(_WIN32)
    __try {
      have_pre = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(addr - 128), 128,
                                pre.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      have_pre = false;
    }
#else
    have_pre = ReadGuestBytes(memory->TranslatePhysical<const uint8_t*>(addr - 128), 128,
                              pre.data());
#endif
  }
  render::RegisterShaderMicrocode(stage, addr, buf.data(), bytes,
                                  have_pre ? pre.data() : nullptr);
}

void OnDraw(const GpuDrawRecord* rec) {
  if (!rec || rec->struct_size != sizeof(GpuDrawRecord)) {
    g_bad_size.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!rec->registers || !rec->register_count) g_no_regs.fetch_add(1, std::memory_order_relaxed);
  if (!rec->vs_address && !rec->vs_immediate) g_no_shader.fetch_add(1, std::memory_order_relaxed);

  // THE MEASUREMENT THIS STAGE EXISTS FOR.
  //
  // Every record should arrive exactly one greater than the last. If it does,
  // no draw was lost between the plugin and here and none was delivered twice -
  // which is the DLL-boundary question, answered continuously and from the
  // first few records, rather than by comparing two cumulative totals that two
  // threads sample at different instants and that only agree at a standstill.
  // (Nor could those totals be had anyway: there is one callback slot, so the
  // plugin's own self-test cannot run while a real consumer is installed.)
  if (rec->draw_serial != g_last_serial + 1) {
    if (rec->draw_serial > g_last_serial) {
      g_gaps.fetch_add(1, std::memory_order_relaxed);
      g_missing.fetch_add(rec->draw_serial - g_last_serial - 1, std::memory_order_relaxed);
      uint64_t expected = 0;
      g_first_gap.compare_exchange_strong(expected, g_last_serial + 1, std::memory_order_relaxed);
    } else {
      g_repeats.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_last_serial = rec->draw_serial;

  // The coverage oracle's denominator. Counted HERE, where a draw actually
  // arrives, rather than from the renderer's own view - a renderer cannot
  // report what it never saw, which is the whole point of the comparison.
  render::NoteDrawHandedOff();

  // BOTH STAGES, EVERY DRAW, THROUGH THE SAME CALL. Not "vertex now, pixel
  // when the pixel path exists" - that asymmetry is exactly what let the
  // sibling renderer run a whole frame on one stale pixel shader while its
  // geometry came out right. Two calls to one function, so neither stage can
  // quietly acquire a capability the other lacks.
  const bool have_vs = render::WantShader(render::ShaderStage::kVertex, rec->vs_address,
                                          rec->vs_dwords, rec->vs_immediate != 0);
  const bool have_ps = render::WantShader(render::ShaderStage::kPixel, rec->ps_address,
                                          rec->ps_dwords, rec->ps_immediate != 0);
  // THE SIZE OF A TRAP, MEASURED RATHER THAN DESCRIBED.
  //
  // WantShader answers "is there a translated program filed under this
  // ADDRESS", and `available` is set at exactly one site - the by-pointer
  // branch of RegisterShaderMicrocode. An IMMEDIATE shader never reaches it:
  // it is matched by content, its DXIL is loaded, a pipeline is built for it,
  // and WantShader still returns false, because there is no address to file it
  // under.
  //
  // So a draw path gated on WantShader would skip every immediate draw while
  // reporting them as "shader not available" - and immediate is 91.8% of draws
  // in light frames. This counts exactly that population: draws where the
  // availability probe says no and a usable pipeline exists anyway. If the
  // number is large, the trap is real and the gate must be WantPipeline.
  if ((!have_vs || !have_ps) && render::WantPipeline(
          (rec->registers && rec->register_count > kRegRbColorInfo)
              ? ((rec->registers[kRegRbColorInfo] >> 16) & 0xF) : 0u)) {
    g_would_skip.fetch_add(1, std::memory_order_relaxed);
  }

  // Can the microcode behind those addresses actually be read?
  // Both stages, symmetrically - the registration path must not favour one
  // either, or it reintroduces the asymmetry the lookup was built to prevent.
  if (!rec->vs_immediate)
    ProbeShaderMicrocode(render::ShaderStage::kVertex, rec->vs_address, rec->vs_dwords);
  else
    RegisterImmediate(render::ShaderStage::kVertex, rec->vs_ucode, rec->vs_ucode_dwords,
                      rec->vs_ucode_serial);
  if (!rec->ps_immediate)
    ProbeShaderMicrocode(render::ShaderStage::kPixel, rec->ps_address, rec->ps_dwords);
  else
    RegisterImmediate(render::ShaderStage::kPixel, rec->ps_ucode, rec->ps_ucode_dwords,
                      rec->ps_ucode_serial);

  const uint32_t di = rec->vgt_draw_initiator;
  g_draws.fetch_add(1, std::memory_order_relaxed);
  g_indices.fetch_add(di >> 16, std::memory_order_relaxed);
  // EVERY DRAW, not just the indexed ones. A render target is state that
  // applies to the whole stream, and two thirds of NG2's draws are auto-index -
  // censusing only the indexed third would have described a third of the
  // picture while looking complete, and reported NOTHING at all in menus, where
  // the indexed count is zero.
  if (g_check_surface) NoteSurface(rec);
  if (g_check_surface) NoteBin(rec);
  if (g_check_surface) NotePipeline(rec);
  if (g_check_surface) { SelectFetchSlots(rec); NoteVertexStreams(rec); }
  if (((di >> 6) & 0x3) == kSourceDMA) {
    g_indexed.fetch_add(1, std::memory_order_relaxed);
    // Only indexed draws have an index buffer to resolve; the auto-index ones
    // carry no address at all.
    if (g_check_inputs) CheckIndexBuffer(rec);
  } else {
    g_auto.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

void Start() {
#if defined(_WIN32)
  if (!std::getenv("NG2_NATIVE_GPU")) return;

  // The runtime has already loaded the plugin, so this is a handle lookup, not
  // a load.
  HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
  if (!m) {
    REXLOG_INFO("[ng2-ngpu] rexgpu-xenos.dll is not loaded - native path stays off");
    return;
  }
  g_setter = reinterpret_cast<GpuSetDrawCallbackFn>(GetProcAddress(m, kSetDrawCallbackSymbol));
  if (!g_setter) {
    // An older plugin without the export. Not an error - the game runs exactly
    // as it always has - but said out loud, because a silent no-op here later
    // reads as "the callback fired zero times", which is a different problem.
    REXLOG_INFO("[ng2-ngpu] this plugin has no {} - native path stays off", kSetDrawCallbackSymbol);
    return;
  }
  g_setter(&OnDraw);

  // The frame boundary. Optional: an older plugin without it leaves the oracle
  // reporting per-run totals instead of per-frame, which is a worse instrument
  // but not a broken game.
  g_swap_setter = reinterpret_cast<GpuSetSwapCallbackFn>(GetProcAddress(m, kSetSwapCallbackSymbol));
  if (g_swap_setter) {
    g_swap_setter(&OnSwap);
  } else {
    REXLOG_INFO("[ng2-ngpu] this plugin has no {} - no frame boundary, per-run totals only",
                kSetSwapCallbackSymbol);
  }
  REXLOG_INFO("[ng2-ngpu] draw hand-off installed (stage 1: counting only), record {} bytes",
              sizeof(GpuDrawRecord));

  g_check_inputs = std::getenv("NG2_NATIVE_GPU_INPUTS") != nullptr;
  g_check_surface = std::getenv("NG2_NATIVE_GPU_SURFACE") != nullptr;
  if (g_check_inputs)
    REXLOG_INFO("[ng2-ngpu] resolving index buffers from guest memory (stage 2b step 1)");

  unsigned every_s = 5;
  if (const char* e = std::getenv("NG2_NATIVE_GPU_EVERY")) {
    const int v = std::atoi(e);
    if (v > 0) every_s = static_cast<unsigned>(v);
  }
  g_reporting.store(true);
  std::thread(Reporter, every_s).detach();
  REXLOG_INFO("[ng2-ngpu] reporting totals every {}s", every_s);
#endif
}

void Stop() {
#if defined(_WIN32)
  if (!g_setter) return;
  g_reporting.store(false);
  // The plugin holds a pointer into this module; clear it before anything here
  // goes away.
  if (g_swap_setter) {
    g_swap_setter(nullptr);
    g_swap_setter = nullptr;
  }
  g_setter(nullptr);
  g_setter = nullptr;
  REXLOG_INFO("[ng2-ngpu] totals: {}", Totals());
#endif
}

}  // namespace ng2::ngpu
