// VENDORED from rexglue-src 23ace0b:src/graphics/command_processor.cpp - systematic renames only (see vendor_rtc_d3d12.py / ORIGIN.txt):
// namespaces d3d12 -> ngpu_d3d12, plugin headers -> rtc_d3d12/facade.h, cvars -> plugin registry reads (7 bool, 1 string, 1 int).
#include <string>
#include <cstdint>
#include <rex/logging.h>
namespace ng2::ngpu::xlat { bool PluginBool(const char*, bool); std::string PluginString(const char*, const char*); int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); void RefreshInt(const char*, int32_t&); void RefreshBool(const char*, bool&); void RefreshString(const char*, std::string&); }
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

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include "rtc_d3d12/cp_base.h"
#include "rtc_d3d12/shared_memory_base.h"
#include <rex/graphics/flags.h>
#include "rtc_d3d12/graphics_system_standin.h"
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

bool& FLAGS_vsync_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("vsync", true); ::ng2::ngpu::xlat::RefreshBool("vsync", s); return s; }   // NATIVE PATCH: [live cvars]

bool& FLAGS_clear_memory_page_state_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("clear_memory_page_state", true) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

bool& FLAGS_occlusion_query_enable_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("occlusion_query_enable", true); return s; }

std::string& FLAGS_readback_resolve_storage_() { static std::string s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginString("readback_resolve", "none") : std::string("none"); return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

bool& FLAGS_readback_resolve_half_pixel_offset_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("readback_resolve_half_pixel_offset", false); return s; }

bool& FLAGS_readback_memexport_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("readback_memexport", true) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

bool& FLAGS_readback_memexport_fast_storage_() { static bool s = ::ng2::ngpu::rtc::NativeOwnsGuestMemory() ? ::ng2::ngpu::xlat::PluginBool("readback_memexport_fast", true) : false; return s; }   // NATIVE FORCED unless the native backend owns guest memory (plugin gpu_offload_to_native)

int32_t& FLAGS_query_occlusion_fake_sample_count_storage_() { static int32_t s = ::ng2::ngpu::xlat::PluginInt("query_occlusion_fake_sample_count", 1000); return s; }

