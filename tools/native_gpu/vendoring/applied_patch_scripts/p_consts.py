import re
P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
a = "  fable2::ngpu::sdk::DrawConstants dc;" + nl + "  uint32_t index_endian = auto_index"
assert s.count(a) == 1
s = s.replace(a, "  // Reused across draws (PROF4: its vectors were allocated and freed per draw); BuildDrawConstants refills every field." + nl +
              "  static fable2::ngpu::sdk::DrawConstants dc;" + nl + "  uint32_t index_endian = auto_index")
open(P, 'w', encoding='utf-8', newline='').write(s)
C = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_sdk_consts.cpp"
c = open(C, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in c else '\n'
old = """  auto pack = [&](const StageInfo& s, uint32_t base, std::vector<uint32_t>& dst) {
    dst.clear();
    for (uint32_t c = 0; c < 256; ++c) {
      if (!s.float_dynamic && !((s.float_bitmap[c >> 6] >> (c & 63)) & 1)) continue;
      for (uint32_t k = 0; k < 4; ++k) dst.push_back(regs[base + c * 4 + k]);
    }
    if (dst.empty()) dst.assign(4, 0);   // at least 16 bytes, as the plugin allocates
  };""".replace('\n', nl)
new = """  // Sized once and filled by set bit (PROF4: a push_back per dword was ~10% of the async replay thread).
  auto pack = [&](const StageInfo& s, uint32_t base, std::vector<uint32_t>& dst) {
    if (s.float_dynamic) { dst.assign(&regs.values[base], &regs.values[base] + 256 * 4); return; }
    uint32_t n = 0;
    for (int w = 0; w < 4; ++w) n += uint32_t(std::popcount(s.float_bitmap[w]));
    if (!n) { dst.assign(4, 0); return; }   // at least 16 bytes, as the plugin allocates
    dst.resize(size_t(n) * 4);
    uint32_t* o = dst.data();
    for (int w = 0; w < 4; ++w)
      for (uint64_t bits = s.float_bitmap[w]; bits; bits &= bits - 1) {
        const uint32_t cidx = uint32_t(w) * 64 + uint32_t(std::countr_zero(bits));
        std::memcpy(o, &regs.values[base + cidx * 4], 16); o += 4;
      }
  };""".replace('\n', nl)
assert c.count(old) == 1
c = c.replace(old, new)
if '#include <bit>' not in c:
    c = c.replace('#include <cstring>', '#include <bit>' + nl + '#include <cstring>', 1)
    assert '#include <bit>' in c, "no cstring include to anchor"
open(C, 'w', encoding='utf-8', newline='').write(c)
print("ok")
