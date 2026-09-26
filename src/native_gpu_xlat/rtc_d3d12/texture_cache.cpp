// VENDORED from rexglue-src 23ace0b:src/graphics/d3d12/texture_cache.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (2 bool, 2 string, 6 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <filesystem>
#include <thread>
#include <algorithm>
#include <string>
#include <mutex>
#include <cstdio>
#include <set>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#include <rex/assert.h>
#include <rex/dbg.h>
#include "rtc_d3d12/command_processor.h"
#include "rtc_d3d12/shared_memory.h"
#include "rtc_d3d12/texture_cache.h"

namespace texpack_shaders {
#include "rtc_d3d12/bytecode/texpack_mip_cs.h"
}  // namespace texpack_shaders

// [texpack]
#include <chrono>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include "rtc_d3d12/d3d12_upload_buffer_pool.h"
#include "rtc_d3d12/d3d12_util.h"

// Hot-reloadable, like texture_pack_path: the app's settings screen turns
// dumping on with the pack off, which drops every texture, so the scene in
// front of the player is dumped there and then. Without this the switch only
// arrived through the tuning file at the next launch, and a player who ticked
// "dump", walked the level and came back found nothing written and no way to
// tell why.
bool& FLAGS_texture_dump_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("texture_dump", false); return s; }
std::string& FLAGS_texture_dump_path_storage_() { static std::string s = ::ng2::ngpu::xlat::PluginString("texture_dump_path", ""); return s; }
bool& FLAGS_texture_pack_resolve_at_load_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("texture_pack_resolve_at_load", true); return s; }
// Live counts, for the on-screen indicator. These are cvars because the app
// and the GPU plugin are separate DLLs with a one-way link, and the registry is
// the channel that already crosses that boundary (REXCVAR_QUERY). They are
// outputs, not settings - nothing should ever write them from a config file.
int32_t& FLAGS_texture_pack_replaced_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_pack_replaced", 0); return s; }
int32_t& FLAGS_texture_pack_original_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_pack_original", 0); return s; }

// Which chapter is loading, written by the app from the file the game opens
// for it. 0 means "not in a chapter", which is the state during boot and menus.
int32_t& FLAGS_texture_pack_chapter_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_pack_chapter", 0); return s; }

// Warming progress, published for the app to draw and to hold input on.
//
// Cvars rather than a shared object because the plugin and the executable are
// separate modules: the app cannot see this translation unit, and the cvar
// registry is the channel every other setting already uses. total > done means
// a stage is still being read.
int32_t& FLAGS_texture_warm_total_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_warm_total", 0); return s; }
int32_t& FLAGS_texture_warm_done_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_warm_done", 0); return s; }
int32_t& FLAGS_texture_pack_upload_budget_mb_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("texture_pack_upload_budget_mb", 0); return s; }

std::string& FLAGS_texture_pack_path_storage_() { static std::string s = ::ng2::ngpu::xlat::PluginString("texture_pack_path", ""); return s; }


// Generated with `xb buildshaders`.

namespace rex::graphics::ngpu_d3d12 {

// [texpack] Formats a shipped ART asset is actually stored in.
//
// The resident-memory load path also carries things that are regenerated every
// frame and are ruinous to replace from a pack: the video decoder's luma and
// chroma planes (k_8 at 1280x720 and 640x360), the scene resolve, and the HDR
// buffer (k_16_16_16_16_FLOAT). Rejecting them at the dump site rather than in
// the offline tool keeps the dump folder from filling with multi-megabyte
// framebuffers rewritten on every key change.
static bool TexturePackIsArtFormat(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_CTX1:
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_1_5_5_5:
    case xenos::TextureFormat::k_5_6_5:
    case xenos::TextureFormat::k_4_4_4_4:
      return true;
    default:
      return false;
  }
}


// --- [texpack] Per-stage residency ------------------------------------------
//
// The pack outgrew the cache: 4,125 files and 6 GB against a soft limit of 4 GB,
// which is how 1,290 textures once produced 2,900 uploads - the working set did
// not fit, so the cache evicted art it was about to need again. Loading the
// whole pack up front cannot fix that; it makes it worse.
//
// What does fit is one stage. A texture's identity is already its
// TexturePackId, so which stage uses it is a fact worth recording rather than a
// rename: the pack files stay exactly where they are and a sidecar per stage
// lists the ids. Nothing is duplicated, and a texture used in six stages is
// listed six times rather than copied six times.
//
// The list is built by OBSERVATION while playing - every replacement records
// itself against the chapter that was loading - so an existing pack needs no
// re-dump and no re-pack. A stage with no list yet simply behaves as it does
// today, which makes a half-observed pack strictly no worse than none.
namespace {

std::mutex g_stage_mutex;
int g_stage_current = 0;                       // chapter being recorded
std::set<std::string> g_stage_seen;            // "<id>-<hash>" keys seen in it this session
bool g_stage_dirty = false;
int g_stage_since_flush = 0;

std::filesystem::path StageListPath(const std::string& pack_dir, int chapter) {
  char name[32];
  std::snprintf(name, sizeof(name), "ch%02d.txt", chapter);
  return std::filesystem::path(pack_dir) / "stages" / name;
}

// Append-only, and merged with what is already on disk, so observations
// accumulate across sessions instead of the last run winning.
void StageFlush(const std::string& pack_dir) {
  if (!g_stage_dirty || g_stage_current <= 0 || pack_dir.empty())
    return;
  const auto path = StageListPath(pack_dir, g_stage_current);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::set<std::string> merged = g_stage_seen;
  if (std::ifstream in(path); in) {
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      // Lines from before content hashes ("<id>" alone) are dropped: once the
      // pack is migrated no file of that name exists, so there is nothing to
      // warm and keeping them would only hold the warming bar short of 100%.
      if (line.size() == 25 && line[16] == '-')
        merged.insert(line);
    }
  }
  if (std::ofstream out(path, std::ios::trunc); out) {
    for (const std::string& key : merged)
      out << key << "\n";
    REXGPU_INFO("[texpack] stage {} now lists {} textures", g_stage_current,
                merged.size());
  }
  g_stage_dirty = false;
}

void StageNote(const std::string& pack_dir, int chapter, uint64_t id, uint32_t hash) {
  if (chapter <= 0)
    return;
  std::lock_guard<std::mutex> lock(g_stage_mutex);
  if (chapter != g_stage_current) {
    StageFlush(pack_dir);                      // finish the stage we are leaving
    g_stage_current = chapter;
    g_stage_seen.clear();
  }
  if (g_stage_seen.insert(fmt::format("{:016X}-{:08X}", id, hash)).second) {
    g_stage_dirty = true;
    ++g_stage_since_flush;
  }
  // Write it out as we go, not only when the stage ends.
  //
  // A chapter change was the only flush, so a session that finished in the
  // stage it was recording - which is every session that ends by quitting -
  // lost the lot. The list is a set merged with what is on disk, so writing it
  // repeatedly costs one rewrite of a few thousand hex lines and can never
  // corrupt what is already there.
  if (g_stage_since_flush >= 64) {
    g_stage_since_flush = 0;
    StageFlush(pack_dir);
  }
}

// Reads a stage's files so the first draw that needs one is a memory copy
// rather than a cold read. Runs off the render thread: the point is to spend
// the loading screen, not a frame.
void StageWarm(const std::string& pack_dir, int chapter) {
  if (pack_dir.empty() || chapter <= 0)
    return;
  const auto path = StageListPath(pack_dir, chapter);
  std::ifstream in(path);
  if (!in)
    return;                                    // never played, nothing to warm
  // Count first, so the bar has a denominator from the moment it appears.
  // A progress bar that does not know its total either lies or cannot be drawn.
  int total = 0;
  {
    std::ifstream count(path);
    std::string line;
    while (std::getline(count, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      // Same test as the reader below, or done could never reach total.
      if (line.size() == 25 && line[16] == '-')
        ++total;
    }
  }
  if (total == 0)
    return;
  REXCVAR_SET(texture_warm_done, 0);
  REXCVAR_SET(texture_warm_total, total);

  std::thread([pack_dir, chapter, path, total] {
    std::ifstream list(path);
    std::string line;
    int warmed = 0;
    uint64_t bytes = 0;
    std::vector<char> scratch(1 << 16);
    while (std::getline(list, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      if (line.size() != 25 || line[16] != '-')
        continue;                              // pre-hash line: no such file to warm
      const auto file = std::filesystem::path(pack_dir) / (line + ".tex");
      std::ifstream f(file, std::ios::binary);
      if (!f) {
        // Still counts as done, or the bar never reaches its total and the
        // player is held at a screen that will not release.
        REXCVAR_SET(texture_warm_done, ++warmed);
        continue;
      }
      // Read and discard: the object is the OS page cache, not our memory. A
      // second copy here would double the footprint of the thing being fixed.
      while (f.read(scratch.data(), std::streamsize(scratch.size())) || f.gcount())
        bytes += uint64_t(f.gcount());
      REXCVAR_SET(texture_warm_done, ++warmed);
    }
    REXGPU_INFO("[texpack] warmed stage {}: {} files, {} MB", chapter, warmed,
                bytes / (1024 * 1024));
    // Leave done == total so the bar reads 100% and finishes green, then clear
    // the total so the overlay knows warming is over rather than merely full.
    REXCVAR_SET(texture_warm_done, total);
    REXCVAR_SET(texture_warm_total, 0);
  }).detach();
}

}  // namespace

// --- [texpack] Replacing guest textures with files from a pack --------------

// The id a dumped texture is filed under. The DUMP and the LOOKUP must agree
// exactly: if these ever drift, every lookup misses and the pack silently does
// nothing at all - no error, just the original textures. Hence one function.
// Templates because TextureCache::TextureKey is PROTECTED: a free function at
// namespace scope cannot name it, but a template instantiated from inside a
// member function can. The alternative was adding these to the SDK's own
// header, and keeping the fork's divergence to a single .cpp is worth more than
// the directness.
template <typename TKey>
static uint64_t TexturePackId(const TKey& tk) {
  const uint32_t tw = uint32_t(tk.width_minus_1) + 1;
  const uint32_t th = uint32_t(tk.height_minus_1) + 1;
  return (uint64_t(tk.base_page) << 40) ^ (uint64_t(uint32_t(tk.format)) << 33) ^
         (uint64_t(tk.tiled) << 32) ^ (uint64_t(tw) << 16) ^ uint64_t(th) ^
         (uint64_t(tk.pitch) << 48);
}

// magic + version + width + height
static constexpr size_t kTexHeaderBytes = 16;

struct TexturePackFile {
  uint32_t width = 0;
  uint32_t height = 0;
  std::string path;
};

// --- [texpack] Content hash: the id alone is NOT enough -----------------------
//
// TexturePackId is built from the texture's ADDRESS, format, size and pitch.
// Nothing in it describes the pixels. A game that streams its chapters reuses
// memory, so two different textures - the glass of a shop window in chapter 1,
// a normal map in chapter 12 - can carry the SAME id, and whichever was dumped
// first is what the pack then hands to both. Measured on Ninja Gaiden II: the
// window rendered violet with bumps, which is a normal map drawn as colour.
//
// So a pack file is named <id>-<hash>.tex, where hash is a CRC-32 of the
// guest bytes the dump wrote (the same bytes tools/upscale_textures.py reads),
// and the lookup hashes what is in memory NOW before it opens anything. A
// collision misses, is logged once, and the game's own texture is used - the
// wrong art can no longer be served. Old <id>.tex files are ignored (counted
// and reported, so a pack that suddenly "does nothing" says why): the tool
// renames them from the raw dump in one pass.
//
// CRC-32 with the zlib polynomial rather than the SDK's xxHash so the offline
// tool can reproduce it with the standard library and no extra package. Sliced
// by eight: about 2 GB/s, a 1 MB texture in half a millisecond, paid once per
// texture creation.
namespace {
struct Crc32Tables {
  uint32_t t[8][256];
  Crc32Tables() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
      for (int s = 1; s < 8; ++s)
        t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFu];
  }
};
}  // namespace

static uint32_t TexturePackContentHash(const uint8_t* p, size_t n) {
  static const Crc32Tables tables;
  const auto& t = tables.t;
  uint32_t c = 0xFFFFFFFFu;
  while (n >= 8) {
    uint32_t a, b;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
    a ^= c;
    c = t[7][a & 0xFFu] ^ t[6][(a >> 8) & 0xFFu] ^ t[5][(a >> 16) & 0xFFu] ^ t[4][a >> 24] ^
        t[3][b & 0xFFu] ^ t[2][(b >> 8) & 0xFFu] ^ t[1][(b >> 16) & 0xFFu] ^ t[0][b >> 24];
    p += 8;
    n -= 8;
  }
  while (n--)
    c = t[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// A texture is identified by BOTH: the id says where and what shape, the hash
// says which pixels.
using TexturePackKey = std::pair<uint64_t, uint32_t>;

// Presence and size are cached: this is asked once per texture creation and
// again on load, and hitting the filesystem for a texture that is NOT in the
// pack is the common case by far.
static std::mutex g_texpack_mutex;
static std::map<TexturePackKey, TexturePackFile> g_texpack_present;
static std::set<TexturePackKey> g_texpack_absent;
// What the pack folder holds, from ONE directory walk per pack path: id -> the
// hashes present. A miss is then a map lookup, not a failed open, and a
// collision (id present, hash not) can be told apart from a texture the pack
// simply does not cover.
static std::string g_texpack_index_dir;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_texpack_index;
static int g_texpack_legacy = 0;               // <id>.tex files, ignored
// [texpack] Content-addressed. The id carries the texture's ADDRESS in its
// high bits, and a game that streams puts the same texture at a different
// address on every visit: in Fable II's dump 13,080 of 35,545 distinct
// contents had been filed under two or more ids, one 128x128 under six. A
// lookup that needed the address to match therefore missed nearly
// everything - a session with a 28,587-file pack made zero replacements
// (2026-09-12). The pixels and the shape identify a texture; where the game
// happened to put it does not. So: hash -> the file ids that carry that
// content, and a match is any of them whose shape bits equal the lookup's.
// File names are unchanged, so NG2's packs and Fable II's need no migration.
static std::unordered_map<uint32_t, std::vector<uint64_t>> g_texpack_by_hash;
// shape -> one (file id, hash) of that shape: sizing before content is known.
static std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> g_texpack_by_shape;
// runtime id -> the (file id, hash) it was served from, so the stage list
// names files that exist rather than addresses that will not recur.
static std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> g_texpack_file_of;
// TexturePackId puts height, width, tiling and format in bits 0..39; the
// address (base_page) starts at bit 40 and the pitch at 48. Below 40 is shape.
static constexpr uint64_t kTexturePackShapeMask = (uint64_t(1) << 40) - 1;
static uint64_t TexturePackShape(uint64_t id) { return id & kTexturePackShapeMask; }
// Hash of the texture currently in guest memory, by id. Written when a texture
// is CREATED (the bytes are read then anyway), reused by the load and view
// lookups of that same texture so the bytes are hashed once, not three times.
// A texture the guest rewrites is re-created by the cache, which rehashes it.
static std::unordered_map<uint64_t, uint32_t> g_texpack_hash;
static std::set<uint64_t> g_texpack_collisions_logged;

// Under g_texpack_mutex.
static void TexturePackIndexBuild(const std::string& dir) {
  if (dir == g_texpack_index_dir)
    return;
  g_texpack_index.clear();
  g_texpack_by_hash.clear();
  g_texpack_by_shape.clear();
  g_texpack_index_dir = dir;
  g_texpack_legacy = 0;
  int hashed = 0;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (it->path().extension() != ".tex")
      continue;
    const std::string stem = it->path().stem().string();
    if (stem.size() == 25 && stem[16] == '-') {
      const uint64_t id = std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16);
      const uint32_t hash = uint32_t(std::strtoul(stem.substr(17, 8).c_str(), nullptr, 16));
      g_texpack_index[id].push_back(hash);
      g_texpack_by_hash[hash].push_back(id);
      g_texpack_by_shape.emplace(TexturePackShape(id), std::make_pair(id, hash));
      ++hashed;
    } else if (stem.size() == 16) {
      ++g_texpack_legacy;
    }
  }
  if (g_texpack_legacy) {
    REXLOG_WARN("[texpack] '{}': {} hashed files indexed; {} files named <id>.tex have no "
                "content hash and are IGNORED - run the texture tool once to migrate the pack",
                dir, hashed, g_texpack_legacy);
  } else {
    REXLOG_INFO("[texpack] '{}': {} hashed files indexed", dir, hashed);
  }
}

// The hash recorded for an id at its creation, for callers that only have the
// id (the stage list). 0 if the texture was never looked up.
static uint32_t TexturePackHashFor(uint64_t id) {
  std::lock_guard<std::mutex> lock(g_texpack_mutex);
  auto it = g_texpack_hash.find(id);
  return it == g_texpack_hash.end() ? 0u : it->second;
}

// Upload buffers still being read by the GPU. Replacements are uploaded once
// per texture rather than per frame, so a plain retirement list by submission
// index is enough - freeing one while the copy is still queued would corrupt
// the texture in a way that looks like a decode bug.
static std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>>
    g_texpack_uploads;

