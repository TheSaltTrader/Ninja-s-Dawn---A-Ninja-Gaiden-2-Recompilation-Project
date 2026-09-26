import re
R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/rexglue-src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:60], n)
    s = s.replace(old, new)
    open(R + p, 'w', encoding='utf-8').write(s)

# header: the bitmap + markers
patch('include/rex/graphics/command_processor.h', 'class CommandProcessor {',
'''// NATIVE LOCKSTEP DIRTY BITMAP (2026-09-26): one bit per register, set on every register-file write the command
// processor makes; the in-app native backend takes and clears it inside the draw callback (same thread), so it no
// longer diffs 3,368 registers per draw (12% of the GPU thread in town, PROFT2). Exported as RexNgpuDirtyRegs.
extern uint64_t g_ngpu_dirty_regs[(0x5003 + 63) / 64];
inline void NgpuMarkDirty(uint32_t index) { g_ngpu_dirty_regs[index >> 6] |= uint64_t(1) << (index & 63); }
inline void NgpuMarkDirtyRange(uint32_t start, uint32_t count) {
  for (uint32_t i = start, e = start + count; i < e;) {
    const uint32_t bit = i & 63, n = (e - i) < (64 - bit) ? (e - i) : (64 - bit);
    g_ngpu_dirty_regs[i >> 6] |= (n == 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1)) << bit;
    i += n;
  }
}

class CommandProcessor {''')

# base: definition, export, marks
patch('src/graphics/command_processor.cpp', '''static bool g_ngpu_swap_gamma_valid = false;''',
'''__declspec(dllexport) uint64_t* RexNgpuDirtyRegs(uint32_t* word_count) {
  if (word_count) *word_count = uint32_t(sizeof(g_ngpu_dirty_regs) / sizeof(g_ngpu_dirty_regs[0]));
  return g_ngpu_dirty_regs;
}
static bool g_ngpu_swap_gamma_valid = false;''')
patch('src/graphics/command_processor.cpp', '''void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {''',
'''uint64_t g_ngpu_dirty_regs[(0x5003 + 63) / 64];

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {''')
patch('src/graphics/command_processor.cpp', '''  const_cast<volatile uint32_t&>(regs.values[index]) = value;
''', '''  const_cast<volatile uint32_t&>(regs.values[index]) = value;
  NgpuMarkDirty(index);
''')
patch('src/graphics/command_processor.cpp', '''  // The register-table lookup only feeds a debug message; it cost ~2% of the GPU thread per write (PROFT1).
  if (auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
      lp && lp->should_log(spdlog::level::debug) && !regs.GetRegisterInfo(index)) {''',
'''  // The register-table lookup only feeds a debug message; it cost ~2% of the GPU thread per write (PROFT1).
  // The level is read once (the cross-module logger lookup per write was itself 0.6%, PROFT2).
  static const bool gpu_debug = [] {
    auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
    return lp && lp->should_log(spdlog::level::debug);
  }();
  if (gpu_debug && !regs.GetRegisterInfo(index)) {''')
patch('src/graphics/command_processor.cpp', '''      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);''',
'''      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
      NgpuMarkDirty(XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0);
    } else {
      register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
      NgpuMarkDirty(XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);''')
# COHER |= write happens right after the store (already marked by the store above).

# d3d12 fast paths: mark, and in offload skip the invalidation work
p = 'src/graphics/d3d12/command_processor.cpp'
s = open(R + p, encoding='utf-8').read()
old = 'memory::copy_and_swap(register_file_->values + start_index, base, num_registers);'
assert s.count(old) == 3
s = s.replace(old, old + '\n    NgpuMarkDirtyRange(start_index, num_registers);')
old2 = '''  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage'''
assert s.count(old2) == 1
s = s.replace(old2, '''  uint32_t end_index = start_index + num_registers - 1;

  // Offloaded: constants only need to land in the register file (and the dirty bitmap); this backend's binding and
  // texture invalidation is dead work. Other ranges keep the per-register path (scratch, COHER, gamma LUT).
  if (OffloadedToNative() && start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    NgpuMarkDirtyRange(start_index, num_registers);
    return;
  }

  auto range_has_any_constant_usage''')
open(R + p, 'w', encoding='utf-8').write(s)
print('ok')
