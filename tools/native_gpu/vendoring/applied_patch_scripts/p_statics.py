R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))

# Per-write magic statics -> plain globals read once (benign race: every thread computes the same value).
for p in ['wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/cp_base.cpp', 'rexglue-src/src/graphics/command_processor.cpp']:
    patch(p, '''bool RegsEnabled() {
  static const bool on = std::getenv("NGPU_PM4_REGS") != nullptr;
  return on;
}''', '''// Read on every register write: a function-local static would pay the thread-safe-init guard each call
// (PROFT5 line profile). A plain global, computed once; a benign race computes the same value.
static int8_t g_regs_enabled = -1;
bool RegsEnabled() {
  if (g_regs_enabled < 0) g_regs_enabled = std::getenv("NGPU_PM4_REGS") != nullptr ? 1 : 0;
  return g_regs_enabled != 0;
}''')
    patch(p, '''  static const bool gpu_debug = [] {
    auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
    return lp && lp->should_log(spdlog::level::debug);
  }();
  if (gpu_debug && !regs.GetRegisterInfo(index)) {''', '''  // A plain static (not a guarded local): see RegsEnabled.
  static int8_t gpu_debug = -1;
  if (gpu_debug < 0) {
    auto* lp = ::rex::GetLoggerRaw(::rex::log::gpu());
    gpu_debug = (lp && lp->should_log(spdlog::level::debug)) ? 1 : 0;
  }
  if (gpu_debug > 0 && !regs.GetRegisterInfo(index)) {''')

# A locked exchange on every draw -> a plain load first.
patch('wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/texture_cache_base.cpp',
      '''  if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {''',
      '''  // NATIVE PATCH (PROFT5): a relaxed load first - the locked exchange on every draw cost ~1% of the GPU thread.
  if (texture_became_outdated_.load(std::memory_order_relaxed) &&
      texture_became_outdated_.exchange(false, std::memory_order_acquire)) {''')

# Plugin: mark-on-change constant copy, 4 registers at a time (SSE2, the x64 baseline).
patch('rexglue-src/include/rex/graphics/command_processor.h', '''inline void NgpuCopySwapMarkChanged(uint32_t* values, uint32_t start, const uint32_t* src_be, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {''', '''inline void NgpuCopySwapMarkChanged(uint32_t* values, uint32_t start, const uint32_t* src_be, uint32_t count) {
  // Four at a time (SSE2): byte-swap by 8/16-bit shifts, compare with the register file, and only a block with a
  // difference is stored and marked lane by lane (PROFT5: the scalar loop was 3.7% of the GPU thread).
  uint32_t i = 0;
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_be + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));          // swap bytes within 16-bit halves
    v = _mm_shufflelo_epi16(_mm_shufflehi_epi16(v, 0xB1), 0xB1);             // swap the halves of each dword
    uint32_t* dst = values + start + i;
    const int eq = _mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst))));
    if (eq == 0xFFFF) continue;
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
    for (uint32_t k = 0; k < 4; ++k)
      if (((eq >> (k * 4)) & 0xF) != 0xF) NgpuMarkDirty(start + i + k);
  }
  for (; i < count; ++i) {''')
s = open(R + 'rexglue-src/include/rex/graphics/command_processor.h', encoding='utf-8').read()
if '<emmintrin.h>' not in s:
    s = s.replace('extern uint64_t g_ngpu_dirty_regs', '}  // namespace rex::graphics\n#include <emmintrin.h>\nnamespace rex::graphics {\nextern uint64_t g_ngpu_dirty_regs', 1) if False else s
    i = s.find('#include')
    s = s[:i] + '#include <emmintrin.h>\n' + s[i:]
    open(R + 'rexglue-src/include/rex/graphics/command_processor.h', 'w', encoding='utf-8').write(s)
print('ok')