// [texpack] Uploads held back by the per-frame budget: textures whose
// upscaled copy is in the pack but whose bytes would push this frame past
// texture_pack_upload_budget_mb. The guest texture stays in use until
// BeginSubmission drains them, a budget's worth per frame, so a region load
// spreads its pack uploads over a few dozen frames instead of stacking them
// into the frames the game happens to load textures in (Bowerstone Market:
// ~1 GB of pack files in the first seconds, 2026-09-13). Entries are
// D3D12Texture pointers kept as void* (the class is nested); a texture
// removes itself in ~D3D12Texture. All on the GPU thread; the mutex is for
// the destructor, which may run elsewhere.
static std::vector<void*> g_texpack_pending;
static std::mutex g_texpack_pending_mutex;
static int64_t g_texpack_budget_left = 0;  // bytes still allowed this submission
static bool g_texpack_draining = false;     // inside the BeginSubmission drain

// guest / guest_size: the texture's base level as it sits in guest memory.
// rehash: true at texture CREATION (hash the bytes and remember them), false on
// the later load and view lookups of the same texture (reuse what creation
// found, so the bytes are hashed once).
// [texpack] Upscaled dimensions for an id, read once from any variant's header
// (all variants of an id share dimensions). For resolve-at-load, which sizes
// the resource at creation before the content - and so the specific file - is
// known.
static std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_texpack_dims;
static void TexturePackIndexBuild(const std::string& dir);
static bool TexturePackDimsForId(uint64_t id, uint32_t& w, uint32_t& h) {
  const std::string dir = rex::cvar::Query<std::string>("texture_pack_path");
  if (dir.empty())
    return false;
  std::lock_guard<std::mutex> lock(g_texpack_mutex);
  TexturePackIndexBuild(dir);
  auto di = g_texpack_dims.find(id);
  if (di != g_texpack_dims.end()) {
    w = di->second.first;
    h = di->second.second;
    return w != 0 && h != 0;
  }
  uint32_t rw = 0, rh = 0;
  uint64_t file_id = 0;
  uint32_t file_hash = 0;
  auto ix = g_texpack_index.find(id);
  if (ix != g_texpack_index.end() && !ix->second.empty()) {
    file_id = id;
    file_hash = ix->second.front();
  } else if (auto sh = g_texpack_by_shape.find(TexturePackShape(id));
             sh != g_texpack_by_shape.end()) {
    // Every file of this shape was upscaled from the same source size, so
    // any of them says how big the resource must be.
    file_id = sh->second.first;
    file_hash = sh->second.second;
  }
  if (file_id) {
    char p[600];
    std::snprintf(p, sizeof(p), "%s/%016llX-%08X.tex", dir.c_str(),
                  (unsigned long long)file_id, file_hash);
    std::ifstream f(p, std::ios::binary);
    uint8_t head[16] = {};
    if (f && f.read(reinterpret_cast<char*>(head), sizeof(head)) &&
        !std::memcmp(head, "NG2T", 4)) {
      uint32_t ver = 0;
      std::memcpy(&ver, head + 4, 4);
      std::memcpy(&rw, head + 8, 4);
      std::memcpy(&rh, head + 12, 4);
      if (ver != 1) { rw = 0; rh = 0; }
    }
  }
  g_texpack_dims[id] = {rw, rh};
  w = rw;
  h = rh;
  return rw != 0 && rh != 0;
}

template <typename TKey>
static const TexturePackFile* TexturePackLookup(const TKey& tk, const uint8_t* guest,
                                                uint32_t guest_size, bool rehash) {
  // A COPY taken under the registry lock, never the storage by reference.
  // The pack path is a hot-reloadable string the app rewrites from its UI
  // thread (F9, the settings screen); this runs on the render thread for
  // every texture load, and reading the string while the other thread
  // reassigns it is a use-after-free - measured on Fable II as a dead process
  // after two F9 presses one second apart.
  const std::string dir = rex::cvar::Query<std::string>("texture_pack_path");
  if (dir.empty())
    return nullptr;
  // Only formats the pack tool actually writes - the same set the dump keeps.
  if (!TexturePackIsArtFormat(tk.format)) {
    return nullptr;
  }
  // A replacement is a single 2D image. Cubes, 3D and arrays have more
  // subresources than one PNG can describe.
  if (tk.dimension != xenos::DataDimension::k2DOrStacked || tk.GetDepthOrArraySize() != 1)
    return nullptr;
  if (tk.base_page == 0)
    return nullptr;

  const uint64_t id = TexturePackId(tk);
  uint32_t hash = 0;
  bool have_hash = false;
  if (!rehash) {
    std::lock_guard<std::mutex> lock(g_texpack_mutex);
    auto h = g_texpack_hash.find(id);
    if (h != g_texpack_hash.end()) {
      hash = h->second;
      have_hash = true;
    }
  }
  if (!have_hash) {
    if (!guest || !guest_size)
      return nullptr;
    hash = TexturePackContentHash(guest, guest_size);   // outside the lock: up to a few MB
    std::lock_guard<std::mutex> lock(g_texpack_mutex);
    g_texpack_hash[id] = hash;
  }
  const TexturePackKey key{id, hash};
  uint64_t file_id = 0;
  {
    std::lock_guard<std::mutex> lock(g_texpack_mutex);
    TexturePackIndexBuild(dir);
    if (g_texpack_absent.count(key))
      return nullptr;
    auto it = g_texpack_present.find(key);
    if (it != g_texpack_present.end())
      return &it->second;
    // Content first: any file with these pixels and this shape, whatever
    // address it was dumped at.
    if (auto bh = g_texpack_by_hash.find(hash); bh != g_texpack_by_hash.end()) {
      const uint64_t shape = TexturePackShape(id);
      for (uint64_t fid : bh->second) {
        if (TexturePackShape(fid) == shape) {
          file_id = fid;
          break;
        }
      }
    }
    if (!file_id) {
      g_texpack_absent.insert(key);
      // The id is in the pack but with other pixels and nothing else carries
      // these pixels: say so once, because "the pack skipped one texture" is
      // otherwise indistinguishable from "the pack never covered it".
      auto ix = g_texpack_index.find(id);
      if (ix != g_texpack_index.end() && g_texpack_collisions_logged.insert(id).second) {
        REXLOG_INFO("[texpack] {:016X}: {} pack file(s) carry this id but none matches the "
                    "texture in memory (hash {:08X}) and no file carries that content; "
                    "the game's own texture is used",
                    id, ix->second.size(), hash);
      }
      return nullptr;
    }
    g_texpack_file_of[id] = {file_id, hash};
  }

  char path[512];
  std::snprintf(path, sizeof(path), "%s/%016llX-%08X.tex", dir.c_str(),
                (unsigned long long)file_id, hash);
  // "NG2T", u32 version, u32 width, u32 height, then raw RGBA. Raw rather than
  // PNG because decoding cost ~16ms per texture on the RENDER THREAD - see the
  // comment on the upload below.
  uint32_t w = 0, h = 0;
  bool ok = false;
  {
    std::ifstream f(path, std::ios::binary);
    uint8_t head[kTexHeaderBytes] = {};
    if (f && f.read(reinterpret_cast<char*>(head), sizeof(head)) &&
        !std::memcmp(head, "NG2T", 4)) {
      uint32_t version = 0;
      std::memcpy(&version, head + 4, 4);
      std::memcpy(&w, head + 8, 4);
      std::memcpy(&h, head + 12, 4);
      ok = version == 1 && w > 0 && h > 0 && w <= 16384 && h <= 16384;
      if (version != 1) {
        REXGPU_ERROR("[texpack] {} is version {}, this build reads version 1", path, version);
      }
    }
  }

  std::lock_guard<std::mutex> lock(g_texpack_mutex);
  if (!ok) {
    g_texpack_absent.insert(key);
    return nullptr;
  }
  TexturePackFile f;
  f.width = w;
  f.height = h;
  f.path = path;
  auto res = g_texpack_present.emplace(key, std::move(f));
  return &res.first->second;
}


namespace shaders {
#include "rtc_d3d12/bytecode/texture_load_128bpb_cs.h"
#include "rtc_d3d12/bytecode/texture_load_128bpb_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_16bpb_cs.h"
#include "rtc_d3d12/bytecode/texture_load_16bpb_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_32bpb_cs.h"
#include "rtc_d3d12/bytecode/texture_load_32bpb_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_64bpb_cs.h"
#include "rtc_d3d12/bytecode/texture_load_64bpb_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_8bpb_cs.h"
#include "rtc_d3d12/bytecode/texture_load_8bpb_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_bgrg8_rgb8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_bgrg8_rgbg8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_ctx1_cs.h"
#include "rtc_d3d12/bytecode/texture_load_depth_float_cs.h"
#include "rtc_d3d12/bytecode/texture_load_depth_float_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_depth_unorm_cs.h"
#include "rtc_d3d12/bytecode/texture_load_depth_unorm_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxn_rg8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt1_rgba8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt3_rgba8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt3a_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt3aas1111_bgra4_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt5_rgba8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_dxt5a_r8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_gbgr8_grgb8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_gbgr8_rgb8_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r10g11b11_rgba16_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r11g11b10_rgba16_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g6b5_b5g6r5_cs.h"
#include "rtc_d3d12/bytecode/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
}  // namespace shaders

const D3D12TextureCache::HostFormat D3D12TextureCache::host_formats_[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kLoadShaderIndexGBGR8ToGRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexGBGR8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, kLoadShaderIndexBGRG8ToRGBG8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexBGRG8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexDXNToRG8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    // R8_UNORM has the same size as BC2, but doesn't have the 4x4 size
    // alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexDXT3AAs1111ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

D3D12TextureCache::D3D12TextureCache(const RegisterFile& register_file,
                                     D3D12SharedMemory& shared_memory,
                                     uint32_t draw_resolution_scale_x,
                                     uint32_t draw_resolution_scale_y,
                                     D3D12CommandProcessor& command_processor,
                                     bool bindless_resources_used)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      bindless_resources_used_(bindless_resources_used) {}

D3D12TextureCache::~D3D12TextureCache() {
  // While the texture descriptor cache still exists (referenced by
  // ~D3D12Texture), destroy all textures.
  DestroyAllTextures(true);

  // First release the buffers to detach them from the heaps.
  for (std::unique_ptr<ScaledResolveVirtualBuffer>& scaled_resolve_buffer_ptr :
       scaled_resolve_2gb_buffers_) {
    scaled_resolve_buffer_ptr.reset();
  }
  scaled_resolve_heaps_.clear();
  COUNT_profile_set("gpu/texture_cache/scaled_resolve_buffer_used_mb", 0);
}

bool D3D12TextureCache::Initialize() {
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (IsDrawResolutionScaled()) {
    // Buffers not used yet - no need aliasing barriers to change ownership of
    // gigabytes between even and odd buffers.
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
    assert_true(scaled_resolve_heaps_.empty());
    uint64_t scaled_resolve_address_space_size =
        uint64_t(SharedMemory::kBufferSize) *
        (draw_resolution_scale_x() * draw_resolution_scale_y());
    scaled_resolve_heaps_.resize(
        size_t(scaled_resolve_address_space_size >> kScaledResolveHeapSizeLog2));
  }
  scaled_resolve_heap_count_ = 0;

  // GetSamplerParameters drops linear filtering for formats the host cannot
  // sample with it. Canary 197929d96.
  for (uint32_t i = 0; i < rex::countof(host_formats_); ++i) {
    const HostFormat& host_format = host_formats_[i];
    uint64_t format_bit = uint64_t(1) << i;
    if (host_format.dxgi_format_unsigned != DXGI_FORMAT_UNKNOWN) {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {
          host_format.dxgi_format_unsigned};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support,
                                                sizeof(format_support))) &&
          (format_support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) {
        host_filterable_unsigned_ |= format_bit;
      }
    }
    if (host_format.dxgi_format_signed != DXGI_FORMAT_UNKNOWN) {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support = {host_format.dxgi_format_signed};
      if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format_support,
                                                sizeof(format_support))) &&
          (format_support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE)) {
        host_filterable_signed_ |= format_bit;
      }
    }
  }

  // Create the loading root signature.
  D3D12_ROOT_PARAMETER root_parameters[3];
  // Parameter 0 is constants (changed multiple times when untiling).
  root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_parameters[0].Constants.ShaderRegister = 0;
  root_parameters[0].Constants.RegisterSpace = 0;
  root_parameters[0].Constants.Num32BitValues = sizeof(LoadConstants) / sizeof(uint32_t);
  root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the source (may be changed multiple times for the same
  // destination).
  D3D12_DESCRIPTOR_RANGE root_dest_range;
  root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  root_dest_range.NumDescriptors = 1;
  root_dest_range.BaseShaderRegister = 0;
  root_dest_range.RegisterSpace = 0;
  root_dest_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[1].DescriptorTable.pDescriptorRanges = &root_dest_range;
  root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the destination.
  D3D12_DESCRIPTOR_RANGE root_source_range;
  root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  root_source_range.NumDescriptors = 1;
  root_source_range.BaseShaderRegister = 0;
  root_source_range.RegisterSpace = 0;
  root_source_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[2].DescriptorTable.pDescriptorRanges = &root_source_range;
  root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
  root_signature_desc.NumParameters = UINT(rex::countof(root_parameters));
  root_signature_desc.pParameters = root_parameters;
  root_signature_desc.NumStaticSamplers = 0;
  root_signature_desc.pStaticSamplers = nullptr;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(load_root_signature_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!load_root_signature_) {
    REXGPU_ERROR(
        "D3D12TextureCache: Failed to create the texture loading root "
        "signature");
    return false;
  }

  // Specify the load shader code.
  D3D12_SHADER_BYTECODE load_shader_code[kLoadShaderCount] = {};
  load_shader_code[kLoadShaderIndex8bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs)};
  load_shader_code[kLoadShaderIndex16bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs)};
  load_shader_code[kLoadShaderIndex32bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs)};
  load_shader_code[kLoadShaderIndex64bpb] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs)};
  load_shader_code[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs)};
  load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b5a1_b5g5r5a1_cs,
                            sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs)};
  load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r5g6b5_b5g6r5_cs, sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs)};
  load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
                            sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs)};
  load_shader_code[kLoadShaderIndexRGBA4ToBGRA4] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r4g4b4a4_b4g4r4a4_cs,
                            sizeof(shaders::texture_load_r4g4b4a4_b4g4r4a4_cs)};
  load_shader_code[kLoadShaderIndexGBGR8ToGRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_gbgr8_grgb8_cs, sizeof(shaders::texture_load_gbgr8_grgb8_cs)};
  load_shader_code[kLoadShaderIndexGBGR8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_gbgr8_rgb8_cs, sizeof(shaders::texture_load_gbgr8_rgb8_cs)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGBG8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_bgrg8_rgbg8_cs, sizeof(shaders::texture_load_bgrg8_rgbg8_cs)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_bgrg8_rgb8_cs, sizeof(shaders::texture_load_bgrg8_rgb8_cs)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r10g11b11_rgba16_cs, sizeof(shaders::texture_load_r10g11b11_rgba16_cs)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_snorm_cs,
                            sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_r11g11b10_rgba16_cs, sizeof(shaders::texture_load_r11g11b10_rgba16_cs)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_snorm_cs,
                            sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs)};
  load_shader_code[kLoadShaderIndexDXT1ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt1_rgba8_cs, sizeof(shaders::texture_load_dxt1_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXT3ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt3_rgba8_cs, sizeof(shaders::texture_load_dxt3_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXT5ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt5_rgba8_cs, sizeof(shaders::texture_load_dxt5_rgba8_cs)};
  load_shader_code[kLoadShaderIndexDXNToRG8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxn_rg8_cs, sizeof(shaders::texture_load_dxn_rg8_cs)};
  load_shader_code[kLoadShaderIndexDXT3A] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3a_cs, sizeof(shaders::texture_load_dxt3a_cs)};
  load_shader_code[kLoadShaderIndexDXT3AAs1111ToBGRA4] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3aas1111_bgra4_cs,
                            sizeof(shaders::texture_load_dxt3aas1111_bgra4_cs)};
  load_shader_code[kLoadShaderIndexDXT5AToR8] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_dxt5a_r8_cs, sizeof(shaders::texture_load_dxt5a_r8_cs)};
  load_shader_code[kLoadShaderIndexCTX1] =
      D3D12_SHADER_BYTECODE{shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs)};
  load_shader_code[kLoadShaderIndexDepthUnorm] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_depth_unorm_cs, sizeof(shaders::texture_load_depth_unorm_cs)};
  load_shader_code[kLoadShaderIndexDepthFloat] = D3D12_SHADER_BYTECODE{
      shaders::texture_load_depth_float_cs, sizeof(shaders::texture_load_depth_float_cs)};
  D3D12_SHADER_BYTECODE load_shader_code_scaled[kLoadShaderCount] = {};
  if (IsDrawResolutionScaled()) {
    load_shader_code_scaled[kLoadShaderIndex8bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_8bpb_scaled_cs, sizeof(shaders::texture_load_8bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex16bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_16bpb_scaled_cs, sizeof(shaders::texture_load_16bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex32bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_32bpb_scaled_cs, sizeof(shaders::texture_load_32bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex64bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_64bpb_scaled_cs, sizeof(shaders::texture_load_64bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_128bpb_scaled_cs, sizeof(shaders::texture_load_128bpb_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs,
                              sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G6B5ToB5G6R5] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g6b5_b5g6r5_scaled_cs,
                              sizeof(shaders::texture_load_r5g6b5_b5g6r5_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs,
                              sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexRGBA4ToBGRA4] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r4g4b4a4_b4g4r4a4_scaled_cs,
                              sizeof(shaders::texture_load_r4g4b4a4_b4g4r4a4_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_scaled_cs,
                              sizeof(shaders::texture_load_r10g11b11_rgba16_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs,
                              sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_scaled_cs,
                              sizeof(shaders::texture_load_r11g11b10_rgba16_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs,
                              sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexDepthUnorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_depth_unorm_scaled_cs,
                              sizeof(shaders::texture_load_depth_unorm_scaled_cs)};
    load_shader_code_scaled[kLoadShaderIndexDepthFloat] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_depth_float_scaled_cs,
                              sizeof(shaders::texture_load_depth_float_scaled_cs)};
  }

  // Create the loading pipelines.
  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    const D3D12_SHADER_BYTECODE& current_load_shader_code = load_shader_code[i];
    if (!current_load_shader_code.pShaderBytecode) {
      continue;
    }
    *(load_pipelines_[i].ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
        device, current_load_shader_code.pShaderBytecode, current_load_shader_code.BytecodeLength,
        load_root_signature_.Get());
    if (!load_pipelines_[i]) {
      REXGPU_ERROR(
          "D3D12TextureCache: Failed to create the texture loading pipeline "
          "for shader {}",
          i);
      return false;
    }
    if (IsDrawResolutionScaled()) {
      const D3D12_SHADER_BYTECODE& current_load_shader_code_scaled = load_shader_code_scaled[i];
      if (current_load_shader_code_scaled.pShaderBytecode) {
        *(load_pipelines_scaled_[i].ReleaseAndGetAddressOf()) =
            ui::ngpu_d3d12::util::CreateComputePipeline(
                device, current_load_shader_code_scaled.pShaderBytecode,
                current_load_shader_code_scaled.BytecodeLength, load_root_signature_.Get());
        if (!load_pipelines_scaled_[i]) {
          REXGPU_ERROR(
              "D3D12TextureCache: Failed to create the resolution-scaled "
              "texture loading pipeline for shader {}",
              i);
          return false;
        }
      }
    }
  }

  srv_descriptor_cache_allocated_ = 0;

  // Create a heap with null SRV descriptors, since it's faster to copy a
  // descriptor than to create an SRV, and null descriptors are used a lot (for
  // the signed version when only unsigned is used, for instance).
  D3D12_DESCRIPTOR_HEAP_DESC null_srv_descriptor_heap_desc;
  null_srv_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  null_srv_descriptor_heap_desc.NumDescriptors = uint32_t(NullSRVDescriptorIndex::kCount);
  null_srv_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  null_srv_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&null_srv_descriptor_heap_desc,
                                          IID_PPV_ARGS(&null_srv_descriptor_heap_)))) {
    REXGPU_ERROR(
        "D3D12TextureCache: Failed to create the descriptor heap for null "
        "SRVs");
    return false;
  }
  null_srv_descriptor_heap_start_ = null_srv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
  null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  null_srv_desc.Texture2DArray.MostDetailedMip = 0;
  null_srv_desc.Texture2DArray.MipLevels = 1;
  null_srv_desc.Texture2DArray.FirstArraySlice = 0;
  null_srv_desc.Texture2DArray.ArraySize = 1;
  null_srv_desc.Texture2DArray.PlaneSlice = 0;
  null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::k2DArray)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  null_srv_desc.Texture3D.MostDetailedMip = 0;
  null_srv_desc.Texture3D.MipLevels = 1;
  null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::k3D)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  null_srv_desc.TextureCube.MostDetailedMip = 0;
  null_srv_desc.TextureCube.MipLevels = 1;
  null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    uint32_t(NullSRVDescriptorIndex::kCube)));

  return true;
}

