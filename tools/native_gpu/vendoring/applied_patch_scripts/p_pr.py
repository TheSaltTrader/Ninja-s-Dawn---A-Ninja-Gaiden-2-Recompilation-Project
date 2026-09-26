P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
old = """  if (page >= 0 && (seen[size_t(page) >> 6] >> (page & 63)) & 1) return true;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
  if (ok && page >= 0) seen[size_t(page) >> 6] |= uint64_t(1) << (page & 63);
  return ok;
}""".replace('\n', nl)
new = """  if (page >= 0 && (seen[size_t(page) >> 6] >> (page & 63)) & 1) return true;
  // NEGATIVE ANSWERS, REMEMBERED BRIEFLY (2026-09-26, PROF6 xperf: at the load->gameplay transition 13% of the
  // replay thread was VirtualQuery on pages that had ALREADY answered "not readable" - only yes was cached). A "no"
  // holds for 100 ms, so memory committed later is still seen within a frame or two.
  static std::unique_ptr<std::atomic<uint32_t>[]> neg(new std::atomic<uint32_t>[(4096u + 512u) * 256]());
  const uint32_t now_ms = uint32_t(GetTickCount64());
  if (page >= 0) { const uint32_t t = neg[size_t(page)].load(std::memory_order_relaxed); if (t && now_ms - t < 100u) return false; }
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(host, &mbi, sizeof(mbi))) return false;
  const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
  if (page >= 0) {
    // The answer holds for the whole region VirtualQuery describes (same state and protection throughout), so every
    // page of it in the same guest range is recorded at once, capped at 64K pages per call.
    const uint8_t* range_end = page < int64_t(4096u * 256) ? vb + (uint64_t(4096) << 20) : pb + (uint64_t(512) << 20);
    const uint8_t* rend = std::min(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize, range_end);
    const int64_t pages = std::min<int64_t>(int64_t((rend - h + 0xFFF) >> 12), 65536);
    for (int64_t q = page; q < page + std::max<int64_t>(pages, 1); ++q) {
      if (ok) seen[size_t(q) >> 6] |= uint64_t(1) << (q & 63);
      else neg[size_t(q)].store(now_ms ? now_ms : 1u, std::memory_order_relaxed);
    }
  }
  return ok;
}""".replace('\n', nl)
assert s.count(old) == 1
s = s.replace(old, new)
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