bool& FLAGS_async_shader_compilation_storage_() { static bool s = ::ng2::ngpu::xlat::PluginBool("async_shader_compilation", true); return s; }

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  bool shared_mode_overrides_legacy = shared_mode != ReadbackResolveMode::kDisabled ||
                                      rex::cvar::HasNonDefaultValue("readback_resolve");
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_name_ &&
      rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_)) {
    return legacy_backend_flag;
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
  // [ring] Published so the shared-memory invalidation diagnostic can say
  // whether a CPU write just invalidated the pages the GPU reads its COMMANDS
  // from. An invalidated-and-re-uploaded ring page gives the reader
  // structurally valid packets with impossible counts - which is what the NG2
  // session captured twice, byte-identically.
  SharedMemory::SetRingRange(ptr, primary_buffer_size_);
  REXGPU_INFO("[ring] primary buffer at {:08X}, {} KB", ptr, primary_buffer_size_ >> 10);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

// ---------------------------------------------------------------------------
// NGPU_PM4: a per-frame census of the PM4 stream this plugin actually executes.
//
// The guest-side call census answers "which Direct3D entry points does a frame
// use"; it cannot answer "what does the GPU actually receive", and for NG2 the
// two disagree in a way that matters: the only function with an immediate
// DRAW_INDX_2 opcode (sub_8373B060) fired ONCE in ten seconds of 900-draw
// frames, so NG2 builds its draw packets dynamically and a static opcode scan
// cannot see them. This is the ground truth to design the native device
// against - how many draws a frame really has, and which Xenos registers the
// title programs at all.
//
// Env-gated so it costs one already-loaded bool when off:
//   NGPU_PM4=1            enable
//   NGPU_PM4_EVERY=<n>    dump every n frames (default 300 = 5 s at 60 fps)
namespace ngpu_pm4 {

bool Enabled() {
  static const bool on = std::getenv("NGPU_PM4") != nullptr;
  return on;
}

// Separate gate: the register census instruments WriteRegister, the hottest
// function in the plugin. Everything else here runs per PACKET, which is three
// orders of magnitude rarer. Kept switchable on its own so a run can tell the
// two apart rather than only telling census from no-census.
// Read on every register write: a function-local static would pay the thread-safe-init guard each call
// (PROFT5 line profile). A plain global, computed once; a benign race computes the same value.
static int8_t g_regs_enabled = -1;
bool RegsEnabled() {
  if (g_regs_enabled < 0) g_regs_enabled = std::getenv("NGPU_PM4_REGS") != nullptr ? 1 : 0;
  return g_regs_enabled != 0;
}

// The ring is executed by one thread (the GPU worker), so plain counters are
// correct here and an atomic would only cost.
constexpr uint32_t kRegMax = 0x8000;   // type-0 base_index is masked to 15 bits
uint32_t g_reg_writes[kRegMax];        // this frame
uint8_t g_reg_ever[kRegMax];           // whole run
uint32_t g_type[4];
uint32_t g_op[128];
uint32_t g_op_ever[128];
uint64_t g_frame = 0;

// Draws, DECODED rather than dumped. A raw dword is unreadable and invites the
// same misreading twice; VGT_DRAW_INITIATOR is three fields that decide the
// whole shape of the native draw call, so decode once, here.
//   source_select  kDMA = a guest index buffer, kAutoIndex = no index buffer
//   prim_type      the topology the native pipeline state needs
uint32_t g_prim[64];                   // draws this frame by primitive type
uint32_t g_srcsel[4];                  // by index source
uint32_t g_idxfmt[2];                  // 16-bit / 32-bit indices, kDMA only
uint64_t g_indices = 0;                // total indices this frame
uint32_t g_draws_indexed = 0, g_draws_auto = 0;

// Predicated tiling. If a title replays the frame once per EDRAM tile, the ring
// asks for each draw several times while the game issues it once, and every
// per-frame draw count here is multiplied by the tile count. NG2 emits
// SET_BIN_MASK_LO six times a frame, which is enough to make the question real.
//
// The test: bucket draws by the (bin_mask, bin_select) pair in force. One pair
// carrying every draw means no replay and the counts stand. Several pairs with
// comparable counts means the frame is being replayed and every draw number in
// M4_ng2_pm4_census.md is inflated by that factor.
constexpr int kBinMax = 48;
uint64_t g_bin_key[kBinMax];
uint32_t g_bin_draws[kBinMax];
int g_bin_n = 0;
uint64_t g_bin_sel[kBinMax];
uint32_t g_bin_predicated = 0;   // draw packets carrying the predicate bit
uint32_t g_bin_skipped = 0;      // ...and how many the predicate rejected
uint32_t g_bin_overflow = 0;     // draws that arrived after kBinMax pairs were seen
uint32_t g_bin_unpred = 0;       // draws with no predicate bit: submitted once, whatever the bins

// Indirect buffers. The ring executes 92 of them a frame in Fable, and a draw
// inside one was written into guest memory at some earlier moment - possibly
// once, at load time, and replayed every frame since. The API wrapper hooks a
// native path installs fire when the buffer is RECORDED. If a title records
// its static geometry once and replays it, those draws are in every frame of
// the ring and in none of the hooks, forever. That is exactly the shape of
// Fable's missing ground, so the census has to be able to see it.
uint32_t g_ib_depth = 0;         // nesting: >0 means we are inside an indirect buffer
uint32_t g_draws_in_ib = 0;      // draws this frame executed from inside one
uint64_t g_indices_in_ib = 0;
constexpr int kIbMax = 32;
uint32_t g_ib_ptr[kIbMax];       // distinct indirect buffers this frame
uint32_t g_ib_draws[kIbMax];
uint32_t g_ib_frames[kIbMax];    // how many of the censused frames this pointer appeared in
int g_ib_n = 0;
uint32_t g_ib_overflow = 0;
uint32_t g_ib_cur = 0;           // the buffer being executed right now
// The shaders a draw inside an indirect buffer uses, by guest microcode
// address: the point of the whole instrument is to NAME them, so they can be
// compared against the set the native path draws rather than inferred.
constexpr int kIbShaderMax = 24;
uint32_t g_ib_vs[kIbShaderMax];
uint32_t g_ib_ps[kIbShaderMax];
uint32_t g_ib_hits[kIbShaderMax];
uint64_t g_ib_shader_indices[kIbShaderMax];
int g_ib_shader_n = 0;
uint32_t g_ib_shader_overflow = 0;

void NoteIndirectBuffer(uint32_t ptr) {
  for (int k = 0; k < g_ib_n; ++k) {
    if (g_ib_ptr[k] == ptr) { ++g_ib_frames[k]; return; }
  }
  if (g_ib_n < kIbMax) {
    g_ib_ptr[g_ib_n] = ptr;
    g_ib_draws[g_ib_n] = 0;
    g_ib_frames[g_ib_n] = 1;
    ++g_ib_n;
  } else {
    ++g_ib_overflow;
  }
}

// The pass table. bin_select says which bin the GPU is rendering right now, so
// one entry per select is one pass over the scene; several passes carrying
// comparable draw counts is a frame being replayed per tile.
constexpr int kSelMax = 24;
uint64_t g_sel_key[kSelMax];
uint32_t g_sel_draws[kSelMax];
uint64_t g_sel_indices[kSelMax];
int g_sel_n = 0;
uint32_t g_sel_overflow = 0;
uint64_t g_last_indices = 0;     // set by Peek so the pass table can attribute them

// Called for every draw packet the predicate LET THROUGH, with the bin state in
// force at that moment. Rejected draws are counted separately in g_bin_skipped:
// a rejected draw is not a bucket, it is a draw this bin does not want.
void Bucket(uint64_t mask, uint64_t select) {
  bool found = false;
  for (int k = 0; k < g_sel_n; ++k) {
    if (g_sel_key[k] == select) { ++g_sel_draws[k]; g_sel_indices[k] += g_last_indices; found = true; break; }
  }
  if (!found) {
    if (g_sel_n < kSelMax) {
      g_sel_key[g_sel_n] = select;
      g_sel_draws[g_sel_n] = 1;
      g_sel_indices[g_sel_n] = g_last_indices;
      ++g_sel_n;
    } else {
      ++g_sel_overflow;
    }
  }
  for (int k = 0; k < g_bin_n; ++k) {
    if (g_bin_key[k] == mask && g_bin_sel[k] == select) { ++g_bin_draws[k]; return; }
  }
  if (g_bin_n < kBinMax) {
    g_bin_key[g_bin_n] = mask;
    g_bin_sel[g_bin_n] = select;
    g_bin_draws[g_bin_n] = 1;
    ++g_bin_n;
  } else {
    ++g_bin_overflow;
  }
}


// Shader loads. NG2 uses IM_LOAD (a POINTER into guest memory) far more than
// IM_LOAD_IMMEDIATE, so the shader identity a native path keys on is this
// address - it does not have to be recovered from a guest function's arguments.
constexpr int kShaderMax = 512;
uint32_t g_shader_addr[kShaderMax];
uint8_t g_shader_type[kShaderMax];
uint32_t g_shader_hits[kShaderMax];
int g_shader_n = 0;

// The last shader of each type loaded, which is what the next draw will use.
uint32_t g_cur_vs = 0, g_cur_ps = 0;
uint32_t g_cur_vs_dwords = 0, g_cur_ps_dwords = 0;
bool g_vs_immediate = false, g_ps_immediate = false;
// The microcode of the last IM_LOAD_IMMEDIATE of each type, copied out of the ring as the guest wrote it (big-endian
// dwords): an inline shader has no guest address, and g_cur_vs/g_cur_ps still name the PREVIOUS address-loaded
// shader - the Fable II lake's reflection blit was handed off as the sky cube's VS it followed.
std::vector<uint8_t> g_imm_vs, g_imm_ps;

// NGPU_PM4_DESCRIBE: for the first few draws of a dumped frame, print EVERYTHING
// a native draw call would need, gathered from the packet and the register file.
// The point is not the numbers - it is to find out, before a renderer exists,
// whether the stream alone is a sufficient input. A field that comes out blank
// here is a field the native path would have had to go hunting for later.
bool g_describe = false;
int g_desc_n = 0;

const char* OpName(uint32_t op) {
  switch (op) {
    case PM4_ME_INIT: return "ME_INIT";
    case PM4_NOP: return "NOP";
    case PM4_INTERRUPT: return "INTERRUPT";
    case PM4_XE_SWAP: return "XE_SWAP";
    case PM4_INDIRECT_BUFFER: return "INDIRECT_BUFFER";
    case PM4_INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
    case PM4_WAIT_REG_MEM: return "WAIT_REG_MEM";
    case PM4_REG_RMW: return "REG_RMW";
    case PM4_REG_TO_MEM: return "REG_TO_MEM";
    case PM4_MEM_WRITE: return "MEM_WRITE";
    case PM4_COND_WRITE: return "COND_WRITE";
    case PM4_EVENT_WRITE: return "EVENT_WRITE";
    case PM4_EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
    case PM4_EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
    case PM4_EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
    case PM4_DRAW_INDX: return "DRAW_INDX";
    case PM4_DRAW_INDX_2: return "DRAW_INDX_2";
    case PM4_SET_CONSTANT: return "SET_CONSTANT";
    case PM4_SET_CONSTANT2: return "SET_CONSTANT2";
    case PM4_LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
    case PM4_SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
    case PM4_IM_LOAD: return "IM_LOAD";
    case PM4_IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
    case PM4_INVALIDATE_STATE: return "INVALIDATE_STATE";
    case PM4_VIZ_QUERY: return "VIZ_QUERY";
    case PM4_SET_BIN_MASK: return "SET_BIN_MASK";
    case PM4_SET_BIN_SELECT: return "SET_BIN_SELECT";
    case PM4_SET_BIN_MASK_LO: return "SET_BIN_MASK_LO";
    case PM4_SET_BIN_MASK_HI: return "SET_BIN_MASK_HI";
    case PM4_SET_BIN_SELECT_LO: return "SET_BIN_SELECT_LO";
    case PM4_SET_BIN_SELECT_HI: return "SET_BIN_SELECT_HI";
    case PM4_CONTEXT_UPDATE: return "CONTEXT_UPDATE";
    default: return "?";
  }
}

// Read a packet's payload WITHOUT consuming it - the real handler runs right
// after and must see the reader untouched.
uint32_t At(memory::RingBuffer* reader, uint32_t offset, uint32_t i) {
  return rex::memory::load_and_swap<uint32_t>(
      reader->buffer() + ((offset + i * sizeof(uint32_t)) % reader->capacity()));
}

void Describe(const RegisterFile& regs, memory::RingBuffer* reader, uint32_t opcode,
              uint32_t offset, uint32_t count, reg::VGT_DRAW_INITIATOR di);

// The shader addresses, tracked whether or not the census is running: the
// native-renderer bridge needs them for every draw, and the census is a
// diagnostic that is normally off.
void TrackShaders(memory::RingBuffer* reader, uint32_t opcode, uint32_t offset, uint32_t count) {
  if (opcode == PM4_IM_LOAD_IMMEDIATE && count >= 2) {
    const bool ps = (At(reader, offset, 0) & 0x3) != 0;
    (ps ? g_ps_immediate : g_vs_immediate) = true;
    const uint32_t dwords = std::min<uint32_t>(At(reader, offset, 1) & 0xFFFF, count - 2);
    std::vector<uint8_t>& dst = ps ? g_imm_ps : g_imm_vs;
    dst.resize(size_t(dwords) * 4);
    for (uint32_t k = 0; k < dwords; ++k)   // raw bytes, the ring may wrap
      std::memcpy(dst.data() + k * 4, reader->buffer() + ((offset + (2 + k) * sizeof(uint32_t)) % reader->capacity()), 4);
    return;
  }
  if (opcode == PM4_IM_LOAD && count >= 2) {
    const uint32_t addr_type = At(reader, offset, 0);
    const uint32_t dwords = At(reader, offset, 1) & 0xFFFF;
    if ((addr_type & 0x3) == 0) { g_cur_vs = addr_type & ~0x3u; g_cur_vs_dwords = dwords; g_vs_immediate = false; }
    else                        { g_cur_ps = addr_type & ~0x3u; g_cur_ps_dwords = dwords; g_ps_immediate = false; }
  }
}

void Peek(memory::RingBuffer* reader, uint32_t opcode, uint32_t offset, uint32_t count,
          const RegisterFile* regs) {
  if (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2) {
    // DRAW_INDX leads with a viz-query token; DRAW_INDX_2 does not.
    const uint32_t i = (opcode == PM4_DRAW_INDX) ? 1 : 0;
    if (count <= i) return;
    reg::VGT_DRAW_INITIATOR di;
    di.value = At(reader, offset, i);
    ++g_prim[static_cast<uint32_t>(di.prim_type) & 0x3F];
    ++g_srcsel[static_cast<uint32_t>(di.source_select) & 3];
    g_indices += di.num_indices;
    if (di.source_select == xenos::SourceSelect::kDMA) {
      ++g_draws_indexed;
      ++g_idxfmt[di.index_size == xenos::IndexFormat::kInt16 ? 0 : 1];
    } else {
      ++g_draws_auto;
    }
    if (g_ib_depth) {
      ++g_draws_in_ib;
      g_indices_in_ib += di.num_indices;
      bool seen = false;
      for (int k = 0; k < g_ib_shader_n; ++k) {
        if (g_ib_vs[k] == g_cur_vs && g_ib_ps[k] == g_cur_ps) {
          ++g_ib_hits[k];
          g_ib_shader_indices[k] += di.num_indices;
          seen = true;
          break;
        }
      }
      if (!seen) {
        if (g_ib_shader_n < kIbShaderMax) {
          g_ib_vs[g_ib_shader_n] = g_cur_vs;
          g_ib_ps[g_ib_shader_n] = g_cur_ps;
          g_ib_hits[g_ib_shader_n] = 1;
          g_ib_shader_indices[g_ib_shader_n] = di.num_indices;
          ++g_ib_shader_n;
        } else {
          ++g_ib_shader_overflow;
        }
      }
      for (int k = 0; k < g_ib_n; ++k) {
        if (g_ib_ptr[k] == g_ib_cur) { ++g_ib_draws[k]; break; }
      }
    }
    if (g_describe && g_desc_n < 6 && regs) Describe(*regs, reader, opcode, offset, count, di);
    return;
  }
  if (opcode == PM4_IM_LOAD_IMMEDIATE && count >= 2) {
    // Microcode embedded in the packet: there is no guest address to key on.
    (At(reader, offset, 0) & 0x3) ? (g_ps_immediate = true) : (g_vs_immediate = true);
    return;
  }
  if (opcode == PM4_IM_LOAD && count >= 2) {
    const uint32_t addr_type = At(reader, offset, 0);
    const uint32_t addr = addr_type & ~0x3u;
    const uint8_t type = static_cast<uint8_t>(addr_type & 0x3);
    const uint32_t dwords = At(reader, offset, 1) & 0xFFFF;
    if (type == 0) { g_cur_vs = addr; g_cur_vs_dwords = dwords; g_vs_immediate = false; }
    else           { g_cur_ps = addr; g_cur_ps_dwords = dwords; g_ps_immediate = false; }
    for (int k = 0; k < g_shader_n; ++k) {
      if (g_shader_addr[k] == addr && g_shader_type[k] == type) {
        ++g_shader_hits[k];
        return;
      }
    }
    if (g_shader_n < kShaderMax) {
      g_shader_addr[g_shader_n] = addr;
      g_shader_type[g_shader_n] = type;
      g_shader_hits[g_shader_n] = 1;
      ++g_shader_n;
    }
  }
}

const char* PrimName(uint32_t p) {
  switch (p) {
    case 1: return "POINT";
    case 2: return "LINE";
    case 3: return "LINE_STRIP";
    case 4: return "TRI";
    case 5: return "TRI_FAN";
    case 6: return "TRI_STRIP";
    case 8: return "RECT";
    case 13: return "QUAD";
    case 14: return "QUAD_STRIP";
    default: return "prim";
  }
}

// Everything a native draw call needs, gathered from the packet plus the
// register file. Register decodes are the ones documented in
// docs/native_gpu/EDRAM_AND_RESOLVES.md - notably RB_COLOR_INFO is a TILE base
// in bits 0:10 with the format at 16:19, not an address, which is the decode
// that cost the Fable session a day.
void Describe(const RegisterFile& regs, memory::RingBuffer* reader, uint32_t opcode,
              uint32_t offset, uint32_t count, reg::VGT_DRAW_INITIATOR di) {
  const uint32_t i0 = (opcode == PM4_DRAW_INDX) ? 1 : 0;
  const bool indexed = di.source_select == xenos::SourceSelect::kDMA;
  uint32_t dma_base = 0, dma_size = 0;
  if (indexed && count > i0 + 2) {
    dma_base = At(reader, offset, i0 + 1);
    dma_size = At(reader, offset, i0 + 2);
  }

  const uint32_t surface  = regs[XE_GPU_REG_RB_SURFACE_INFO];
  const uint32_t color0   = regs[XE_GPU_REG_RB_COLOR_INFO];
  const uint32_t depth    = regs[XE_GPU_REG_RB_DEPTH_INFO];
  const uint32_t modectl  = regs[XE_GPU_REG_RB_MODECONTROL];

  REXLOG_INFO("[ngpu-draw] #{} {} prim={} indices={}{}", g_desc_n,
              opcode == PM4_DRAW_INDX ? "DRAW_INDX" : "DRAW_INDX_2",
              PrimName(static_cast<uint32_t>(di.prim_type) & 0x3F),
              static_cast<uint32_t>(di.num_indices),
              indexed ? "" : "  (auto-index: no index buffer)");
  if (indexed) {
    REXLOG_INFO("[ngpu-draw]    IB guest=0x{:08X} words={} fmt={} endian={}", dma_base,
                dma_size & 0xFFFFFF, di.index_size == xenos::IndexFormat::kInt16 ? "u16" : "u32",
                (dma_size >> 30) & 3);
  }
  REXLOG_INFO("[ngpu-draw]    VS={} PS={}",
              g_vs_immediate ? std::string("(immediate)")
                             : fmt::format("0x{:08X} {} dw", g_cur_vs, g_cur_vs_dwords),
              g_ps_immediate ? std::string("(immediate)")
                             : fmt::format("0x{:08X} {} dw", g_cur_ps, g_cur_ps_dwords));
  REXLOG_INFO("[ngpu-draw]    RT colour base={} tiles fmt={} | depth base={} tiles fmt={} | "
              "pitch={} msaa={} | edram_mode={}",
              color0 & 0x7FF, (color0 >> 16) & 0xF, depth & 0x7FF, (depth >> 16) & 1,
              surface & 0x3FFF, (surface >> 16) & 3, modectl & 0x7);
  REXLOG_INFO("[ngpu-draw]    depthctl={:08X} blend0={:08X} colorctl={:08X} mask={:08X} "
              "rast={:08X} progcntl={:08X}",
              regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_BLENDCONTROL0],
              regs[XE_GPU_REG_RB_COLORCONTROL], regs[XE_GPU_REG_RB_COLOR_MASK],
              regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL], regs[XE_GPU_REG_SQ_PROGRAM_CNTL]);
  REXLOG_INFO("[ngpu-draw]    viewport scale=({}, {}, {}) offset=({}, {}, {}) scissor={:08X}..{:08X}",
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
              regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
              regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]);
  // Fetch constants are 6 dwords per texture slot and 2 per vertex slot; which
  // slots a draw uses is decided by the SHADER microcode, so all this can say
  // is how many are populated. Naming them needs the translator.
  int tex_slots = 0, nonzero = 0;
  for (uint32_t f = 0; f < 32; ++f) {
    const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + f * 6;
    bool any = false;
    for (uint32_t w = 0; w < 6; ++w) {
      if (regs[base + w]) { any = true; ++nonzero; }
    }
    if (any) ++tex_slots;
  }
  REXLOG_INFO("[ngpu-draw]    fetch constants: {} of 32 slots populated ({} non-zero dwords)",
              tex_slots, nonzero);
  ++g_desc_n;
}