void D3D12TextureCache::ClearCache() {
  TextureCache::ClearCache();

  // Clear texture descriptor cache.
  srv_descriptor_cache_free_.clear();
  srv_descriptor_cache_allocated_ = 0;
  srv_descriptor_cache_.clear();
}

void D3D12TextureCache::BeginSubmission(uint64_t new_submission_index) {
  TextureCache::BeginSubmission(new_submission_index);

  // [texpack] A fresh upload budget, then the uploads held back earlier, as
  // many as it covers. The command processor reset the deferred command list
  // just before calling this, so the copies land in this submission.
  g_texpack_budget_left = int64_t(REXCVAR_GET(texture_pack_upload_budget_mb)) << 20;
  {
    std::vector<void*> pending;
    {
      std::lock_guard<std::mutex> lock(g_texpack_pending_mutex);
      pending.swap(g_texpack_pending);
    }
    size_t done = 0;
    g_texpack_draining = true;
    for (; done < pending.size() && g_texpack_budget_left > 0; ++done) {
      auto* texture = static_cast<D3D12Texture*>(pending[done]);
      ApplyTexpackResolve(*texture, texture->key());
    }
    g_texpack_draining = false;
    if (done < pending.size()) {
      // The leftovers keep their place; anything queued meanwhile goes behind.
      std::lock_guard<std::mutex> lock(g_texpack_pending_mutex);
      pending.erase(pending.begin(), pending.begin() + ptrdiff_t(done));
      pending.insert(pending.end(), g_texpack_pending.begin(), g_texpack_pending.end());
      g_texpack_pending.swap(pending);
    }
  }

  // [texpack] A new stage: finish recording the last one and warm this one.
  //
  // Checked per submission rather than hooked, for the same reason the pack
  // path is: this is the one place that runs on the GPU thread every frame and
  // already reads its cvars here.
  {
    static int last_chapter = 0;
    const int chapter = REXCVAR_GET(texture_pack_chapter);
    if (chapter != last_chapter) {
      last_chapter = chapter;
      const std::string pack = rex::cvar::Query<std::string>("texture_pack_path");
      if (!pack.empty()) {
        {
          std::lock_guard<std::mutex> lock(g_stage_mutex);
          StageFlush(pack);
        }
        StageWarm(pack, chapter);
      }
    }
  }

  // [texpack] Turning the pack on or off takes effect immediately.
  //
  // Textures are created once and kept, so changing the path alone would only
  // affect textures loaded AFTER it - the scene in front of you would not
  // change, which makes comparing the pack against the original almost
  // impossible. Dropping every texture forces them all to reload from whichever
  // source is now selected. The clear is only REQUESTED here; the command
  // processor performs it at end of frame, after waiting for the GPU.
  {
    static std::string applied_pack_path;
    // Copied under the registry lock - see TexturePackLookup for why.
    const std::string want = rex::cvar::Query<std::string>("texture_pack_path");
    if (want != applied_pack_path) {
      applied_pack_path = want;
      {
        std::lock_guard<std::mutex> lock(g_texpack_mutex);
        g_texpack_present.clear();
        g_texpack_absent.clear();
        g_texpack_hash.clear();
        g_texpack_index.clear();
        g_texpack_by_hash.clear();
        g_texpack_by_shape.clear();
        g_texpack_file_of.clear();
        g_texpack_dims.clear();
        g_texpack_index_dir.clear();          // re-walk the folder on the next lookup
        g_texpack_collisions_logged.clear();
      }
      // The counts describe the mode that is running now, so they restart with
      // it - otherwise the indicator would show the sum of both modes and mean
      // nothing.
      REXCVAR_SET(texture_pack_replaced, 0);
      REXCVAR_SET(texture_pack_original, 0);
      REXLOG_INFO("[texpack] pack path changed to '{}' - reloading every texture",
                  want.empty() ? "(off)" : want.c_str());
      // Requested here, performed by the command processor at end of frame
      // after it has drained the GPU - see cache_clear_requested_. This was
      // once suspected of corrupting the guest command stream, but the ring
      // buffer errors it was blamed for occur with it disabled too; they belong
      // to the attract demo, not to this.
      command_processor_.ClearCaches();
    }
  }

  // [texpack] Dumping switched on, or pointed at another folder: drop every
  // texture too. The dump runs per texture LOAD, so without this only what
  // loads AFTER the switch is written and the scene already in memory is
  // silently missed - which on NG2 read as "dumping does nothing" and made
  // dumping a restart-required setting there. Dropping the cache makes every
  // resident texture load again, and the dump's own id+hash set skips what it
  // wrote before, so nothing is written twice. Switching dumping OFF changes
  // nothing that is drawn, so it is not worth a reload.
  {
    static std::string applied_dump;
    const std::string want_dump =
        REXCVAR_GET(texture_dump) ? rex::cvar::Query<std::string>("texture_dump_path")
                                  : std::string();
    if (want_dump != applied_dump) {
      const bool reload = !want_dump.empty();
      applied_dump = want_dump;
      if (reload) {
        REXLOG_INFO("[texpack] dumping to '{}' - reloading every texture so the scene in "
                    "memory is written too",
                    want_dump);
        command_processor_.ClearCaches();
      }
    }
  }

  // ExecuteCommandLists is a full UAV and aliasing barrier.
  if (IsDrawResolutionScaled()) {
    size_t scaled_resolve_buffer_count = GetScaledResolveBufferCount();
    for (size_t i = 0; i < scaled_resolve_buffer_count; ++i) {
      ScaledResolveVirtualBuffer* scaled_resolve_buffer = scaled_resolve_2gb_buffers_[i].get();
      if (scaled_resolve_buffer) {
        scaled_resolve_buffer->ClearUAVBarrierPending();
      }
    }
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
  }
}

void D3D12TextureCache::BeginFrame() {
  TextureCache::BeginFrame();

  std::memset(unsupported_format_features_used_, 0, sizeof(unsupported_format_features_used_));
}

void D3D12TextureCache::EndFrame() {
  // Report used unsupported texture formats.
  bool unsupported_header_written = false;
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t unsupported_features = unsupported_format_features_used_[i];
    if (unsupported_features == 0) {
      continue;
    }
    if (!unsupported_header_written) {
      REXGPU_ERROR("Unsupported texture formats used in the frame:");
      unsupported_header_written = true;
    }
    REXGPU_ERROR("* {}{}{}{}", FormatInfo::Get(xenos::TextureFormat(i))->name,
                 unsupported_features & kUnsupportedResourceBit ? " resource" : "",
                 unsupported_features & kUnsupportedUnormBit ? " unsigned" : "",
                 unsupported_features & kUnsupportedSnormBit ? " signed" : "");
    unsupported_format_features_used_[i] = 0;
  }
}

void D3D12TextureCache::RequestTextures(uint32_t used_texture_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  TextureCache::RequestTextures(used_texture_mask);

  // Pre-create 3D-as-2D wrappers before draw setup. Wrapper loading may bind
  // compute pipelines and must happen in the texture request phase.
  if (REXCVAR_GET(gpu_3d_to_2d_texture)) {
    uint32_t textures_3d = used_texture_mask;
    uint32_t index_3d;
    while (rex::bit_scan_forward(textures_3d, &index_3d)) {
      textures_3d &= ~(uint32_t(1) << index_3d);
      const TextureBinding* binding = GetValidTextureBinding(index_3d);
      if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
        continue;
      }
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        texture->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      D3D12Texture* texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
      if (texture_signed) {
        texture_signed->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  // Transition the textures to the needed usage - always in
  // NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE states because barriers
  // between read-only stages, if needed, are discouraged (also if these were
  // tracked separately, checks would be needed to make sure, if the same
  // texture is bound through different fetch constants to both VS and PS, it
  // would be in both states).
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    D3D12Texture* binding_texture = static_cast<D3D12Texture*>(binding->texture);
    if (binding_texture != nullptr) {
      // Will be referenced by the command list, so mark as used.
      TexpackReverify(*binding_texture);
      binding_texture->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture->resource(),
          binding_texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    D3D12Texture* binding_texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
    if (binding_texture_signed != nullptr) {
      binding_texture_signed->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture_signed->resource(),
          binding_texture_signed->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }
}

bool D3D12TextureCache::AreActiveTextureSRVKeysUpToDate(
    const TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    const TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      if (key.key.is_valid) {
        return false;
      }
      continue;
    }
    if (key.key != binding->key || key.host_swizzle != binding->host_swizzle ||
        key.swizzled_signs != binding->swizzled_signs ||
        key.generation != SRVGenerationOf(binding)) {
      return false;
    }
  }
  return true;
}

void D3D12TextureCache::WriteActiveTextureSRVKeys(
    TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      key.key.MakeInvalid();
      key.host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_0000;
      key.swizzled_signs = kSwizzledSignsUnsigned;
      key.generation = 0;
      continue;
    }
    key.key = binding->key;
    key.host_swizzle = binding->host_swizzle;
    key.swizzled_signs = binding->swizzled_signs;
    key.generation = SRVGenerationOf(binding);
  }
}

void D3D12TextureCache::WriteActiveTextureBindfulSRV(
    const D3D12Shader::TextureBinding& host_shader_binding, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  assert_false(bindless_resources_used_);
  uint32_t descriptor_index = UINT32_MAX;
  Texture* texture = nullptr;
  uint32_t fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (host_shader_binding.is_signed) {
      // Not supporting signed compressed textures - hopefully DXN and DXT5A are
      // not used as signed.
      if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked, true,
                                                           binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index_signed;
        }
      }
    } else {
      if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
        texture = binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked,
                                                           false, binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index;
        }
      }
    }
  }
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_CPU_DESCRIPTOR_HANDLE source_handle;
  if (descriptor_index != UINT32_MAX) {
    assert_not_null(texture);
    texture->MarkAsUsed();
    source_handle = GetTextureDescriptorCPUHandle(descriptor_index);
  } else {
    NullSRVDescriptorIndex null_descriptor_index;
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        null_descriptor_index = NullSRVDescriptorIndex::k3D;
        break;
      case xenos::FetchOpDimension::kCube:
        null_descriptor_index = NullSRVDescriptorIndex::kCube;
        break;
      default:
        assert_true(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                    host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        null_descriptor_index = NullSRVDescriptorIndex::k2DArray;
    }
    source_handle = provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                                  uint32_t(null_descriptor_index));
  }
  auto device = provider.GetDevice();
  {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
    SCOPE_profile_cpu_i("gpu",
                        "rex::graphics::d3d12::D3D12TextureCache::WriteActiveTextureBindfulSRV->"
                        "CopyDescriptorsSimple");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
    device->CopyDescriptorsSimple(1, handle, source_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
}

uint32_t D3D12TextureCache::GetActiveTextureBindlessSRVIndex(
    const D3D12Shader::TextureBinding& host_shader_binding) {
  assert_true(bindless_resources_used_);
  uint32_t descriptor_index = UINT32_MAX;
  uint32_t fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (force_special_view) {
      Texture* texture = nullptr;
      bool use_signed =
          host_shader_binding.is_signed && texture_util::IsAnySignSigned(binding->swizzled_signs);
      if (use_signed) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
      } else {
        texture = binding->texture;
      }
      if (texture) {
        descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                         xenos::DataDimension::k2DOrStacked,
                                                         use_signed, binding->host_swizzle);
      }
    } else {
      descriptor_index = host_shader_binding.is_signed ? d3d12_binding.descriptor_index_signed
                                                       : d3d12_binding.descriptor_index;
    }
  }
  if (descriptor_index == UINT32_MAX) {
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTexture3D);
        break;
      case xenos::FetchOpDimension::kCube:
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTextureCube);
        break;
      default:
        assert_true(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                    host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        descriptor_index = uint32_t(D3D12CommandProcessor::SystemBindlessView::kNullTexture2DArray);
    }
  }
  return descriptor_index;
}

D3D12TextureCache::SamplerParameters D3D12TextureCache::GetSamplerParameters(
    const D3D12Shader::SamplerBinding& binding) const {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
    parameters.force_bc_w_to_max = fetch.force_bc_w_to_max;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t base_page, mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                 nullptr, &mip_min_level, &mip_max_level);

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  // With kBaseMap and a real base page, the base map IS level 0 - keeping the
  // fetch constant's min level would sample the wrong subresource.
  // Canary 4a863a0e1.
  if (mip_base_map && base_page != 0) {
    mip_min_level = 0;
  }
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;

  // Fall back to point sampling if the host formats don't report
  // D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE.
  if (parameters.mag_linear || parameters.min_linear || parameters.mip_linear ||
      parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    TextureKey texture_key;
    uint8_t texture_swizzled_signs;
    BindingInfoFromFetchConstant(fetch, texture_key, &texture_swizzled_signs);
    uint64_t format_bit =
        texture_key.is_valid ? uint64_t(1) << uint32_t(texture_key.format) : 0;
    if (!texture_key.is_valid ||
        (texture_util::IsAnySignNotSigned(texture_swizzled_signs) &&
         (host_filterable_unsigned_ & format_bit) == 0) ||
        (texture_util::IsAnySignSigned(texture_swizzled_signs) &&
         (host_filterable_signed_ & format_bit) == 0)) {
      parameters.mag_linear = 0;
      parameters.min_linear = 0;
      parameters.mip_linear = 0;
      parameters.aniso_filter = xenos::AnisoFilter::kDisabled;
    }
  }

  return parameters;
}

