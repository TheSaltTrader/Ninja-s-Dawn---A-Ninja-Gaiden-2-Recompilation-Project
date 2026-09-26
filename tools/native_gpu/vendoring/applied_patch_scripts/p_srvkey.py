P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
def rep(a, b):
    global s
    a2 = a.replace('\n', nl); b2 = b.replace('\n', nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2, 1)
rep("""    static std::unordered_map<std::string, uint32_t> runs;
    if (dedupe_frame != g_s.frames) { dedupe_frame = g_s.frames; runs.clear(); }
    std::string key;
    key.reserve(specs.size() * 48);""",
"""    // Keyed by a 64-bit hash of the fields (PROF3: hashing a std::string key, plus its allocation, was ~5% of the
    // async replay thread); the byte string is built in a reused buffer.
    static std::unordered_map<uint64_t, uint32_t> runs;
    if (dedupe_frame != g_s.frames) { dedupe_frame = g_s.frames; runs.clear(); }
    static std::string key;
    key.clear();""")
rep("""    if (auto it = runs.find(key); it != runs.end()) { vfirst = it->second; ++g_st.desc_reused; }""",
"""    const uint64_t khash = XXH3_64bits(key.data(), key.size());
    if (auto it = runs.find(khash); it != runs.end()) { vfirst = it->second; ++g_st.desc_reused; }""")
rep("""      runs.emplace(std::move(key), vfirst);""", """      runs.emplace(khash, vfirst);""")
open(P, 'w', encoding='utf-8', newline='').write(s)
print("ok")