// Dumped every NGPU_PM4_EVERY frames rather than once, so a gameplay frame can
// be picked out of the log afterwards instead of guessing a frame number in
// advance - the mistake that nearly made the guest-side transcript a capture of
// the title screen.
void FrameEnd() {
  ++g_frame;
  static const uint32_t kEvery = [] {
    const char* e = std::getenv("NGPU_PM4_EVERY");
    const int n = e ? std::atoi(e) : 0;
    return n > 0 ? static_cast<uint32_t>(n) : 300u;
  }();
  if (g_frame % kEvery == 0) {
    REXLOG_INFO("[ngpu-pm4] frame {}: type0={} type1={} type2={} type3={}", g_frame, g_type[0],
                g_type[1], g_type[2], g_type[3]);
    std::string ops;
    for (uint32_t o = 0; o < 128; ++o) {
      if (g_op[o]) ops += fmt::format(" {}({:02X})={}", OpName(o), o, g_op[o]);
    }
    REXLOG_INFO("[ngpu-pm4] frame {} opcodes:{}", g_frame, ops);
    // Never-fired opcodes matter as much as fired ones: they are the packets a
    // native device for THIS title does not have to implement.
    std::string never;
    for (uint32_t o = 0; o < 128; ++o) {
      if (!g_op_ever[o] && std::string_view(OpName(o)) != "?") never += fmt::format(" {}", OpName(o));
    }
    if (!never.empty()) REXLOG_INFO("[ngpu-pm4] not seen in this run:{}", never);

    std::string prims;
    for (uint32_t p = 0; p < 64; ++p) {
      if (g_prim[p]) prims += fmt::format(" {}({})={}", PrimName(p), p, g_prim[p]);
    }
    REXLOG_INFO("[ngpu-pm4] frame {} draws: indexed(kDMA)={} auto={} | indices={} | src[dma={} imm={} auto={} rsvd={}] | idx16={} idx32={}",
                g_frame, g_draws_indexed, g_draws_auto, g_indices, g_srcsel[0], g_srcsel[1],
                g_srcsel[2], g_srcsel[3], g_idxfmt[0], g_idxfmt[1]);
    REXLOG_INFO("[ngpu-pm4] frame {} prims:{}", g_frame, prims);

    // Predicated tiling. Several pairs with comparable draw counts means the
    // frame is REPLAYED per EDRAM tile, and every per-frame draw count above -
    // including the ring-vs-hooks coverage figure in M4_fable2.md - is inflated
    // by that factor. One pair carrying everything means the counts stand.
    {
      std::string bins;
      for (int k = 0; k < g_bin_n; ++k) {
        bins += fmt::format(" [{:016X}/{:016X}]={}", g_bin_key[k], g_bin_sel[k], g_bin_draws[k]);
      }
      if (g_bin_overflow) bins += fmt::format(" (+{} draws past {} pairs)", g_bin_overflow, kBinMax);
      std::string sels;
      for (int k = 0; k < g_sel_n; ++k) {
        sels += fmt::format(" select {:08X}: {} draws {} indices", g_sel_key[k], g_sel_draws[k], g_sel_indices[k]);
      }
      if (g_sel_overflow) sels += fmt::format(" (+{} past {})", g_sel_overflow, kSelMax);
      REXLOG_INFO("[ngpu-pm4] frame {} passes: {} distinct bin_select values -{}", g_frame, g_sel_n, sels);
      REXLOG_INFO("[ngpu-pm4] frame {} bins: {} distinct (mask,select) pair{}{}, predicated draws={} rejected={}",
                  g_frame, g_bin_n, g_bin_n == 1 ? "" : "s", bins, g_bin_predicated, g_bin_skipped);
      REXLOG_INFO("[ngpu-pm4] frame {} draws not predicated at all (submitted once whatever the bins): {}", g_frame, g_bin_unpred);

      // Every draw the ring asked for must land in exactly one bucket. If the
      // two totals disagree the census is seeing less of the frame than it
      // appears to, and the line has to say so rather than look clean - the
      // instrument's own coverage, measured instead of assumed.
      uint32_t bucketed = g_bin_overflow;
      for (int k = 0; k < g_bin_n; ++k) bucketed += g_bin_draws[k];
      const uint32_t asked = g_draws_indexed + g_draws_auto;
      REXLOG_INFO("[ngpu-pm4] frame {} bin census: {} bucketed + {} rejected vs {} draws in the frame - {}",
                  g_frame, bucketed, g_bin_skipped, asked,
                  bucketed == asked ? "reconciles" : "DOES NOT RECONCILE");

      std::string ibs;
      for (int k = 0; k < g_ib_n; ++k) {
        ibs += fmt::format(" {:08X}x{}draws", g_ib_ptr[k], g_ib_draws[k]);
      }
      if (g_ib_overflow) ibs += fmt::format(" (+{} past {})", g_ib_overflow, kIbMax);
      REXLOG_INFO("[ngpu-pm4] frame {} indirect buffers: {} distinct, {} draws inside them carrying {} indices -{}",
                  g_frame, g_ib_n, g_draws_in_ib, g_indices_in_ib, ibs.empty() ? " none" : ibs.c_str());
      if (g_ib_shader_n) {
        std::string sh;
        for (int k = 0; k < g_ib_shader_n; ++k) {
          sh += fmt::format(" VS:{:08X}/PS:{:08X} x{} ({} idx)", g_ib_vs[k], g_ib_ps[k], g_ib_hits[k], g_ib_shader_indices[k]);
          if (sh.size() > 900) { REXLOG_INFO("[ngpu-pm4] indirect-buffer shaders{}", sh); sh.clear(); }
        }
        if (!sh.empty()) REXLOG_INFO("[ngpu-pm4] indirect-buffer shaders{}", sh);
        if (g_ib_shader_overflow) REXLOG_INFO("[ngpu-pm4] indirect-buffer shaders: {} draws past {} pairs", g_ib_shader_overflow, kIbShaderMax);
      }
    }

    REXLOG_INFO("[ngpu-pm4] frame {}: {} distinct shader addresses loaded by IM_LOAD", g_frame,
                g_shader_n);
    {
      std::string sh;
      for (int k = 0; k < g_shader_n; ++k) {
        sh += fmt::format(" {}{:08X}x{}", g_shader_type[k] == 0 ? "VS:" : "PS:", g_shader_addr[k],
                          g_shader_hits[k]);
        if (sh.size() > 900) { REXLOG_INFO("[ngpu-pm4] shaders{}", sh); sh.clear(); }
      }
      if (!sh.empty()) REXLOG_INFO("[ngpu-pm4] shaders{}", sh);
    }
    // The whole register set, in chunks - a truncated list is the one that
    // omits exactly the register you go looking for later.
    int nregs = 0;
    std::string regs;
    if (!RegsEnabled()) {
      REXLOG_INFO("[ngpu-pm4] register census OFF (set NGPU_PM4_REGS=1) - not zero, not measured");
    }
    for (uint32_t r = 0; r < kRegMax; ++r) {
      if (!g_reg_writes[r]) continue;
      ++nregs;
      regs += fmt::format(" {:04X}x{}", r, g_reg_writes[r]);
      if (regs.size() > 900) { REXLOG_INFO("[ngpu-pm4] regs{}", regs); regs.clear(); }
    }
    if (!regs.empty()) REXLOG_INFO("[ngpu-pm4] regs{}", regs);
    REXLOG_INFO("[ngpu-pm4] frame {}: {} distinct registers written this frame", g_frame, nregs);
  }
  std::memset(g_reg_writes, 0, sizeof(g_reg_writes));
  std::memset(g_type, 0, sizeof(g_type));
  std::memset(g_op, 0, sizeof(g_op));
  std::memset(g_prim, 0, sizeof(g_prim));
  std::memset(g_srcsel, 0, sizeof(g_srcsel));
  std::memset(g_idxfmt, 0, sizeof(g_idxfmt));
  g_indices = 0;
  g_draws_indexed = g_draws_auto = 0;
  g_bin_n = 0;
  g_bin_predicated = g_bin_skipped = g_bin_overflow = g_bin_unpred = 0;
  g_sel_n = 0;
  g_sel_overflow = 0;
  g_draws_in_ib = 0;
  g_indices_in_ib = 0;
  g_ib_n = 0;
  g_ib_overflow = 0;
  g_ib_shader_n = 0;
  g_ib_shader_overflow = 0;
  g_shader_n = 0;
  g_desc_n = 0;
  g_describe = ((g_frame + 1) % kEvery == 0);
}

}  // namespace ngpu_pm4

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  if (ngpu_pm4::RegsEnabled() && index < ngpu_pm4::kRegMax) {
    ++ngpu_pm4::g_reg_writes[index];
    ngpu_pm4::g_reg_ever[index] = 1;
  }
  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  // The register-table lookup only feeds a debug message; it cost ~2% of the GPU thread per write (PROFT1).
  // The level is read once (the cross-module logger lookup per write was itself 0.6%, PROFT2).
  // A plain static (not a guarded local): see RegsEnabled.
  static int8_t gpu_debug = -1;
  if (gpu_debug < 0) {
    auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
    gpu_debug = (lp && lp->should_log(spdlog::level::debug)) ? 1 : 0;
  }
  if (gpu_debug > 0 && !regs.GetRegisterInfo(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");
  const uint32_t ngpu_ib_saved = ngpu_pm4::g_ib_cur;
  if (ngpu_pm4::Enabled()) {
    ++ngpu_pm4::g_ib_depth;
    ngpu_pm4::g_ib_cur = ptr;
    ngpu_pm4::NoteIndirectBuffer(ptr);
  }
  struct NgpuIbScope {
    uint32_t saved;
    ~NgpuIbScope() {
      if (ngpu_pm4::Enabled()) {
        --ngpu_pm4::g_ib_depth;
        ngpu_pm4::g_ib_cur = saved;
      }
    }
  } ngpu_ib_scope{ngpu_ib_saved};

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::CallInThreadSafe(std::function<void()> fn) {
  if (system::XThread::IsInThread(worker_thread_.get())) {
    fn();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(safe_fns_lock_);
    safe_fns_.push(std::move(fn));
  }
  safe_fns_pending_.store(true, std::memory_order_release);
  write_ptr_index_event_->Set();
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if (packet == 0) {
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  if (ngpu_pm4::Enabled()) ++ngpu_pm4::g_type[packet_type];

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    WriteRegister(target_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  return true;
}

// ---------------------------------------------------------------------------
// Native renderer bridge.
//
// Every draw in both titles reaches the GPU inside a chained indirect buffer
// (Fable 6,402 of 6,402; NG2 2,338 of 2,338), so this parser is the only
// complete reader of the command stream. A native renderer is therefore a
// CONSUMER hanging off this dispatch, not a second reader of the same bytes:
// the packet has already been sized and dispatched before the consumer sees
// it, so there is no independent parser to desynchronise, and the consumer's
// draw count reconciles against the census because it IS this traversal.
//
// The callback fires on the GPU worker thread, after the tiling predicate has
// been applied, once per draw the console would execute.
// ---------------------------------------------------------------------------
extern "C" {

struct RexNgpuDraw {
  uint32_t size;               // sizeof, so the two sides can differ in version safely
  const uint32_t* regs;        // the register file, indexed by register number
  uint32_t reg_count;
  uint32_t draw_initiator;     // VGT_DRAW_INITIATOR: prim 0..5, src 6..7, idx32 bit 11, count 16..31
  uint32_t index_addr;         // kDMA: the index buffer, GPU physical
  uint32_t index_size;         // as the packet gave it
  uint32_t inline_addr;        // kImmediate: guest address of the inline index data
  uint32_t vs_addr, vs_dwords, ps_addr, ps_dwords;
  uint32_t vs_inline, ps_inline;    // microcode came in the packet, not at an address
  uint32_t predicated;
  uint64_t bin_mask, bin_select;    // the tiling predicate in force at this draw
  // (size >= offsetof end) the microcode of an INLINE shader (vs_inline/ps_inline), big-endian dwords as the guest
  // wrote them, valid for the duration of the callback; null when the shader was loaded from an address.
  const uint8_t* vs_code;
  const uint8_t* ps_code;
  uint32_t vs_code_dwords, ps_code_dwords;
};

typedef void (*RexNgpuDrawFn)(const RexNgpuDraw*);
static RexNgpuDrawFn g_ngpu_draw_cb = nullptr;

__declspec(dllexport) void RexNgpuSetDrawCallback(RexNgpuDrawFn fn) { g_ngpu_draw_cb = fn; }

// THE RESOLVE, ON THE DRAW STREAM. A consumer replaying recorded draws needs to
// know where in that stream each resolve belongs. Taking it from the guest's own
// D3D9 Resolve hook does not work: that hook fires on the guest thread while
// this one processes the ring asynchronously, so when it fires the draws the
// guest already submitted may not have been processed here yet, and any
// position derived from a draw count is BEHIND where the resolve belongs. Two
// streams with no common clock - which no lock or atomic repairs.
//
// The marker carries NO parameters on purpose. The consumer already has them,
// in the fetch-constant encoding its own hook provides, whereas this side has
// RB_COPY_DEST_* registers in a different encoding; translating between them
// would be a second change riding along with this one. Ordering is the whole
// payload.
typedef void (*RexNgpuResolveFn)(void);
static RexNgpuResolveFn g_ngpu_resolve_cb = nullptr;

__declspec(dllexport) void RexNgpuSetResolveCallback(RexNgpuResolveFn fn) { g_ngpu_resolve_cb = fn; }

// Called from the copy path, which runs on this thread in ring order.
void RexNgpuNotifyResolve() {
  if (g_ngpu_resolve_cb) g_ngpu_resolve_cb();
}

// The frame boundary, on the same thread as the draws.
//
// A renderer driven by the draw callback cannot take its frame boundary from
// the guest's present hook: that runs on the guest thread, which is ahead of or
// behind this one by an unbounded amount, so a frame's draws and the end of
// that frame would arrive out of order. The swap packet IS the boundary the
// draws are ordered against, and it is right here.
//
// Called before IssueSwap so a consumer can finish and present its own frame
// while this one still holds the frame's state.
typedef void (*RexNgpuSwapFn)(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height);
static RexNgpuSwapFn g_ngpu_swap_cb = nullptr;
__declspec(dllexport) void RexNgpuSetSwapCallback(RexNgpuSwapFn fn) { g_ngpu_swap_cb = fn; }

}  // extern "C"

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      if (ngpu_pm4::Enabled() &&
          (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2)) {
        // A draw the predicate rejected. It never reaches the census below, so
        // without this a tiled title's rejected passes are invisible and the
        // executed ones look like the whole frame.
        ++ngpu_pm4::g_bin_skipped;
      }
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }

  if (g_ngpu_draw_cb && !ngpu_pm4::Enabled()) {
    ngpu_pm4::TrackShaders(reader, opcode, data_start_offset, count);
  }
  if (g_ngpu_draw_cb && (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2)) {
    const uint32_t i = (opcode == PM4_DRAW_INDX) ? 1u : 0u;
    if (count > i) {
      RexNgpuDraw d = {};
      d.size = sizeof(d);
      d.regs = register_file_ ? register_file_->values : nullptr;
      d.reg_count = register_file_ ? uint32_t(RegisterFile::kRegisterCount) : 0;
      d.draw_initiator = ngpu_pm4::At(reader, data_start_offset, i);
      const uint32_t src = (d.draw_initiator >> 6) & 3;
      if (src == 0 && count > i + 2) {           // kDMA: the index buffer follows
        d.index_addr = ngpu_pm4::At(reader, data_start_offset, i + 1);
        d.index_size = ngpu_pm4::At(reader, data_start_offset, i + 2);
      } else if (src == 1) {                     // kImmediate: the indices are in the packet
        d.inline_addr = 0;
      }
      d.vs_addr = ngpu_pm4::g_cur_vs; d.vs_dwords = ngpu_pm4::g_cur_vs_dwords;
      d.ps_addr = ngpu_pm4::g_cur_ps; d.ps_dwords = ngpu_pm4::g_cur_ps_dwords;
      d.vs_inline = ngpu_pm4::g_vs_immediate ? 1u : 0u;
      d.ps_inline = ngpu_pm4::g_ps_immediate ? 1u : 0u;
      if (d.vs_inline && !ngpu_pm4::g_imm_vs.empty()) { d.vs_code = ngpu_pm4::g_imm_vs.data(); d.vs_code_dwords = uint32_t(ngpu_pm4::g_imm_vs.size() / 4); }
      if (d.ps_inline && !ngpu_pm4::g_imm_ps.empty()) { d.ps_code = ngpu_pm4::g_imm_ps.data(); d.ps_code_dwords = uint32_t(ngpu_pm4::g_imm_ps.size() / 4); }
      d.predicated = (packet & 1) ? 1u : 0u;
      d.bin_mask = bin_mask_;
      d.bin_select = bin_select_;
      g_ngpu_draw_cb(&d);
    }
  }
  if (ngpu_pm4::Enabled()) {
    ++ngpu_pm4::g_op[opcode & 0x7F];
    ++ngpu_pm4::g_op_ever[opcode & 0x7F];
    if (opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2) {
      // The index count comes from this packet, so the pass table can say how
      // much GEOMETRY each pass carries and not only how many calls: two passes
      // over the same scene carry the same indices, a real second pass does not.
      const uint32_t di_i = (opcode == PM4_DRAW_INDX) ? 1u : 0u;
      ngpu_pm4::g_last_indices = 0;
      if (count > di_i) {
        reg::VGT_DRAW_INITIATOR di;
        di.value = ngpu_pm4::At(reader, data_start_offset, di_i);
        ngpu_pm4::g_last_indices = di.num_indices;
      }
      ngpu_pm4::Bucket(bin_mask_, bin_select_);
      if (packet & 1) ++ngpu_pm4::g_bin_predicated; else ++ngpu_pm4::g_bin_unpred;
    }
    ngpu_pm4::Peek(reader, opcode, data_start_offset, count, register_file_);
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  {
    static uint64_t last_frame_tick = 0;
    uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    if (last_frame_tick) {
      uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      int64_t dt_us = static_cast<int64_t>((now - last_frame_tick) * 1000000 / freq);
      PROFILE_FRAME_TIME_US(dt_us);
      PROFILE_FPS(freq / (now - last_frame_tick));
    }
    last_frame_tick = now;
  }
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  if (g_ngpu_swap_cb) g_ngpu_swap_cb(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  if (ngpu_pm4::Enabled()) ngpu_pm4::FrameEnd();

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;

  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  memory::store(memory_->TranslatePhysical(address), data_value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished || pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished || pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    if (id < 32) {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
    }
  }

  return true;
}

}  // namespace rex::graphics