void D3D12TextureCache::WriteSampler(SamplerParameters parameters,
                                     D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  D3D12_SAMPLER_DESC desc;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(parameters.aniso_filter) - 1);
  } else {
    D3D12_FILTER_TYPE d3d_filter_min =
        parameters.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mag =
        parameters.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mip =
        parameters.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(d3d_filter_min, d3d_filter_mag, d3d_filter_mip,
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D12_TEXTURE_ADDRESS_MODE_WRAP,
      /* kMirroredRepeat       */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      /* kClampToEdge          */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      /* kMirrorClampToEdge    */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D
      // 12, but there's no Direct3D 9 equivalent anyway, and too weird to be
      // suitable for intentional real usage.
      /* kClampToHalfway       */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      // No mirror and clamp to border equivalents in Direct3D 12, but they
      // aren't there in Direct3D 9 either.
      /* kMirrorClampToHalfway */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      /* kClampToBorder        */ D3D12_TEXTURE_ADDRESS_MODE_BORDER,
      /* kMirrorClampToBorder  */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[uint32_t(parameters.clamp_x)];
  desc.AddressV = kAddressModeMap[uint32_t(parameters.clamp_y)];
  desc.AddressW = kAddressModeMap[uint32_t(parameters.clamp_z)];
  // LOD biasing is performed in shaders.
  desc.MipLODBias = 0.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      assert_true(parameters.border_color == xenos::BorderColor::k_ABGR_Black);
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  if (parameters.force_bc_w_to_max) {
    desc.BorderColor[3] = 1.0f;
  }
  desc.MinLOD = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    // "It is undefined whether LOD clamping based on MinLOD and MaxLOD Sampler
    // states should happen before or after deciding if magnification is
    // occuring" - Direct3D 11.3 Functional Specification.
    // Using the GL_NEAREST / GL_LINEAR minification filter emulation logic
    // described in the Vulkan VkSamplerCreateInfo specification, preserving
    // magnification vs. minification - point mip sampling (usable only without
    // anisotropic filtering on Direct3D 12) and MaxLOD 0.25. With anisotropic
    // filtering, magnification vs. minification doesn't matter as the filter is
    // always linear for both on Direct3D 12 - but linear filtering specifically
    // is what must not be done for kBaseMap, so setting MaxLOD to MinLOD.
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      assert_false(parameters.mip_linear);
      desc.MaxLOD += 0.25f;
    }
  } else {
    // Maximum mip level is in the texture resource itself.
    desc.MaxLOD = FLT_MAX;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  device->CreateSampler(&desc, handle);
}

bool D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
    uint32_t& scale_x, uint32_t& scale_y, const ui::ngpu_d3d12::D3D12Provider& provider) {
  bool was_clamped;
  if (provider.GetTiledResourcesTier() < D3D12_TILED_RESOURCES_TIER_1) {
    was_clamped = scale_x > 1 || scale_y > 1;
    scale_x = 1;
    scale_y = 1;
    return !was_clamped;
  }
  // Limit to the virtual address space available for a resource.
  was_clamped = false;
  uint32_t virtual_address_bits_per_resource = provider.GetVirtualAddressBitsPerResource();
  while (scale_x > 1 || scale_y > 1) {
    uint64_t highest_scaled_address = uint64_t(SharedMemory::kBufferSize) * (scale_x * scale_y) - 1;
    if (uint32_t(64) - rex::lzcnt(highest_scaled_address) <= virtual_address_bits_per_resource) {
      break;
    }
    // When reducing from a square size, prefer decreasing the horizontal
    // resolution as vertical resolution difference is visible more clearly in
    // perspective.
    was_clamped = true;
    if (scale_x >= scale_y) {
      --scale_x;
    } else {
      --scale_y;
    }
  }
  return !was_clamped;
}

bool D3D12TextureCache::EnsureScaledResolveMemoryCommitted(uint32_t start_unscaled,
                                                           uint32_t length_unscaled,
                                                           uint32_t length_scaled_alignment_log2) {
  assert_true(IsDrawResolutionScaled());

  if (length_unscaled == 0) {
    return true;
  }
  if (start_unscaled > SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    // Exceeds the physical address space.
    return false;
  }

  uint32_t draw_resolution_scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t first_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t last_scaled =
      (uint64_t(start_unscaled + (length_unscaled - 1)) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;

  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Ensure GPU virtual memory for buffers that may be used to access the range
  // is allocated - buffers are created. Always creating both buffers for all
  // addresses before creating the heaps so when creating a new buffer, it can
  // be safely assumed that no existing heaps should be mapped to it.
  std::array<size_t, 2> possible_buffers_first =
      GetPossibleScaledResolveBufferIndices(first_scaled);
  std::array<size_t, 2> possible_buffers_last = GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_first = std::min(possible_buffers_first[0], possible_buffers_first[1]);
  size_t possible_buffer_last = std::max(possible_buffers_last[0], possible_buffers_last[1]);
  for (size_t i = possible_buffer_first; i <= possible_buffer_last; ++i) {
    if (scaled_resolve_2gb_buffers_[i]) {
      continue;
    }
    D3D12_RESOURCE_DESC scaled_resolve_buffer_desc;
    // Buffer indices are gigabytes.
    ui::ngpu_d3d12::util::FillBufferResourceDesc(
        scaled_resolve_buffer_desc,
        std::min(
            uint64_t(1) << 31,
            uint64_t(SharedMemory::kBufferSize) * draw_resolution_scale_area - (uint64_t(i) << 30)),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // The first access will be a resolve.
    constexpr D3D12_RESOURCE_STATES kScaledResolveVirtualBufferInitialState =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* scaled_resolve_buffer_resource;
    if (FAILED(device->CreateReservedResource(&scaled_resolve_buffer_desc,
                                              kScaledResolveVirtualBufferInitialState, nullptr,
                                              IID_PPV_ARGS(&scaled_resolve_buffer_resource)))) {
      REXGPU_ERROR(
          "D3D12TextureCache: Failed to create a 2 GB tiled buffer for draw "
          "resolution scaling");
      return false;
    }
    scaled_resolve_2gb_buffers_[i] =
        std::unique_ptr<ScaledResolveVirtualBuffer>(new ScaledResolveVirtualBuffer(
            scaled_resolve_buffer_resource, kScaledResolveVirtualBufferInitialState));
    scaled_resolve_buffer_resource->Release();
  }

  uint32_t heap_first = uint32_t(first_scaled >> kScaledResolveHeapSizeLog2);
  uint32_t heap_last = uint32_t(last_scaled >> kScaledResolveHeapSizeLog2);
  for (uint32_t i = heap_first; i <= heap_last; ++i) {
    if (scaled_resolve_heaps_[i]) {
      continue;
    }
    auto direct_queue = provider.GetDirectQueue();
    command_processor_.DrainSubmissions();   // NATIVE PATCH: [async submit] keep queue order with earlier submissions
    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = kScaledResolveHeapSize;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
    Microsoft::WRL::ComPtr<ID3D12Heap> scaled_resolve_heap;
    if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&scaled_resolve_heap)))) {
      REXGPU_ERROR("D3D12TextureCache: Failed to create a scaled resolve tile heap");
      return false;
    }
    scaled_resolve_heaps_[i] = scaled_resolve_heap;
    ++scaled_resolve_heap_count_;
    COUNT_profile_set("gpu/texture_cache/scaled_resolve_buffer_used_mb",
                      scaled_resolve_heap_count_ << (kScaledResolveHeapSizeLog2 - 20));
    D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
    region_start_coordinates.Y = 0;
    region_start_coordinates.Z = 0;
    region_start_coordinates.Subresource = 0;
    D3D12_TILE_REGION_SIZE region_size;
    region_size.NumTiles = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    region_size.UseBox = FALSE;
    D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_range_start_offset = 0;
    UINT range_tile_count = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    std::array<size_t, 2> buffer_indices =
        GetPossibleScaledResolveBufferIndices(uint64_t(i) << kScaledResolveHeapSizeLog2);
    for (size_t j = 0; j < 2; ++j) {
      size_t buffer_index = buffer_indices[j];
      if (j && buffer_index == buffer_indices[0]) {
        break;
      }
      region_start_coordinates.X =
          UINT(((uint64_t(i) << kScaledResolveHeapSizeLog2) - (uint64_t(buffer_index) << 30)) /
               D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
      direct_queue->UpdateTileMappings(
          scaled_resolve_2gb_buffers_[buffer_index]->resource(), 1, &region_start_coordinates,
          &region_size, scaled_resolve_heap.Get(), 1, &range_flags, &heap_range_start_offset,
          &range_tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    command_processor_.NotifyQueueOperationsDoneDirectly();
  }
  return true;
}

bool D3D12TextureCache::MakeScaledResolveRangeCurrent(uint32_t start_unscaled,
                                                      uint32_t length_unscaled,
                                                      uint32_t length_scaled_alignment_log2) {
  assert_true(IsDrawResolutionScaled());

  if (!length_unscaled || start_unscaled >= SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    // If length is 0, the needed buffer can't be chosen because no buffer is
    // needed.
    return false;
  }

  uint32_t draw_resolution_scale_area = draw_resolution_scale_x() * draw_resolution_scale_y();
  uint64_t start_scaled = uint64_t(start_unscaled) * draw_resolution_scale_area;
  uint64_t length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  uint64_t length_scaled =
      (uint64_t(length_unscaled) * draw_resolution_scale_area + length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;
  uint64_t last_scaled = start_scaled + (length_scaled - 1);

  // Get one or two buffers that can hold the whole range.
  std::array<size_t, 2> possible_buffer_indices_first =
      GetPossibleScaledResolveBufferIndices(start_scaled);
  std::array<size_t, 2> possible_buffer_indices_last =
      GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_indices_common[2];
  size_t possible_buffer_indices_common_count = 0;
  for (size_t i = 0;
       i <= size_t(possible_buffer_indices_first[0] != possible_buffer_indices_first[1]); ++i) {
    size_t possible_buffer_index_first = possible_buffer_indices_first[i];
    for (size_t j = 0;
         j <= size_t(possible_buffer_indices_last[0] != possible_buffer_indices_last[1]); ++j) {
      if (possible_buffer_indices_last[j] == possible_buffer_index_first) {
        bool possible_buffer_index_already_added = false;
        for (size_t k = 0; k < possible_buffer_indices_common_count; ++k) {
          if (possible_buffer_indices_common[k] == possible_buffer_index_first) {
            possible_buffer_index_already_added = true;
            break;
          }
        }
        if (!possible_buffer_index_already_added) {
          assert_true(possible_buffer_indices_common_count < 2);
          possible_buffer_indices_common[possible_buffer_indices_common_count++] =
              possible_buffer_index_first;
        }
      }
    }
  }
  if (!possible_buffer_indices_common_count) {
    // Too wide range requested - no buffer that contains both the start and the
    // end.
    return false;
  }

  size_t gigabyte_first = size_t(start_scaled >> 30);
  size_t gigabyte_last = size_t(last_scaled >> 30);

  // Choose the buffer that the range will be accessed through.
  size_t new_buffer_index;
  if (possible_buffer_indices_common_count >= 2) {
    // Prefer the buffer that is already used to make less aliasing barriers.
    assert_true(gigabyte_first + 1 >= gigabyte_last);
    size_t possible_buffer_indices_already_used[2] = {};
    for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
      size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
      for (size_t j = 0; j < possible_buffer_indices_common_count; ++j) {
        if (possible_buffer_indices_common[j] == gigabyte_current_buffer_index) {
          ++possible_buffer_indices_already_used[j];
        }
      }
    }
    new_buffer_index = possible_buffer_indices_common[size_t(
        possible_buffer_indices_already_used[1] > possible_buffer_indices_already_used[0])];
  } else {
    // The range can be accessed only by one buffer.
    new_buffer_index = possible_buffer_indices_common[0];
  }

  // Switch the current buffer for the range.
  const ScaledResolveVirtualBuffer* new_buffer =
      scaled_resolve_2gb_buffers_[new_buffer_index].get();
  assert_not_null(new_buffer);
  ID3D12Resource* new_buffer_resource = new_buffer->resource();
  for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
    size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
    if (gigabyte_current_buffer_index == new_buffer_index) {
      continue;
    }
    if (gigabyte_current_buffer_index != SIZE_MAX) {
      ScaledResolveVirtualBuffer* gigabyte_current_buffer =
          scaled_resolve_2gb_buffers_[gigabyte_current_buffer_index].get();
      assert_not_null(gigabyte_current_buffer);
      command_processor_.PushAliasingBarrier(gigabyte_current_buffer->resource(),
                                             new_buffer_resource);
      // An aliasing barrier synchronizes and flushes everything.
      gigabyte_current_buffer->ClearUAVBarrierPending();
    }
    scaled_resolve_1gb_buffer_indices_[i] = new_buffer_index;
  }

  scaled_resolve_current_range_start_scaled_ = start_scaled;
  scaled_resolve_current_range_length_scaled_ = length_scaled;
  return true;
}

void D3D12TextureCache::TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATES new_state) {
  assert_true(IsDrawResolutionScaled());
  ScaledResolveVirtualBuffer& buffer = GetCurrentScaledResolveBuffer();
  command_processor_.PushTransitionBarrier(buffer.resource(), buffer.SetResourceState(new_state),
                                           new_state);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2SRV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  assert_true(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  assert_not_null(buffer);
  ui::ngpu_d3d12::util::CreateBufferTypedSRV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->resource(),
      ui::ngpu_d3d12::util::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      uint32_t(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (uint64_t(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2UAV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, uint32_t element_size_bytes_pow2) {
  assert_true(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  assert_not_null(buffer);
  ui::ngpu_d3d12::util::CreateBufferTypedUAV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->resource(),
      ui::ngpu_d3d12::util::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      uint32_t(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (uint64_t(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

ID3D12Resource* D3D12TextureCache::RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
                                                      xenos::TextureFormat& format_out,
                                                      uint32_t* width_unscaled_out,
                                                      uint32_t* height_unscaled_out) {
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return nullptr;
  }
  D3D12Texture* texture = static_cast<D3D12Texture*>(FindOrCreateTexture(key));
  if (texture == nullptr || !LoadTextureData(*texture)) {
    return nullptr;
  }
  texture->MarkAsUsed();
  // The swap texture is likely to be used only for the presentation compute
  // shader, and not during emulation, where it'd be NON_PIXEL_SHADER_RESOURCE |
  // PIXEL_SHADER_RESOURCE.
  ID3D12Resource* texture_resource = texture->resource();
  command_processor_.PushTransitionBarrier(
      texture_resource, texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  srv_desc_out.Format = GetDXGIUnormFormat(key);
  srv_desc_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc_out.Shader4ComponentMapping =
      GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key)) |
      D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
  srv_desc_out.Texture2D.MostDetailedMip = 0;
  srv_desc_out.Texture2D.MipLevels = 1;
  srv_desc_out.Texture2D.PlaneSlice = 0;
  srv_desc_out.Texture2D.ResourceMinLODClamp = 0.0f;
  // Only texture->key, not the result of BindingInfoFromFetchConstant, contains
  // whether the texture is scaled.
  key = texture->key();
  if (width_unscaled_out) {
    *width_unscaled_out = key.GetWidth();
  }
  if (height_unscaled_out) {
    *height_unscaled_out = key.GetHeight();
  }
  format_out = key.format;
  return texture_resource;
}

D3D12TextureCache::D3D12Texture::D3D12Texture(D3D12TextureCache& texture_cache,
                                              const TextureKey& key, ID3D12Resource* resource,
                                              D3D12_RESOURCE_STATES resource_state,
                                              bool track_usage)
    : Texture(texture_cache, key, track_usage),
      resource_(resource),
      resource_state_(resource_state) {
  ID3D12Device* device = texture_cache.command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC resource_desc = resource_->GetDesc();
  SetHostMemoryUsage(device->GetResourceAllocationInfo(0, 1, &resource_desc).SizeInBytes);
}

D3D12TextureCache::D3D12Texture::~D3D12Texture() {
  {
    // [texpack] An upload still waiting for its frame must not find a dead
    // texture (g_texpack_pending holds raw pointers).
    std::lock_guard<std::mutex> lock(g_texpack_pending_mutex);
    g_texpack_pending.erase(std::remove(g_texpack_pending.begin(), g_texpack_pending.end(),
                                        static_cast<void*>(this)),
                            g_texpack_pending.end());
  }
  auto& d3d12_texture_cache = static_cast<D3D12TextureCache&>(texture_cache());
  for (const auto& descriptor_pair : srv_descriptors_) {
    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);
  }
}

uint32_t D3D12TextureCache::SRVGenerationOf(const TextureBinding* binding) {
  if (!binding || !binding->texture) return 0;
  const D3D12Texture* t = static_cast<const D3D12Texture*>(binding->texture);
  return uint32_t(reinterpret_cast<uintptr_t>(t) >> 4) * 2654435761u + t->srv_generation() * 40503u +
         1u;
}

void D3D12TextureCache::ReleaseRetiredDescriptors(uint64_t completed_submission) {
  for (auto it = texpack_retired_descriptors_.begin(); it != texpack_retired_descriptors_.end();) {
    if (it->first <= completed_submission) {
      ReleaseTextureDescriptor(it->second);
      it = texpack_retired_descriptors_.erase(it);
    } else {
      ++it;
    }
  }
}

void D3D12TextureCache::D3D12Texture::ClearSRVDescriptors() {
  auto& cache = static_cast<D3D12TextureCache&>(texture_cache());
  const uint64_t submission = cache.command_processor_.GetCurrentSubmission();
  for (const auto& p : srv_descriptors_)
    cache.texpack_retired_descriptors_.emplace_back(submission, p.second);
  srv_descriptors_.clear();
  ++srv_generation_;
}

bool D3D12TextureCache::IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width,
                                              uint32_t height) const {
  DXGI_FORMAT dxgi_format_uncompressed = host_formats_[uint32_t(format)].dxgi_format_uncompressed;
  if (dxgi_format_uncompressed == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(format);
  if (!(width & (format_info->block_width - 1)) && !(height & (format_info->block_height - 1))) {
    return false;
  }
  // UnalignedBlockTexturesSupported is for block-compressed textures with the
  // block size of 4x4, but not for 2x1 (4:2:2) subsampled formats.
  if (format_info->block_width == 4 && format_info->block_height == 4 &&
      command_processor_.GetD3D12Provider().AreUnalignedBlockTexturesSupported()) {
    return false;
  }
  return true;
}

TextureCache::LoadShaderIndex D3D12TextureCache::GetLoadShaderIndex(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  if (key.signed_separate) {
    return host_format.load_shader_signed;
  }
  if (IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
    return host_format.load_shader_decompress;
  }
  return host_format.load_shader;
}

bool D3D12TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormat& host_format = host_formats_[uint32_t(key.format)];
  return host_format.load_shader_signed != kLoadShaderIndexUnknown &&
         host_format.load_shader_signed != host_format.load_shader;
}

bool D3D12TextureCache::IsScaledResolveSupportedForFormat(TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndex(key);
  return load_shader != kLoadShaderIndexUnknown && load_pipelines_scaled_[load_shader] != nullptr;
}

uint32_t D3D12TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  // Dense cache-line-aligned swizzle array avoids cache misses from accessing
  // the full HostFormat struct on every texture fetch.
  alignas(64) static const auto swizzle_cache = []() {
    std::array<uint16_t, 64> arr{};
    for (int i = 0; i < 64; ++i) {
      arr[i] = static_cast<uint16_t>(host_formats_[i].swizzle);
    }
    return arr;
  }();
  return swizzle_cache[uint32_t(key.format)];
}

uint32_t D3D12TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURECUBE_DIMENSION;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t D3D12TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

std::unique_ptr<TextureCache::Texture> D3D12TextureCache::CreateTexture(TextureKey key) {
  D3D12_RESOURCE_DESC desc;
  desc.Format = GetDXGIResourceFormat(key);
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[uint32_t(key.format)] |= kUnsupportedResourceBit;
    return nullptr;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  } else {
    // 1D textures are treated as 2D for simplicity.
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  }
  desc.Alignment = 0;
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  if (key.scaled_resolve) {
    desc.Width *= draw_resolution_scale_x();
    desc.Height *= draw_resolution_scale_y();
  }
  desc.DepthOrArraySize = key.GetDepthOrArraySize();
  desc.MipLevels = key.mip_max_level + 1;

  // [probe] Does resolution scaling actually reach this title's shadow maps?
  //
  // Set NG2_SHADOW_PROBE to find out. A 360 shadow map is normally rendered to
  // EDRAM and RESOLVED to a texture, and only resolved textures are enlarged by
  // draw_resolution_scale (just above). An engine that renders shadows straight
  // to a texture gets nothing from the setting, so "supersampling improves
  // shadows" is a claim about THIS game, not about the SDK, and it has to be
  // measured rather than reasoned about.
  if (std::getenv("NG2_SHADOW_PROBE")) {
    static std::mutex probe_mutex;
    static std::map<std::string, int> seen;
    static int creations = 0;
    char line[96];
    std::snprintf(line, sizeof(line), "%s %ux%u fmt=%u",
                  key.scaled_resolve ? "SCALED  " : "unscaled",
                  key.GetWidth(), key.GetHeight(), uint32_t(key.format));
    std::lock_guard<std::mutex> lock(probe_mutex);
    seen[line]++;
    if (++creations % 250 == 0) {
      int scaled = 0, total = 0;
      for (const auto& kv : seen) {
        total += kv.second;
        if (kv.first.rfind("SCALED", 0) == 0)
          scaled += kv.second;
      }
      REXLOG_INFO("[probe] {} creations, {} of them SCALED resolves, {} distinct shapes",
                  total, scaled, seen.size());
      for (const auto& kv : seen) {
        if (kv.first.rfind("SCALED", 0) == 0)
          REXLOG_INFO("[probe]   {}  x{}", kv.first, kv.second);
      }
    }
  }
  // [texpack] A replacement is a different SIZE, so the resource has to be
  // created for it - the guest dimensions are not a container it can be poured
  // into. One mip: the pack has no mip chain, and a sampler asking for level 3
  // of a texture that has one level reads nothing.
  // The content hash is taken HERE, at creation, from the bytes the guest has in
  // memory right now; the load and view lookups of this texture reuse it.
  const uint8_t* texpack_guest = shared_memory().memory().TranslatePhysical<const uint8_t*>(
      uint32_t(key.base_page) << 12);
  // Copied, not pointed at: the entry lives in a map the next pack switch
  // clears, and the decision has to outlive that on the texture (see below).
  TexturePackFile replacement;
  bool replaced = false;
  if (REXCVAR_GET(texture_pack_resolve_at_load)) {
    // Resolve-at-load: the resource stays GUEST-sized (always decoded and
    // visible); a separate 4x resource is built at load by ApplyTexpackResolve
    // and the view samples it. Nothing to size here.
  } else if (const TexturePackFile* found =
                 shared_memory().AnyPageGpuWritten(
                     uint32_t(key.base_page) << 12,
                     key.GetGuestLayout().base.level_data_extent_bytes)
                     ? nullptr   // a render target's memory: never art
                     : TexturePackLookup(key, texpack_guest,
                                         key.GetGuestLayout().base.level_data_extent_bytes,
                                         true)) {
    replacement = *found;
    replaced = true;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Width = replacement.width;
    desc.Height = replacement.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
  }
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  // Untiling through a buffer instead of using unordered access because copying
  // is not done that often.
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // Assuming untiling will be the next operation.
  D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COPY_DEST;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             resource_state, nullptr, IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  auto* texture = new D3D12Texture(*this, key, resource.Get(), resource_state);
  // [texpack] The decision is taken here, once, and travels with the resource
  // it sized. The load and the view read it from the texture; a lookup of
  // their own could answer differently after a pack switch, for a resource
  // that cannot change with it.
  if (replaced) {
    texture->SetTexpackReplacement(replacement.width, replacement.height, replacement.path);
  }
  return std::unique_ptr<Texture>(texture);
}

// [texpack] Build, refresh or drop a texture's separate upscaled resource from
// the bytes in memory NOW. The guest resource is untouched, so this never makes
// anything disappear: if the content is not in the pack, the view falls back to
// the guest resource. Re-checked every load, so a reused (streamed) address that
// changes content stays correct.
// FABLE2_TEXPACK_TRACE=1: one line per resolve-at-load decision, capped.
static bool TexpackTraceOn() {
  static const bool on = [] {
    const char* v = std::getenv("FABLE2_TEXPACK_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}
static void TexpackTrace(const char* what, uint64_t id, uint32_t hash, uint32_t w, uint32_t h,
                         uint64_t file_id, uint32_t prev_hash) {
  static std::atomic<uint32_t> lines{0};
  if (!TexpackTraceOn()) return;
  if (what[0] == 'n') {   // "none": once per id, or the loading screen fills the cap
    static std::mutex m;
    static std::set<uint64_t> said;
    std::lock_guard<std::mutex> lock(m);
    if (!said.insert(id).second) return;
  }
  if (++lines > 40000) return;
  REXLOG_INFO("[texpack-trace] {:016X} {}x{} hash {:08X} prev {:08X} -> {} {:016X}", id, w, h, hash,
              prev_hash, what, file_id);
}

// [texpack] Mip chain for a replacement. The pack files hold one level; a 2x
// texture sampled at distance with no smaller levels aliases - far grass
// looked like a television with a bad signal (2026-09-14). A 2x2 box compute
// pass per level, recorded right after the level-0 upload.
bool D3D12TextureCache::TexpackMipInit() {
  if (texpack_mip_init_tried_) return texpack_mip_pipeline_ != nullptr;
  texpack_mip_init_tried_ = true;
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.RegisterSpace = 0;
  params[0].Constants.Num32BitValues = 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE src_range = {};
  src_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  src_range.NumDescriptors = 1;
  src_range.BaseShaderRegister = 0;
  src_range.RegisterSpace = 0;
  src_range.OffsetInDescriptorsFromTableStart = 0;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &src_range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE dst_range = {};
  dst_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  dst_range.NumDescriptors = 1;
  dst_range.BaseShaderRegister = 0;
  dst_range.RegisterSpace = 0;
  dst_range.OffsetInDescriptorsFromTableStart = 0;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &dst_range;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
  rs_desc.NumParameters = 3;
  rs_desc.pParameters = params;
  rs_desc.NumStaticSamplers = 0;
  rs_desc.pStaticSamplers = nullptr;
  rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(texpack_mip_root_signature_.ReleaseAndGetAddressOf()) =
      ui::ngpu_d3d12::util::CreateRootSignature(provider, rs_desc);
  if (!texpack_mip_root_signature_) {
    REXLOG_ERROR("[texpack] mip root signature failed; replacements get no mip chain");
    return false;
  }
  *(texpack_mip_pipeline_.ReleaseAndGetAddressOf()) = ui::ngpu_d3d12::util::CreateComputePipeline(
      device, texpack_shaders::texpack_mip_cs, sizeof(texpack_shaders::texpack_mip_cs),
      texpack_mip_root_signature_.Get());
  if (!texpack_mip_pipeline_) {
    REXLOG_ERROR("[texpack] mip pipeline failed; replacements get no mip chain");
    return false;
  }
  REXLOG_INFO("[texpack] mip chains for replacements: compute pass ready");
  return true;
}

// The resource arrives with every level in COPY_DEST and level 0 just
// copied; every level is left in PIXEL|NON_PIXEL_SHADER_RESOURCE whatever
// happens (a failure leaves the smaller levels unwritten, and is logged).
bool D3D12TextureCache::TexpackGenerateMips(ID3D12Resource* res, uint32_t w, uint32_t h,
                                            uint32_t levels) {
  const D3D12_RESOURCE_STATES kSampled = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair descs[2 * 16];
  const uint32_t passes = levels > 1 ? std::min<uint32_t>(levels - 1, 16) : 0;
  const bool ok = passes > 0 && TexpackMipInit() &&
                  command_processor_.RequestOneUseSingleViewDescriptors(2 * passes, descs);
  if (!ok) {
    static std::atomic<uint32_t> n{0};
    if (passes > 0 && ++n <= 3)
      REXLOG_WARN("[texpack] no mip chain for a {}x{} replacement (descriptors or pipeline)",
                  w, h);
    command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_COPY_DEST, kSampled);
    command_processor_.SubmitBarriers();
    return false;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  DeferredCommandList& cl = command_processor_.GetDeferredCommandList();
  command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
  for (uint32_t i = 1; i <= passes; ++i)
    command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, i);
  command_processor_.SubmitBarriers();
  cl.D3DSetComputeRootSignature(texpack_mip_root_signature_.Get());
  command_processor_.SetExternalPipeline(texpack_mip_pipeline_.Get());
  uint32_t sw = w, sh = h;
  for (uint32_t i = 1; i <= passes; ++i) {
    const uint32_t dw = std::max<uint32_t>(sw / 2, 1), dh = std::max<uint32_t>(sh / 2, 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = i - 1;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(res, &srv, descs[2 * (i - 1)].first);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = i;
    device->CreateUnorderedAccessView(res, nullptr, &uav, descs[2 * (i - 1) + 1].first);
    const uint32_t constants[4] = {dw, dh, sw - 1, sh - 1};
    cl.D3DSetComputeRoot32BitConstants(0, 4, constants, 0);
    cl.D3DSetComputeRootDescriptorTable(1, descs[2 * (i - 1)].second);
    cl.D3DSetComputeRootDescriptorTable(2, descs[2 * (i - 1) + 1].second);
    cl.D3DDispatch((dw + 7) / 8, (dh + 7) / 8, 1);
    command_processor_.PushUAVBarrier(res);
    command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, i);
    command_processor_.SubmitBarriers();
    sw = dw;
    sh = dh;
  }
  for (uint32_t i = passes + 1; i < levels; ++i)  // beyond 16 passes: never in practice
    command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_COPY_DEST, kSampled, i);
  for (uint32_t i = 0; i <= passes; ++i)
    command_processor_.PushTransitionBarrier(res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             kSampled, i);
  command_processor_.SubmitBarriers();
  return true;
}

static double TexpackNowSeconds() {
  // NATIVE PATCH (2026-09-26, PROFT4): called for every bound pack texture on every draw; the tick count (~16 ms
  // resolution) is plenty for a half-second re-verify interval and costs no QueryPerformanceCounter.
  return double(GetTickCount64()) * 0.001;
}

// The bytes under a replaced texture, re-checked as it is bound. The pack is
// resolved from the memory at load time; a texture whose memory is rewritten
// without the plugin seeing a reload (a write it did not catch, a GPU copy it
// could not see) kept the old picture for as long as it lived - the Oakfield
// ground and sky wearing the market's textures (2026-09-14). Eight samples
// every half second cost nothing measurable and bound that to half a second.
void D3D12TextureCache::TexpackReverify(D3D12Texture& texture) {
  if (!texture.texpack_resource()) return;
  const double now = TexpackNowSeconds();
  if (now - texture.texpack_verified_at() < 0.5) return;
  texture.SetTexpackVerifiedAt(now);
  const uint8_t* guest = shared_memory().memory().TranslatePhysical<const uint8_t*>(
      uint32_t(texture.key().base_page) << 12);
  const uint32_t size = texture.GetGuestBaseSize();
  if (!guest || !size) return;
  if (texture.TexpackSamplesMatch(guest, size)) return;
  static std::atomic<uint32_t> n{0};
  const uint32_t k = ++n;
  if (k <= 20 || k % 100 == 0)
    REXLOG_INFO("[texpack] re-verify: the memory under {:016X} ({}x{}) changed without a reload "
                "({} so far); reloading it", TexturePackId(texture.key()), texture.key().GetWidth(),
                texture.key().GetHeight(), k);
  // The path a CPU write takes: the pages are re-uploaded from guest memory
  // and the watch fires (base outdated, handle released) - nothing dangles.
  shared_memory().MemoryInvalidationCallback(uint32_t(texture.key().base_page) << 12, size, true);
}

void D3D12TextureCache::ApplyTexpackResolve(D3D12Texture& texture, const TextureKey& key) {
  const std::string dir = rex::cvar::Query<std::string>("texture_pack_path");
  auto retire_current = [&]() {
    if (texture.texpack_resource()) {
      g_texpack_uploads.emplace_back(command_processor_.GetCurrentSubmission(),
                                     texture.DetachTexpackResource());
      texture.ClearSRVDescriptors();
    }
  };
  if (dir.empty() || !TexturePackIsArtFormat(key.format) ||
      key.dimension != xenos::DataDimension::k2DOrStacked ||
      key.GetDepthOrArraySize() != 1 || key.base_page == 0) {
    retire_current();
    return;
  }
  const uint8_t* guest = shared_memory().memory().TranslatePhysical<const uint8_t*>(
      uint32_t(key.base_page) << 12);
  const uint32_t gsize = texture.GetGuestBaseSize();
  if (!guest || !gsize) return;
  // A resolve destination: the bytes are a render target's, never art, and
  // they change every frame - hashing them served the previous occupant of
  // the memory (the menu spinner across the Oakfield sky, 2026-09-13).
  if (shared_memory().AnyPageGpuWritten(uint32_t(key.base_page) << 12, gsize)) {
    static std::atomic<uint32_t> gpu_written_skipped{0};
    const uint32_t n = ++gpu_written_skipped;
    if (n == 1 || n % 1000 == 0)
      REXLOG_INFO("[texpack] {} texture loads left alone: memory written by the GPU (render "
                  "targets, not art)", n);
    if (texture.texpack_resource())
      TexpackTrace("retire:gpu-written", TexturePackId(key), 0, key.GetWidth(), key.GetHeight(), 0,
                   texture.texpack_content_hash());
    retire_current();
    return;
  }
  const uint32_t hash = TexturePackContentHash(guest, gsize);
  if (texture.texpack_resource() && texture.texpack_content_hash() == hash) return;
  const uint32_t trace_prev = texture.texpack_resource() ? texture.texpack_content_hash() : 0;
  // Match against the in-memory index first: the tens of thousands of per-stage
  // loads whose content is not in the pack never touch the disk.
  const uint64_t id = TexturePackId(key);
  uint64_t file_id = 0;
  {
    std::lock_guard<std::mutex> lock(g_texpack_mutex);
    TexturePackIndexBuild(dir);
    if (auto bh = g_texpack_by_hash.find(hash); bh != g_texpack_by_hash.end()) {
      const uint64_t shape = TexturePackShape(id);
      for (uint64_t fid : bh->second) {
        if (TexturePackShape(fid) == shape) {
          file_id = fid;
          break;
        }
      }
    }
    if (file_id)
      g_texpack_file_of[id] = {file_id, hash};
  }
  if (!file_id) {
    TexpackTrace(texture.texpack_resource() ? "retire:no-file" : "none", id, hash, key.GetWidth(),
                 key.GetHeight(), 0, trace_prev);
    retire_current();
    return;
  }
  TexpackTrace(trace_prev ? "replace:changed" : "replace", id, hash, key.GetWidth(), key.GetHeight(),
               file_id, trace_prev);
  char p[600];
  std::snprintf(p, sizeof(p), "%s/%016llX-%08X.tex", dir.c_str(),
                (unsigned long long)file_id, hash);
  uint32_t w = 0, h = 0;
  {
    std::ifstream hf(p, std::ios::binary);
    uint8_t head[16] = {};
    if (hf && hf.read(reinterpret_cast<char*>(head), sizeof(head)) &&
        !std::memcmp(head, "NG2T", 4)) {
      uint32_t ver = 0;
      std::memcpy(&ver, head + 4, 4);
      std::memcpy(&w, head + 8, 4);
      std::memcpy(&h, head + 12, 4);
      if (ver != 1) { w = 0; h = 0; }
    }
  }
  if (!w || !h) { retire_current(); return; }
  // Per-frame upload budget (g_texpack_pending). The estimate is the pixel
  // bytes; the placed footprint only adds row padding. Draining skips the
  // check (the drain loop stops on its own when the budget is spent).
  {
    const int64_t budget_mb = REXCVAR_GET(texture_pack_upload_budget_mb);
    const int64_t need = int64_t(w) * int64_t(h) * 4;
    if (budget_mb > 0 && !g_texpack_draining && g_texpack_budget_left < need) {
      retire_current();  // the guest data shows - right content, low res - until then
      {
        std::lock_guard<std::mutex> lock(g_texpack_pending_mutex);
        void* const self = static_cast<void*>(&texture);
        if (std::find(g_texpack_pending.begin(), g_texpack_pending.end(), self) ==
            g_texpack_pending.end())
          g_texpack_pending.push_back(self);
      }
      static std::atomic<uint32_t> deferred{0};
      const uint32_t n = ++deferred;
      if (n == 1 || n % 500 == 0)
        REXLOG_INFO("[texpack] {} uploads held for a later frame by the {} MB/frame budget", n,
                    budget_mb);
      return;
    }
    g_texpack_budget_left -= need;
  }
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  g_texpack_uploads.erase(
      std::remove_if(g_texpack_uploads.begin(), g_texpack_uploads.end(),
                     [completed](const auto& e) { return e.first <= completed; }),
      g_texpack_uploads.end());
  ReleaseRetiredDescriptors(completed);
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC rdesc = {};
  rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rdesc.Width = w;
  rdesc.Height = h;
  rdesc.DepthOrArraySize = 1;
  // The full chain: a 2x texture with one level aliases at distance.
  uint32_t texpack_levels = 1;
  while ((std::max<uint32_t>(w, h) >> texpack_levels) >= 1u) ++texpack_levels;
  rdesc.MipLevels = UINT16(texpack_levels);
  rdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  rdesc.SampleDesc.Count = 1;
  rdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rdesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  Microsoft::WRL::ComPtr<ID3D12Resource> res;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &rdesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&res)))) {
    retire_current();
    return;
  }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT rows = 0;
  UINT64 rowbytes = 0, upsize = 0;
  device->GetCopyableFootprints(&rdesc, 0, 1, 0, &fp, &rows, &rowbytes, &upsize);
  D3D12_RESOURCE_DESC updesc = {};
  updesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  updesc.Width = upsize;
  updesc.Height = 1;
  updesc.DepthOrArraySize = 1;
  updesc.MipLevels = 1;
  updesc.Format = DXGI_FORMAT_UNKNOWN;
  updesc.SampleDesc.Count = 1;
  updesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Microsoft::WRL::ComPtr<ID3D12Resource> upload;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesUpload,
                                             D3D12_HEAP_FLAG_NONE, &updesc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload)))) {
    retire_current();
    return;
  }
  void* mapped = nullptr;
  if (FAILED(upload->Map(0, nullptr, &mapped))) { retire_current(); return; }
  bool ok = false;
  {
    std::ifstream f(p, std::ios::binary);
    if (f) {
      f.seekg(16);
      uint8_t* out = static_cast<uint8_t*>(mapped) + fp.Offset;
      const size_t srcpitch = size_t(w) * 4;
      if (fp.Footprint.RowPitch == srcpitch) {
        ok = bool(f.read(reinterpret_cast<char*>(out), std::streamsize(srcpitch * rows)));
      } else {
        ok = true;
        for (UINT y = 0; y < rows && ok; ++y)
          ok = bool(f.read(reinterpret_cast<char*>(out + size_t(y) * fp.Footprint.RowPitch),
                           std::streamsize(srcpitch)));
      }
    }
  }
  upload->Unmap(0, nullptr);
  if (!ok) { retire_current(); return; }
  DeferredCommandList& cl = command_processor_.GetDeferredCommandList();
  D3D12_TEXTURE_COPY_LOCATION srcloc = {}, dstloc = {};
  srcloc.pResource = upload.Get();
  srcloc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcloc.PlacedFootprint = fp;
  dstloc.pResource = res.Get();
  dstloc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dstloc.SubresourceIndex = 0;
  cl.D3DCopyTextureRegion(&dstloc, 0, 0, 0, &srcloc, nullptr);
  TexpackGenerateMips(res.Get(), w, h, texpack_levels);
  retire_current();  // drop the previous variant's resource, if any
  g_texpack_uploads.emplace_back(command_processor_.GetCurrentSubmission(), std::move(upload));
  texture.SetTexpackResource(res, hash);
  texture.SetTexpackSamples(guest, gsize, TexpackNowSeconds());
  texture.ClearSRVDescriptors();
  // Which stage used this file, for the per-region warming. StageNote lived
  // only in the upload path, and once resolve-at-load carried the pack that
  // path served nothing, so no stage list was ever written and the warming
  // bar never appeared (2026-09-12). The file served is what gets listed.
  StageNote(dir, REXCVAR_GET(texture_pack_chapter), file_id, hash);
  {
    static std::atomic<uint32_t> built{0};
    const uint32_t n = ++built;
    if (n == 1 || n % 1000 == 0)
      REXLOG_INFO("[texpack] {} upscaled textures resolved at load", n);
  }
}

uint64_t g_prof_tex_loads = 0, g_prof_tex_load_bytes = 0, g_prof_tex_loads_scaled = 0;   // NATIVE PATCH: [gpu prof]

bool D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  D3D12CommandProcessor::GpuCatScope gpu_cat(command_processor_, D3D12CommandProcessor::kGpuCatTexLoad);   // NATIVE PATCH
  {
    extern uint64_t g_prof_tex_loads, g_prof_tex_load_bytes, g_prof_tex_loads_scaled;
    ++g_prof_tex_loads;
    g_prof_tex_load_bytes += uint64_t(load_base ? texture.GetGuestBaseSize() : 0u) + uint64_t(load_mips ? texture.GetGuestMipsSize() : 0u);
    if (texture.key().scaled_resolve) ++g_prof_tex_loads_scaled;
  }
  command_processor_.NoteTextureLoad(  // [hitch]
      uint64_t(load_base ? texture.GetGuestBaseSize() : 0u) +
      uint64_t(load_mips ? texture.GetGuestMipsSize() : 0u));
  // [split] Loading from memory the GPU resolved in the still-open submission:
  // end that submission first so the resolve is complete and visible before the
  // load reads it (the streaming flash), instead of a boundary after every
  // resolve. Nothing of this load has been recorded yet, so the new submission
  // gets all of it; the draw re-binds its render targets afterwards.
  if ((load_base && command_processor_.RangeResolvedInOpenSubmission(
                        uint32_t(texture.key().base_page) << 12, texture.GetGuestBaseSize())) ||
      (load_mips && command_processor_.RangeResolvedInOpenSubmission(
                        uint32_t(texture.key().mip_page) << 12, texture.GetGuestMipsSize()))) {
    command_processor_.SplitSubmissionForFreshResolve();
  }
  // [readback] Any texture whose guest range still holds an unlanded resolve
  // copy would upload stale/empty bytes (the impostor flash). Land the copies
  // that have already FINISHED (a cheap memcpy) for base and mips. Never defer
  // the upload: the swap texture and other same-frame resolve targets are
  // resolved in the still-open submission every frame, and skipping their
  // upload black-screens the present (RequestSwapTexture failed, 2026-09-14).
  if (load_base) {
    command_processor_.LandImpostorReadbackBeforeUpload(
        uint32_t(texture.key().base_page) << 12, texture.GetGuestBaseSize());
  }
  if (load_mips) {
    command_processor_.LandImpostorReadbackBeforeUpload(
        uint32_t(texture.key().mip_page) << 12, texture.GetGuestMipsSize());
  }
  // [diag] A reload frame (>= 40 texture loads): name up to 12 of its
  // textures, for the first 10 such frames. The black distant-ridge flash of
  // 2026-09-14 lands on exactly these frames.
  {
    static uint32_t diag_frames = 0;
    static uint32_t diag_last_count = 0;
    static uint32_t diag_in_frame = 0;
    const uint32_t loads = command_processor_.FrameTextureLoads();
    if (loads < diag_last_count) diag_in_frame = 0;  // a new frame began
    diag_last_count = loads;
    if (loads >= 40 && diag_frames < 10) {
      if (loads == 40) ++diag_frames;
      if (diag_in_frame < 12) {
        ++diag_in_frame;
        const TextureKey& k = texture.key();
        const uint32_t base = uint32_t(k.base_page) << 12;
        const uint32_t size = texture.GetGuestBaseSize();
        REXLOG_INFO("[diag] reload frame #{} texture {:08X}+{} {}x{} fmt {} mips {}{}{}",
                    diag_frames, base, size, uint32_t(k.width_minus_1) + 1,
                    uint32_t(k.height_minus_1) + 1, uint32_t(k.format), uint32_t(k.mip_max_level),
                    shared_memory().AnyPageGpuWritten(base, size) ? " GPU-written" : "",
                    command_processor_.HasPendingResolveReadback(base, size) ? " READBACK-PENDING" : "");
      }
    }
  }
  // [texpack][diag] Report what this path actually sees, once. The tuning file
  // showed the cvars arriving but nothing was written, and from outside there
  // is no way to tell "cvar still false here" from "fopen failed".
  {
    static bool reported = false;
    if (!reported) {
      reported = true;
      REXLOG_INFO("[texpack] first texture load: dump={} path='{}'",
                  REXCVAR_GET(texture_dump) ? 1 : 0,
                  rex::cvar::Query<std::string>("texture_dump_path"));
    }
  }

  // [texpack] Dump every unique guest texture, for offline upscaling.
  //
  // The GUEST bytes are what get written, not the finished host texture:
  // conversion happens in a GPU compute shader, so host-format pixels never
  // exist on the CPU and reading them back would mean a fence and a stall per
  // texture. Untiling is a pure function of the key, which is dumped beside
  // the bytes, so an offline tool reproduces it exactly and this costs one
  // memcpy per texture the first time it is seen.
  if (REXCVAR_GET(texture_dump) &&
      !rex::cvar::Query<std::string>("texture_dump_path").empty() &&
      TexturePackIsArtFormat(texture.key().format)) {
    const TextureKey& tk = texture.key();
    const uint32_t tw = uint32_t(tk.width_minus_1) + 1;
    const uint32_t th = uint32_t(tk.height_minus_1) + 1;
    const uint32_t tsize = texture.GetGuestBaseSize();
    // The id keys everything that changes the LAYOUT; the content hash keys
    // the pixels. Both, so a second texture that lands at an address the game
    // reused is dumped as its own file instead of being lost behind the first.
    const uint64_t id = TexturePackId(tk);
    // (shape, content hash) of every raw dump on disk plus this session's -
    // the pack's own identity, address-free. Seeded from the folder the
    // first time a folder is dumped to. Before this the key carried the
    // address and the set was per session: every session re-dumped what it
    // loaded, and a texture met at a new address became a new file - an
    // hour of play at 400% draw distance wrote 101,493 files, 67 GB, and
    // filled the disk (2026-09-13).
    static std::set<std::pair<uint64_t, uint32_t>> dumped;
    static std::string dumped_dir;
    static uint32_t dumped_this_session = 0;
    static std::mutex dump_mutex;
    const uint8_t* src = shared_memory().memory().TranslatePhysical<const uint8_t*>(
        uint32_t(tk.base_page) << 12);
    if (src && tsize && tsize < (64u << 20) &&
        !shared_memory().AnyPageGpuWritten(uint32_t(tk.base_page) << 12, tsize)) {
      const uint32_t hash = TexturePackContentHash(src, tsize);
      const std::string dir = rex::cvar::Query<std::string>("texture_dump_path");
      bool fresh = false;
      // Only a snapshot whose bytes hash to the name is written (below): the
      // live memory may be mid-stream, and a file named by one moment's hash
      // holding another moment's bytes poisons the pack for that hash.
      std::vector<uint8_t> snap;
      {
        std::lock_guard<std::mutex> lock(dump_mutex);
        if (dir != dumped_dir) {
          dumped.clear();
          dumped_dir = dir;
          dumped_this_session = 0;
          std::error_code ec;
          for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            const std::string n = e.path().filename().string();
            unsigned long long fid = 0;
            unsigned fh = 0;
            if (n.size() == 33 && std::sscanf(n.c_str(), "tex_%16llx-%8x.bin", &fid, &fh) == 2)
              dumped.insert({TexturePackShape(uint64_t(fid)), uint32_t(fh)});
          }
          REXLOG_INFO("[texpack] dump folder '{}' already holds {} distinct contents; "
                      "those are not written again",
                      dir, dumped.size());
        }
        // 4,000 a session (~2 GB): a 20,000 cap let one run write 11 GB.
        fresh = dumped_this_session < 4000 &&
                dumped.insert({TexturePackShape(id), hash}).second;
        if (fresh) ++dumped_this_session;
      }
      if (fresh) {
        snap.assign(src, src + tsize);
        if (TexturePackContentHash(snap.data(), tsize) != hash ||
            TexturePackContentHash(src, tsize) != hash) {
          // Torn: the memory changed under the copy. Forget it so the settled
          // content is dumped on a later load.
          std::lock_guard<std::mutex> lock(dump_mutex);
          dumped.erase({TexturePackShape(id), hash});
          --dumped_this_session;
          static std::atomic<uint32_t> torn{0};
          const uint32_t n = ++torn;
          if (n <= 10 || n % 100 == 0)
            REXLOG_INFO("[texpack] dump skipped: memory under {:016X} changed while it was "
                        "copied ({} so far)", id, n);
          fresh = false;
        }
      }
      if (fresh) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/tex_%016llX-%08X.bin",
                      dir.c_str(), (unsigned long long)id, hash);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(snap.data(), 1, tsize, f);
          std::fclose(f);
          // One index line per texture: everything the decoder needs, and the
          // hash last so a reader of the old nine-column lines still works.
          std::snprintf(path, sizeof(path), "%s/index.txt", dir.c_str());
          if (FILE* ix = std::fopen(path, "ab")) {
            std::fprintf(ix,
                         "%016llX %u %u %u %u %u %u %u %u %08X",
                         (unsigned long long)id, tw, th, uint32_t(tk.format),
                         uint32_t(tk.tiled), uint32_t(tk.pitch),
                         uint32_t(tk.endianness), uint32_t(tk.dimension), tsize, hash);
            std::fputc(10, ix);
            std::fclose(ix);
          }
        }
      }
    }
  }

  D3D12Texture& d3d12_texture = static_cast<D3D12Texture&>(texture);
  TextureKey texture_key = d3d12_texture.key();

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  // [texpack] A replacement supersedes the guest data entirely.
  //
  // The normal path runs a compute shader that reads guest memory through the
  // shared-memory buffer and writes host-format pixels. A pack texture is
  // already host-format and a different size, so none of that applies: decode
  // the PNG, copy it straight in, and skip the shader. Returning true here is
  // what tells the cache the texture is resident.
  // The decision the resource was created with, NOT a fresh lookup - see
  // D3D12Texture::SetTexpackReplacement. If the pack was switched since, this
  // texture still uploads the file it was sized for; the switch drops every
  // texture at the end of the frame and the recreated one decides afresh.
  if (load_base && REXCVAR_GET(texture_pack_resolve_at_load)) {
    // Resolve-at-load: build/refresh the separate upscaled resource, then fall
    // through to the normal decode which fills the guest resource as usual.
    ApplyTexpackResolve(d3d12_texture, texture_key);
  }
  TexturePackFile texpack_replacement{d3d12_texture.texpack_width(),
                                      d3d12_texture.texpack_height(),
                                      d3d12_texture.texpack_path()};
  if (TexturePackFile* replacement =
          d3d12_texture.texpack_replaced() ? &texpack_replacement : nullptr) {
    if (!load_base) {
      return true;  // mips: the replacement has only a base level
    }
    if (replacement->path.empty()) {
      // Resolve-at-load: choose the pack file by the bytes in memory NOW -
      // the same bytes and CRC the dump writes at this same point, so the
      // file the dump produced is exactly what this opens.
      const std::string rdir = rex::cvar::Query<std::string>("texture_pack_path");
      const uint8_t* rsrc = shared_memory().memory().TranslatePhysical<const uint8_t*>(
          uint32_t(texture_key.base_page) << 12);
      const uint32_t rsize = d3d12_texture.GetGuestBaseSize();
      bool resolved = false;
      if (!rdir.empty() && rsrc && rsize) {
        const uint32_t rhash = TexturePackContentHash(rsrc, rsize);
        char rp[600];
        std::snprintf(rp, sizeof(rp), "%s/%016llX-%08X.tex", rdir.c_str(),
                      (unsigned long long)TexturePackId(texture_key), rhash);
        std::error_code rec;
        if (std::filesystem::exists(rp, rec)) {
          replacement->path = rp;
          resolved = true;
        }
      }
      if (!resolved) {
        static std::atomic<int> logged{0};
        if (logged++ < 10)
          REXLOG_INFO("[texpack] resolve-at-load: no pack file for the content at "
                      "{:016X} this load", (unsigned long long)TexturePackId(texture_key));
        return false;
      }
    }
    if (d3d12_texture.texpack_uploaded()) {
      // Already in the resource. The game wrote the guest copy (that is why
      // the cache asked again); the replacement does not come from there.
      static std::atomic<uint32_t> skipped{0};
      const uint32_t s = ++skipped;
      if (s == 1 || s % 1000 == 0) {
        REXLOG_INFO("[texpack] {} re-loads of pack textures skipped (resource already holds them)",
                    s);
      }
      return true;
    }
    // Retire upload buffers the GPU has finished with. Freeing one while its
    // copy is still queued corrupts the texture in a way that reads exactly
    // like a decoder bug, which is an expensive thing to debug twice.
    const uint64_t completed = command_processor_.GetCompletedSubmission();
    g_texpack_uploads.erase(
        std::remove_if(g_texpack_uploads.begin(), g_texpack_uploads.end(),
                       [completed](const auto& e) { return e.first <= completed; }),
        g_texpack_uploads.end());
    ReleaseRetiredDescriptors(completed);

    const auto texpack_t0 = std::chrono::steady_clock::now();

    ID3D12Resource* dest = d3d12_texture.resource();
    const D3D12_RESOURCE_DESC dest_desc = dest->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT row_count = 0;
    UINT64 row_bytes = 0, upload_size = 0;
    device->GetCopyableFootprints(&dest_desc, 0, 1, 0, &footprint, &row_count, &row_bytes,
                                  &upload_size);
    // The read below trusts these to agree. They do by construction now (the
    // resource was created for this very replacement), and the day something
    // breaks that, this is the difference between a logged skip and a heap
    // overrun on the GPU thread.
    // The last row is NOT padded to the pitch in the footprint's size, so the
    // bound is (rows - 1) pitches plus one row of pixels - exactly what the
    // read below writes. Demanding rows * pitch rejected every width that is
    // not a multiple of 64 (432, 368, 288 wide) on the first play of 0.0.13.
    if (dest_desc.Width != replacement->width || dest_desc.Height != replacement->height ||
        footprint.Footprint.RowPitch < uint64_t(replacement->width) * 4 || row_count == 0 ||
        footprint.Offset + uint64_t(row_count - 1) * footprint.Footprint.RowPitch +
                uint64_t(replacement->width) * 4 >
            upload_size) {
      static std::atomic<int> logged{0};
      if (logged++ < 5) {
        REXGPU_ERROR("[texpack] {}: resource {}x{} does not fit the replacement {}x{} "
                     "(row pitch {}, upload {} bytes) - skipped",
                     replacement->path, dest_desc.Width, dest_desc.Height, replacement->width,
                     replacement->height, footprint.Footprint.RowPitch, upload_size);
      }
      return false;
    }

    D3D12_RESOURCE_DESC upload_desc = {};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_size;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(
            &ui::ngpu_d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) {
      REXGPU_ERROR("[texpack] could not create a {} byte upload buffer", upload_size);
      return false;
    }
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) {
      return false;
    }

    // Read the pixels STRAIGHT INTO the mapped upload buffer.
    //
    // The previous version decoded a PNG here, and measured, that cost ~16 ms
    // per texture on the render thread - 12.2 seconds across 800 textures, seen
    // as an 8-second frame while a scene streamed in. The cost scaled with
    // pixel count rather than file size, which is what identified the decode
    // rather than the I/O. There is no decode now, and no intermediate
    // allocation either: the file lands directly in GPU-visible memory.
    //
    // One read when the pitches agree, which is the common case - the row pitch
    // is aligned to 256 bytes and a width that is a multiple of 64 needs no
    // padding, so every power-of-two texture from 64 upwards takes this path.
    bool read_ok = false;
    // Split the cost, because the two halves want opposite fixes.
    //
    // "3.7 ms each" is file read AND the GPU-side work around it. Pre-loading a
    // chapter's textures can only ever remove the READ half, so building that
    // before knowing the split risks four pieces of machinery that buy nothing.
    const auto texpack_read_t0 = std::chrono::steady_clock::now();
    {
      std::ifstream f(replacement->path, std::ios::binary);
      if (f) {
        f.seekg(kTexHeaderBytes);
        uint8_t* out = static_cast<uint8_t*>(mapped) + footprint.Offset;
        const size_t src_pitch = size_t(replacement->width) * 4;
        if (footprint.Footprint.RowPitch == src_pitch) {
          read_ok = bool(f.read(reinterpret_cast<char*>(out),
                                std::streamsize(src_pitch * row_count)));
        } else {
          read_ok = true;
          for (UINT y = 0; y < row_count && read_ok; ++y) {
            read_ok = bool(f.read(
                reinterpret_cast<char*>(out + size_t(y) * footprint.Footprint.RowPitch),
                std::streamsize(src_pitch)));
          }
        }
      }
    }
    const uint64_t texpack_read_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - texpack_read_t0)
                     .count());
    upload->Unmap(0, nullptr);
    if (!read_ok) {
      REXGPU_ERROR("[texpack] short read on {}", replacement->path);
      return false;
    }

    command_processor_.PushTransitionBarrier(
        dest, d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
        D3D12_RESOURCE_STATE_COPY_DEST);
    command_processor_.SubmitBarriers();
    D3D12_TEXTURE_COPY_LOCATION src_loc = {}, dest_loc = {};
    src_loc.pResource = upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = footprint;
    dest_loc.pResource = dest;
    dest_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest_loc.SubresourceIndex = 0;
    command_list.D3DCopyTextureRegion(&dest_loc, 0, 0, 0, &src_loc, nullptr);

    g_texpack_uploads.emplace_back(command_processor_.GetCurrentSubmission(), std::move(upload));
    d3d12_texture.SetTexpackUploaded();
    // Say so. Without this there is no way to tell "the pack is working" from
    // "every lookup missed and you are looking at the original textures",
    // which look identical when the pack happens not to cover what is on
    // screen.
    // Which stage used this texture. Recorded here rather than at lookup so a
    // miss - a texture the pack does not cover - is never listed as belonging
    // to the stage; the list is what to warm, and warming a file that is not
    // there is wasted I/O.
    {
      const uint64_t stage_id = TexturePackId(texture.key());
      uint64_t file_id = stage_id;
      uint32_t file_hash = TexturePackHashFor(stage_id);
      {
        std::lock_guard<std::mutex> lock(g_texpack_mutex);
        if (auto fo = g_texpack_file_of.find(stage_id); fo != g_texpack_file_of.end()) {
          file_id = fo->second.first;
          file_hash = fo->second.second;
        }
      }
      StageNote(rex::cvar::Query<std::string>("texture_pack_path"),
                REXCVAR_GET(texture_pack_chapter), file_id, file_hash);
    }

    // Which textures are uploaded AGAIN AND AGAIN. One session showed 9300
    // uploads of a pack with 338 files - about 5 per frame at 39 fps, each
    // 1.3 ms of render-thread time - and nothing named the textures. Every
    // five seconds: the three ids uploaded most often in that window.
    {
      static std::unordered_map<uint64_t, uint32_t> uploads_by_id;
      static std::unordered_map<uint64_t, uint32_t> size_by_id;
      static auto window_start = std::chrono::steady_clock::now();
      const uint64_t id = TexturePackId(texture.key());
      ++uploads_by_id[id];
      size_by_id[id] = (replacement->width << 16) | replacement->height;
      const auto now_t = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(now_t - window_start).count() >= 5.0) {
        uint64_t top_id[3] = {0, 0, 0};
        uint32_t top_n[3] = {0, 0, 0};
        uint32_t total = 0;
        for (const auto& kv : uploads_by_id) {
          total += kv.second;
          for (int i = 0; i < 3; ++i) {
            if (kv.second > top_n[i]) {
              for (int j = 2; j > i; --j) { top_n[j] = top_n[j - 1]; top_id[j] = top_id[j - 1]; }
              top_n[i] = kv.second;
              top_id[i] = kv.first;
              break;
            }
          }
        }
        REXLOG_INFO("[texpack] re-uploads in {:.1f} s: {} uploads of {} ids; top {:016X} {}x{} x{}, "
                    "{:016X} {}x{} x{}, {:016X} {}x{} x{}",
                    std::chrono::duration<double>(now_t - window_start).count(), total,
                    uploads_by_id.size(), top_id[0], size_by_id[top_id[0]] >> 16,
                    size_by_id[top_id[0]] & 0xFFFF, top_n[0], top_id[1],
                    size_by_id[top_id[1]] >> 16, size_by_id[top_id[1]] & 0xFFFF, top_n[1],
                    top_id[2], size_by_id[top_id[2]] >> 16, size_by_id[top_id[2]] & 0xFFFF,
                    top_n[2]);
        uploads_by_id.clear();
        size_by_id.clear();
        window_start = now_t;
      }
    }

    static std::atomic<uint32_t> replaced{0};
    static std::atomic<uint64_t> total_us{0};
    static std::atomic<uint64_t> total_read_us{0};
    const uint32_t n = ++replaced;
    REXCVAR_SET(texture_pack_replaced, int32_t(n));
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - texpack_t0)
                                     .count());
    const uint64_t sum = (total_us += us);
    const uint64_t read_sum = (total_read_us += texpack_read_us);
    // This whole path runs ON THE RENDER THREAD inside a draw. If the total is
    // large, that time IS the stutter, and no amount of cache tuning will help.
    if (n == 1 || n % 100 == 0) {
      REXLOG_INFO("[texpack] {} replacements, {} ms total, {:.2f} ms each "
                  "(read {:.2f} ms = {:.0f}%, gpu {:.2f} ms) (latest {}x{})",
                  n, sum / 1000, double(sum) / 1000.0 / double(n),
                  double(read_sum) / 1000.0 / double(n),
                  sum ? 100.0 * double(read_sum) / double(sum) : 0.0,
                  double(sum - read_sum) / 1000.0 / double(n),
                  replacement->width, replacement->height);
    }
    return true;
  }

  // Past the replacement block: this texture is coming from the GAME, either
  // because the pack is off or because it does not cover this one. Counting
  // both is what lets the indicator say "1290 enhanced, 485 original" rather
  // than just asserting that the pack is on.
  {
    static std::atomic<int32_t> original{0};
    REXCVAR_SET(texture_pack_original, ++original);
  }


  // Get the pipeline.
  LoadShaderIndex load_shader = GetLoadShaderIndex(texture_key);
  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }
  bool texture_resolution_scaled = texture_key.scaled_resolve;
  ID3D12PipelineState* pipeline = texture_resolution_scaled
                                      ? load_pipelines_scaled_[load_shader].Get()
                                      : load_pipelines_[load_shader].Get();
  if (pipeline == nullptr) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  // Get the guest layout.
  const texture_util::TextureGuestLayout& guest_layout = d3d12_texture.guest_layout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || d3d12_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  assert_true(level_first <= level_last);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t texture_resolution_scale_x = texture_resolution_scaled ? draw_resolution_scale_x() : 1;
  uint32_t texture_resolution_scale_y = texture_resolution_scaled ? draw_resolution_scale_y() : 1;

  // The loop counter can mean two things depending on whether the packed mip
  // tail is stored as mip 0, because in this case, it would be ambiguous since
  // both the base and the mips would be on "level 0", but stored in separate
  // places.
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    // Packed mip tail is the level 0 - may need to load mip tails for the base,
    // the mips, or both.
    // Loop iteration 0 - base packed mip tail.
    // Loop iteration 1 - mips packed mip tail.
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    // Packed mip tail is not the level 0.
    // Loop iteration is the actual level being loaded.
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  // Get the host layout and the buffer.
  bool host_block_compressed = host_formats_[uint32_t(guest_format)].is_block_compressed &&
                               !IsDecompressionNeeded(guest_format, width, height);
  uint32_t host_block_width = host_block_compressed ? block_width : 1;
  uint32_t host_block_height = host_block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1)
                                      << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    // Decompressing guest blocks.
    host_x_blocks_per_thread *= block_width;
  }
  UINT64 copy_buffer_size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_slice_layout_base;
  UINT64 host_slice_size_base;
  // Indexing is the same as for guest stored mips:
  // 1...min(level_last, level_packed) if level_packed is not 0, or only 0 if
  // level_packed == 0.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT
  host_slice_layouts_mips[xenos::kTextureMaxMips];
  UINT64 host_slice_sizes_mips[xenos::kTextureMaxMips];
  // Using custom calculations instead of GetCopyableFootprints because
  // shaders may unconditionally copy multiple blocks along X per thread for
  // simplicity, to make sure all rows (also including the last one -
  // GetCopyableFootprints aligns row offsets, but not the total size) are
  // properly padded to the number of blocks copied in an invocation without
  // implicit assumptions about D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
  DXGI_FORMAT host_copy_format = GetDXGIResourceFormat(guest_format, width, height);
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    level_host_slice_layout.Offset = copy_buffer_size;
    level_host_slice_layout.Footprint.Format = host_copy_format;
    if (level == level_packed) {
      // Loading the packed tail for the base or the mips - load the whole tail
      // to copy regions out of it.
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_host_slice_layout.Footprint.Width = guest_layout_packed.x_extent_blocks * block_width;
      level_host_slice_layout.Footprint.Height = guest_layout_packed.y_extent_blocks * block_height;
      level_host_slice_layout.Footprint.Depth = guest_layout_packed.z_extent;
    } else {
      level_host_slice_layout.Footprint.Width = std::max(width >> level, uint32_t(1));
      level_host_slice_layout.Footprint.Height = std::max(height >> level, uint32_t(1));
      level_host_slice_layout.Footprint.Depth = std::max(depth >> level, uint32_t(1));
    }
    level_host_slice_layout.Footprint.Width =
        rex::round_up(level_host_slice_layout.Footprint.Width * texture_resolution_scale_x,
                      UINT(host_block_width));
    level_host_slice_layout.Footprint.Height =
        rex::round_up(level_host_slice_layout.Footprint.Height * texture_resolution_scale_y,
                      UINT(host_block_height));
    level_host_slice_layout.Footprint.RowPitch =
        rex::align(rex::round_up(level_host_slice_layout.Footprint.Width / host_block_width,
                                 host_x_blocks_per_thread) *
                       load_shader_info.bytes_per_host_block,
                   uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    UINT64 level_host_slice_size =
        rex::align(UINT64(level_host_slice_layout.Footprint.RowPitch) *
                       (level_host_slice_layout.Footprint.Height / host_block_height) *
                       level_host_slice_layout.Footprint.Depth,
                   UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    (is_base ? host_slice_size_base : host_slice_sizes_mips[level]) = level_host_slice_size;
    copy_buffer_size += level_host_slice_size * array_size;
  }
  D3D12_RESOURCE_STATES copy_buffer_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* copy_buffer =
      command_processor_.RequestScratchGPUBuffer(uint32_t(copy_buffer_size), copy_buffer_state);
  if (copy_buffer == nullptr) {
    return false;
  }

  // Begin loading.
  // May use different buffers for scaled base and mips, and also addressability
  // of more than 128 * 2^20 (2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP)
  // texels is not mandatory - need two separate UAV descriptors for base and
  // mips.
  // Destination.
  uint32_t descriptor_count = 1;
  if (texture_resolution_scaled) {
    // Source - base and mips, one or both.
    descriptor_count += (level_first == 0 && level_last != 0) ? 2 : 1;
  } else {
    // Source - shared memory.
    if (!bindless_resources_used_) {
      ++descriptor_count;
    }
  }
  ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair descriptors_allocated[3];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(descriptor_count,
                                                             descriptors_allocated)) {
    command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
    return false;
  }
  uint32_t descriptor_write_index = 0;
  command_processor_.SetExternalPipeline(pipeline);
  command_list.D3DSetComputeRootSignature(load_root_signature_.Get());
  // Set up the destination descriptor.
  assert_true(descriptor_write_index < descriptor_count);
  ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair descriptor_dest =
      descriptors_allocated[descriptor_write_index++];
  ui::ngpu_d3d12::util::CreateBufferTypedUAV(
      device, descriptor_dest.first, copy_buffer,
      ui::ngpu_d3d12::util::GetUintPow2DXGIFormat(load_shader_info.dest_bpe_log2),
      uint32_t(copy_buffer_size) >> load_shader_info.dest_bpe_log2);
  command_list.D3DSetComputeRootDescriptorTable(2, descriptor_dest.second);
  // Set up the unscaled source descriptor (scaled needs two descriptors that
  // depend on the buffer being current, so they will be set later - for mips,
  // after loading the base is done).
  if (!texture_resolution_scaled) {
    D3D12SharedMemory& d3d12_shared_memory = static_cast<D3D12SharedMemory&>(shared_memory());
    d3d12_shared_memory.UseForReading();
    ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair descriptor_unscaled_source;
    if (bindless_resources_used_) {
      descriptor_unscaled_source = command_processor_.GetSharedMemoryUintPow2BindlessSRVHandlePair(
          load_shader_info.source_bpe_log2);
    } else {
      assert_true(descriptor_write_index < descriptor_count);
      descriptor_unscaled_source = descriptors_allocated[descriptor_write_index++];
      d3d12_shared_memory.WriteUintPow2SRVDescriptor(descriptor_unscaled_source.first,
                                                     load_shader_info.source_bpe_log2);
    }
    command_list.D3DSetComputeRootDescriptorTable(1, descriptor_unscaled_source.second);
  }

  // Submit the copy buffer population commands.

  auto& cbuffer_pool = command_processor_.GetConstantBufferPool();
  LoadConstants load_constants;
  // 3 bits for each.
  assert_true(texture_resolution_scale_x <= 7);
  assert_true(texture_resolution_scale_y <= 7);
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (texture_resolution_scale_x << 4) |
      (texture_resolution_scale_y << 7);

  // The loop is slices within levels because the base and the levels may need
  // different portions of the scaled resolve virtual address space to be
  // available through buffers, and to create a descriptor, the buffer start
  // address is required - which may be different for base and mips.
  bool scaled_mips_source_set_up = false;
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;

    uint32_t guest_address = (is_base ? texture_key.base_page : texture_key.mip_page) << 12;

    // Set up the base or mips source, also making it accessible if loading from
    // scaled resolve memory.
    if (texture_resolution_scaled && (is_base || !scaled_mips_source_set_up)) {
      uint32_t guest_size_unscaled =
          is_base ? d3d12_texture.GetGuestBaseSize() : d3d12_texture.GetGuestMipsSize();
      if (!MakeScaledResolveRangeCurrent(guest_address, guest_size_unscaled,
                                         load_shader_info.source_bpe_log2)) {
        command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
        return false;
      }
      TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      assert_true(descriptor_write_index < descriptor_count);
      ui::ngpu_d3d12::util::DescriptorCpuGpuHandlePair descriptor_scaled_source =
          descriptors_allocated[descriptor_write_index++];
      CreateCurrentScaledResolveRangeUintPow2SRV(descriptor_scaled_source.first,
                                                 load_shader_info.source_bpe_log2);
      command_list.D3DSetComputeRootDescriptorTable(1, descriptor_scaled_source.second);
      if (!is_base) {
        scaled_mips_source_set_up = true;
      }
    }

    if (texture_resolution_scaled) {
      // Offset already applied in the buffer because more than 512 MB can't be
      // directly addresses as R32 on some hardware (above
      // 2^D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP).
      load_constants.guest_offset = 0;
    } else {
      load_constants.guest_offset = guest_address;
    }
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level] *
                                     (texture_resolution_scale_x * texture_resolution_scale_y);
    }
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (texture_key.tiled) {
      // Shaders expect pitch in blocks for tiled textures.
      level_guest_pitch /= bytes_per_block;
      assert_zero(level_guest_pitch & (xenos::kTextureTileWidthHeight - 1));
    }
    load_constants.guest_pitch_aligned = level_guest_pitch;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    assert_true(!is_3d_tiling || !(load_constants.guest_z_stride_block_rows_aligned &
                                   (xenos::kTextureTileWidthHeight - 1)));

    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      // This is the packed mip tail, containing not only the specified level,
      // but also other levels at different offsets - load the entire needed
      // extents.
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, uint32_t(1));
      level_height = std::max(height >> level, uint32_t(1));
      level_depth = std::max(depth >> level, uint32_t(1));
    }
    load_constants.size_blocks[0] =
        (level_width + (block_width - 1)) / block_width * texture_resolution_scale_x;
    load_constants.size_blocks[1] =
        (level_height + (block_height - 1)) / block_height * texture_resolution_scale_y;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    uint32_t group_count_x =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    uint32_t group_count_y =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_slice_layout =
        is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
    uint32_t host_slice_size =
        uint32_t(is_base ? host_slice_size_base : host_slice_sizes_mips[level]);
    load_constants.host_offset = uint32_t(level_host_slice_layout.Offset);
    load_constants.host_pitch = level_host_slice_layout.Footprint.RowPitch;

    command_list.D3DSetComputeRoot32BitConstants(0, sizeof(load_constants) / sizeof(uint32_t),
                                                 &load_constants, 0);

    uint32_t level_array_slice_stride_bytes_scaled =
        level_guest_layout.array_slice_stride_bytes *
        (texture_resolution_scale_x * texture_resolution_scale_y);
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      if (slice != 0) {
        command_list.D3DSetComputeRoot32BitConstants(
            0, sizeof(load_constants.guest_offset) / sizeof(uint32_t), &load_constants.guest_offset,
            offsetof(LoadConstants, guest_offset) / sizeof(uint32_t));
        command_list.D3DSetComputeRoot32BitConstants(
            0, sizeof(load_constants.host_offset) / sizeof(uint32_t), &load_constants.host_offset,
            offsetof(LoadConstants, host_offset) / sizeof(uint32_t));
      }
      assert_true(copy_buffer_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      command_processor_.SubmitBarriers();
      command_list.D3DDispatch(group_count_x, group_count_y, load_constants.size_blocks[2]);
      load_constants.guest_offset += level_array_slice_stride_bytes_scaled;
      load_constants.host_offset += host_slice_size;
    }
  }

  // Update LRU caching because the texture will be used by the command list.
  d3d12_texture.MarkAsUsed();

  // Submit copying from the copy buffer to the host texture.
  ID3D12Resource* texture_resource = d3d12_texture.resource();
  command_processor_.PushTransitionBarrier(
      texture_resource, d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
      D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.PushTransitionBarrier(copy_buffer, copy_buffer_state,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
  copy_buffer_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_processor_.SubmitBarriers();
  uint32_t texture_level_count = texture_key.mip_max_level + 1;
  D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
  location_source.pResource = copy_buffer;
  location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location_dest.pResource = texture_resource;
  location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  for (uint32_t level = level_first; level <= level_last; ++level) {
    uint32_t guest_level = std::min(level, level_packed);
    location_source.PlacedFootprint =
        level ? host_slice_layouts_mips[guest_level] : host_slice_layout_base;
    location_dest.SubresourceIndex = level;
    UINT64 host_slice_size = level ? host_slice_sizes_mips[guest_level] : host_slice_size_base;
    D3D12_BOX source_box;
    const D3D12_BOX* source_box_ptr;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      source_box.left = level_offset_blocks_x * block_width * texture_resolution_scale_x;
      source_box.top = level_offset_blocks_y * block_height * texture_resolution_scale_y;
      source_box.front = level_offset_z;
      source_box.right =
          source_box.left +
          rex::align(std::max((width * texture_resolution_scale_x) >> level, uint32_t(1)),
                     host_block_width);
      source_box.bottom =
          source_box.top +
          rex::align(std::max((height * texture_resolution_scale_y) >> level, uint32_t(1)),
                     host_block_height);
      source_box.back = source_box.front + std::max(depth >> level, uint32_t(1));
      source_box_ptr = &source_box;
    } else {
      // Non-packed level: copy the whole footprint. The footprint is sized as
      // the guest mip reduced then scaled, while the host mip subresource is
      // the base scaled then reduced, so for the deepest mips of scaled
      // textures the footprint can be a row or column larger. Compressed dests
      // round up to the block and absorb it. For uncompressed dests clamp the
      // copy to the exact subresource with an explicit source box to avoid
      // overrunning. Clamp per axis to min(footprint, subresource) since a
      // non-power-of-two axis can be smaller than the subresource while another
      // axis is larger, and the box must not exceed the source footprint
      // either. Canary f25003c0e.
      source_box_ptr = nullptr;
      if (!host_block_compressed) {
        const D3D12_SUBRESOURCE_FOOTPRINT& footprint = location_source.PlacedFootprint.Footprint;
        uint32_t dst_width =
            std::max((width * texture_resolution_scale_x) >> level, uint32_t(1));
        uint32_t dst_height =
            std::max((height * texture_resolution_scale_y) >> level, uint32_t(1));
        uint32_t dst_depth = std::max(depth >> level, uint32_t(1));
        if (footprint.Width > dst_width || footprint.Height > dst_height ||
            footprint.Depth > dst_depth) {
          source_box.left = 0;
          source_box.top = 0;
          source_box.front = 0;
          source_box.right = std::min(footprint.Width, dst_width);
          source_box.bottom = std::min(footprint.Height, dst_height);
          source_box.back = std::min(footprint.Depth, dst_depth);
          source_box_ptr = &source_box;
        }
      }
    }
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      command_list.D3DCopyTextureRegion(&location_dest, 0, 0, 0, &location_source, source_box_ptr);
      location_dest.SubresourceIndex += texture_level_count;
      location_source.PlacedFootprint.Offset += host_slice_size;
    }
  }

  command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);

  return true;
}

void D3D12TextureCache::UpdateTextureBindingsImpl(uint32_t fetch_constant_mask) {
  uint32_t bindings_remaining = fetch_constant_mask;
  uint32_t binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[binding_index];
    d3d12_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    if (IsSignedVersionSeparateForFormat(binding->key)) {
      if (binding->texture && texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture),
                                          binding->key.dimension, false, binding->host_swizzle);
      }
      if (binding->texture_signed && texture_util::IsAnySignSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index_signed =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture_signed),
                                          binding->key.dimension, true, binding->host_swizzle);
      }
    } else {
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, false, binding->host_swizzle);
        }
        if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index_signed = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, true, binding->host_swizzle);
        }
      }
    }
  }
}

ID3D12Resource* D3D12TextureCache::D3D12Texture::GetOrCreate3DAs2DResource(
    D3D12_RESOURCE_STATES end_state) {
  if (!REXCVAR_GET(gpu_3d_to_2d_texture)) {
    return nullptr;
  }

  auto& d3d12_cache = static_cast<D3D12TextureCache&>(texture_cache());

  if (texture_3d_as_2d_) {
    d3d12_cache.command_processor_.PushTransitionBarrier(
        texture_3d_as_2d_->resource(), texture_3d_as_2d_->SetResourceState(end_state), end_state);
    return texture_3d_as_2d_->resource();
  }

  const ui::ngpu_d3d12::D3D12Provider& provider = d3d12_cache.command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = key().GetWidth();
  desc.Height = key().GetHeight();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = d3d12_cache.GetDXGIResourceFormat(key());
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    return nullptr;
  }
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_2d;
  if (FAILED(device->CreateCommittedResource(&ui::ngpu_d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             initial_state, nullptr, IID_PPV_ARGS(&resource_2d)))) {
    REXGPU_ERROR("D3D12TextureCache: Failed to create 3D-as-2D wrapper resource");
    return nullptr;
  }

  TextureKey key_2d = key();
  key_2d.depth_or_array_size_minus_1 = 0;
  key_2d.mip_max_level = 0;
  texture_3d_as_2d_.reset(
      new D3D12Texture(d3d12_cache, key_2d, resource_2d.Get(), initial_state, false));
  texture_3d_as_2d_->SetForceLoad3DTiling(true);

  if (!d3d12_cache.LoadTextureData(*texture_3d_as_2d_)) {
    REXGPU_ERROR("D3D12TextureCache: Failed to load 3D-as-2D wrapper data");
    texture_3d_as_2d_.reset();
    return nullptr;
  }

  d3d12_cache.command_processor_.PushTransitionBarrier(
      texture_3d_as_2d_->resource(), texture_3d_as_2d_->SetResourceState(end_state), end_state);
  return texture_3d_as_2d_->resource();
}

uint32_t D3D12TextureCache::FindOrCreateTextureDescriptor(D3D12Texture& texture,
                                                          xenos::DataDimension dimension,
                                                          bool is_signed, uint32_t host_swizzle) {
  D3D12Texture::SRVDescriptorKey descriptor_key;
  descriptor_key.key = 0;
  descriptor_key.is_signed = uint32_t(is_signed);
  descriptor_key.host_swizzle = host_swizzle;
  descriptor_key.dimension = uint32_t(dimension);

  // Try to find an existing descriptor.
  uint32_t existing_descriptor_index = texture.GetSRVDescriptorIndex(descriptor_key);
  if (existing_descriptor_index != UINT32_MAX) {
    PROFILE_TEXTURE_CACHE_HIT();
    return existing_descriptor_index;
  }
  PROFILE_TEXTURE_CACHE_MISS();

  TextureKey texture_key = texture.key();

  // Create a new bindless or cached descriptor if supported.
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};

  if (IsSignedVersionSeparateForFormat(texture_key) &&
      texture_key.signed_separate != uint32_t(is_signed)) {
    // Not the version with the needed signedness.
    return UINT32_MAX;
  }
  xenos::TextureFormat format = texture_key.format;
  if (is_signed) {
    // Not supporting signed compressed textures - hopefully DXN and DXT5A are
    // not used as signed.
    desc.Format = host_formats_[uint32_t(format)].dxgi_format_signed;
  } else {
    desc.Format = GetDXGIUnormFormat(texture_key);
  }
  // [texpack] The resource was created as RGBA, so the view must say so - a
  // view whose format disagrees with its resource is invalid, and the debug
  // layer is the only thing that would ever tell you.
  //
  // The SWIZZLE is deliberately left alone. It describes how the guest's
  // channels map to what the shader expects, which is a property of the
  // texture's meaning rather than of how it is stored; the offline tool decodes
  // into the same channel order the load shader would have produced.
  // The resource's own decision, for the same reason as the upload: a view in
  // the guest format on an RGBA resource created for the pack is invalid, and
  // that is exactly what a fresh lookup answers once the pack is switched off.
  const bool srv_replacement = texture.texpack_replaced() || texture.texpack_resource() != nullptr;
  if (srv_replacement) {
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[uint32_t(format)] |=
        is_signed ? kUnsupportedSnormBit : kUnsupportedUnormBit;
    return UINT32_MAX;
  }

  uint32_t mip_levels = srv_replacement ? uint32_t(-1) : texture_key.mip_max_level + 1;
  ID3D12Resource* resource_for_view =
      texture.texpack_resource() ? texture.texpack_resource() : texture.resource();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MostDetailedMip = 0;
      desc.Texture3D.MipLevels = mip_levels;
      desc.Texture3D.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::kCube:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      desc.TextureCube.MostDetailedMip = 0;
      desc.TextureCube.MipLevels = mip_levels;
      desc.TextureCube.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      if (texture_key.dimension == xenos::DataDimension::k3D) {
        resource_for_view =
            texture.GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!resource_for_view) {
          return UINT32_MAX;
        }
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = 1;
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      } else {
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = mip_levels;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = texture_key.GetDepthOrArraySize();
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      }
      break;
    default:
      assert_unhandled_case(dimension);
      return UINT32_MAX;
  }

  desc.Shader4ComponentMapping =
      host_swizzle | D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;

  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  uint32_t descriptor_index;
  if (bindless_resources_used_) {
    descriptor_index = command_processor_.RequestPersistentViewBindlessDescriptor();
    if (descriptor_index == UINT32_MAX) {
      REXGPU_ERROR(
          "Failed to create a texture descriptor - no free bindless view "
          "descriptors");
      return UINT32_MAX;
    }
  } else {
    if (!srv_descriptor_cache_free_.empty()) {
      descriptor_index = srv_descriptor_cache_free_.back();
      srv_descriptor_cache_free_.pop_back();
    } else {
      // Allocated + 1 (including the descriptor that is being added), rounded
      // up to kSRVDescriptorCachePageSize, (allocated + 1 + size - 1).
      uint32_t cache_pages_needed =
          (srv_descriptor_cache_allocated_ + kSRVDescriptorCachePageSize) /
          kSRVDescriptorCachePageSize;
      if (srv_descriptor_cache_.size() < cache_pages_needed) {
        D3D12_DESCRIPTOR_HEAP_DESC cache_heap_desc;
        cache_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cache_heap_desc.NumDescriptors = kSRVDescriptorCachePageSize;
        cache_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        cache_heap_desc.NodeMask = 0;
        while (srv_descriptor_cache_.size() < cache_pages_needed) {
          Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cache_heap;
          if (FAILED(device->CreateDescriptorHeap(&cache_heap_desc, IID_PPV_ARGS(&cache_heap)))) {
            REXGPU_ERROR(
                "D3D12TextureCache: Failed to create a texture descriptor - "
                "couldn't create a descriptor cache heap");
            return UINT32_MAX;
          }
          srv_descriptor_cache_.emplace_back(cache_heap.Get());
        }
      }
      descriptor_index = srv_descriptor_cache_allocated_++;
    }
  }
  device->CreateShaderResourceView(resource_for_view, &desc,
                                   GetTextureDescriptorCPUHandle(descriptor_index));
  texture.AddSRVDescriptorIndex(descriptor_key, descriptor_index);
  return descriptor_index;
}

void D3D12TextureCache::ReleaseTextureDescriptor(uint32_t descriptor_index) {
  if (bindless_resources_used_) {
    command_processor_.ReleaseViewBindlessDescriptorImmediately(descriptor_index);
  } else {
    srv_descriptor_cache_free_.push_back(descriptor_index);
  }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12TextureCache::GetTextureDescriptorCPUHandle(
    uint32_t descriptor_index) const {
  const ui::ngpu_d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (bindless_resources_used_) {
    return provider.OffsetViewDescriptor(command_processor_.GetViewBindlessHeapCPUStart(),
                                         descriptor_index);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      srv_descriptor_cache_[descriptor_index / kSRVDescriptorCachePageSize].heap_start();
  uint32_t heap_offset = descriptor_index % kSRVDescriptorCachePageSize;
  return provider.OffsetViewDescriptor(heap_start, heap_offset);
}

xenos::ClampMode D3D12TextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D 12,
    // but there's no Direct3D 9 equivalent anyway, and too weird to be suitable
    // for intentional real usage.
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    // No Direct3D 12 equivalents.
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

}  // namespace rex::graphics::ngpu_d3d12
