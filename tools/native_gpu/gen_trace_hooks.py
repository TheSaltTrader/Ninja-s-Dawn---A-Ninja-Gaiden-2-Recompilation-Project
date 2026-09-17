"""M4 step 1b: generate the call-census tracer for a game's Direct3D surface.

Inputs: the M3 JSON (library entry points = `api` + `vd_callers` + the
in-library PM4 emitters) and the live device-table dump from scan_tables.py
(the SetRenderState / SetSamplerState dispatch tables at device+0x40 and
device+0x1D4). Outputs, for the game project:

  config/hooks/native_gpu_trace.toml   one [[midasm_hook]] per function, at its
                                       first instruction, passing r3..r10
  src/native_gpu_trace.cpp             the hook bodies: a per-function call
                                       counter, the first argument samples,
                                       and a census line every 10 s in the log

The hooks change nothing (no return/jump), so the game runs as before; the
census tells which entry points a frame really uses and what they receive.

usage: gen_trace_hooks.py <M3 json> <tables.txt> <table guest addr hex> <game_dir> [--ring <ringcallers.json>] [--dirty <dirty.json>] [--dump]
--dump: the draw/shader/constant/render-target/present hooks also call the
hand-written draw dump (src/native_gpu_dump.cpp, DUMP table below).
--dirty: also hook the shadow-state setters dirty_setters.py found (functions
that set bits in the device's 64-bit dirty mask at +0x10 without writing PM4).
--ring: also hook every direct caller of the ring-buffer make-space helper
(M4_<game>_ringcallers.json, from the census notes) - the PM4 writers the
address-cluster definition of the library missed (Fable: 78 functions in five
clusters, the real draw entry points among them).
"""
import json, re, sys

m3, tables_txt, table_addr, game = sys.argv[1:5]
# --frame-marker <hex>: the entry point that fires once per frame (for NG2, the
# VdSwap caller). It turns the census from cumulative-per-10s, which flattens a
# 12-per-frame call and a 2000-per-frame call into the same kind of thing, into
# per-frame counts plus one verbatim ordered transcript of a single frame. The
# pass structure of a frame is what explains where every render target comes
# from; a histogram cannot show it.
frame_marker = 0
if '--frame-marker' in sys.argv:
    frame_marker = int(sys.argv[sys.argv.index('--frame-marker') + 1], 16)
d = json.load(open(m3))
lib = [int(x[4:], 16) for x in d["library"]]
lib_lo, lib_hi = min(lib), max(lib)

# Xbox 360 D3DRENDERSTATETYPE: value = 4 * index. Reconstructed from the XDK
# header (the values UnleashedRecomp uses - ZENABLE 40 .. ALPHAREF 100,
# SCISSORTESTENABLE 200 .. COLORWRITEENABLE 212 - anchor it); entries past
# COLORWRITEENABLE3 are the XDK's order and may be off by a slot.
D3DRS = ("ZENABLE ZFUNC ZWRITEENABLE FILLMODE CULLMODE ALPHABLENDENABLE "
         "SEPARATEALPHABLENDENABLE BLENDFACTOR SRCBLEND DESTBLEND BLENDOP SRCBLENDALPHA "
         "DESTBLENDALPHA BLENDOPALPHA ALPHATESTENABLE ALPHAREF ALPHAFUNC STENCILENABLE "
         "TWOSIDEDSTENCILMODE STENCILFAIL STENCILZFAIL STENCILPASS STENCILFUNC STENCILREF "
         "STENCILMASK STENCILWRITEMASK CCW_STENCILFAIL CCW_STENCILZFAIL CCW_STENCILPASS "
         "CCW_STENCILFUNC CCW_STENCILREF CCW_STENCILMASK CCW_STENCILWRITEMASK CLIPPLANEENABLE "
         "POINTSIZE POINTSIZE_MIN POINTSPRITEENABLE POINTSIZE_MAX MULTISAMPLEANTIALIAS "
         "MULTISAMPLEMASK SCISSORTESTENABLE SLOPESCALEDEPTHBIAS DEPTHBIAS COLORWRITEENABLE "
         "COLORWRITEENABLE1 COLORWRITEENABLE2 COLORWRITEENABLE3 TESSELLATIONMODE "
         "MINTESSELLATIONLEVEL MAXTESSELLATIONLEVEL WRAP0 WRAP1 WRAP2 WRAP3 WRAP4 WRAP5 WRAP6 "
         "WRAP7 WRAP8 WRAP9 WRAP10 WRAP11 WRAP12 WRAP13 WRAP14 WRAP15 VIEWPORTENABLE "
         "HIGHPRECISIONBLENDENABLE HIGHPRECISIONBLENDENABLE1 HIGHPRECISIONBLENDENABLE2 "
         "HIGHPRECISIONBLENDENABLE3 HALFPIXELOFFSET PRIMITIVERESETENABLE PRIMITIVERESETINDEX "
         "ALPHATOMASKENABLE ALPHATOMASKOFFSETS GUARDBAND_X GUARDBAND_Y DISCARDBAND_X "
         "DISCARDBAND_Y HISTENCILENABLE HISTENCILWRITEENABLE HISTENCILFUNC HISTENCILREF "
         "PRESENTINTERVAL PRESENTIMMEDIATETHRESHOLD HIZENABLE HIZWRITEENABLE LASTPIXEL "
         "LINEWIDTH BUFFER2FRAMES").split()
assert len(D3DRS) == 91, len(D3DRS)  # indices 10..100
D3DSAMP = ("ADDRESSU ADDRESSV ADDRESSW BORDERCOLOR MAGFILTER MINFILTER MIPFILTER MIPMAPLODBIAS "
           "MAXMIPLEVEL MAXANISOTROPY MAGFILTERZ MINFILTERZ SEPARATEZFILTERENABLE MINMIPLEVEL "
           "TRILINEARTHRESHOLD ANISOTROPYBIAS HGRADIENTEXPBIAS VGRADIENTEXPBIAS "
           "WHITEBORDERCOLORW POINTBORDERENABLE").split()
assert len(D3DSAMP) == 20

# --- the dispatch tables from the live dump ---
tbl = int(table_addr, 16)
entries = {}
inside = False
for line in open(tables_txt):
    if line.startswith("stride 4: table at guest 0x%08X" % tbl):
        inside = True; continue
    if inside:
        m = re.match(r"\s+\[\s*(\d+)\] 0x([0-9A-F]{8}) -> sub_([0-9A-F]{8})", line)
        if not m:
            if line.startswith("stride"): break
            continue
        entries[int(m.group(1))] = int(m.group(3), 16)
labels = {}  # addr -> label
def add(addr, label):
    labels.setdefault(addr, label)
for i in range(0, 101):
    a = entries.get(i)
    if a is None: continue
    name = "D3DRS_" + (D3DRS[i - 10] if i >= 10 else f"invalid{i}")
    add(a, f"SetRenderState_{name[6:]}" if i >= 10 else "SetRenderState_INVALID")
for i in range(0, 20):
    a = entries.get(101 + i)
    if a is None: continue
    add(a, f"SetSamplerState_{D3DSAMP[i]}")
n_state = len(labels)

# --- the library surface ---
surface = set(d["api"]) | set(d["vd_callers"])
for fn, ops in d["emitters"].items():
    if lib_lo <= int(fn[4:], 16) <= lib_hi:
        surface.add(fn)
for fn in sorted(surface):
    a = int(fn[4:], 16)
    sites = len(d["api"].get(fn, []))
    ops = ",".join(sorted({o for o, _ in d["emitters"].get(fn, [])}))
    imps = ",".join(x[7:] for x in d["vd_callers"].get(fn, []))
    tag = " ".join(t for t in (f"sites={sites}", ops, imps) if t)
    add(a, f"lib {tag}")

if "--ring" in sys.argv:
    rj = json.load(open(sys.argv[sys.argv.index("--ring") + 1]))
    for fn in rj["direct"]:
        add(int(fn[4:], 16), "ring")
if "--dirty" in sys.argv:
    dj = json.load(open(sys.argv[sys.argv.index("--dirty") + 1]))
    for fn, info in dj["setters"].items():
        add(int(fn[4:], 16), "dirty bits=" + ",".join(str(b) for b in info["bits"]) + " writes=" + ",".join(str(w) for w in info["writes"][:6]))
items = sorted(labels.items())
print(f"{len(items)} hooks: {n_state} state setters + {len(items) - n_state} library entry points")

regs = ["r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10"]
toml = ["# GENERATED by ng2recomp/tools/native_gpu/gen_trace_hooks.py - the native-GPU",
        "# call-census tracer: one hook at the first instruction of every Direct3D",
        "# library entry point and device state setter. The hooks only count and",
        "# sample arguments (see src/native_gpu_trace.cpp); nothing changes.", ""]
for a, label in items:
    toml += ["[[midasm_hook]]", f"address = 0x{a:08X}", f'name = "ngpu_{a:08X}"',
             "registers = [" + ", ".join(f'"{r}"' for r in regs) + "]", f"# {label}", ""]
open(f"{game}/config/hooks/native_gpu_trace.toml", "w").write("\n".join(toml))

cpp = ['// GENERATED by ng2recomp/tools/native_gpu/gen_trace_hooks.py. The native-GPU',
       '// call-census tracer: every Direct3D library entry point and device state',
       '// setter has a mid-asm hook at its first instruction that counts the call and',
       '// keeps the first argument samples; a census goes to the log every 10 s',
       '// ([ngpu] lines). Enabled by the ngpu_trace cvar (default off: the hooks',
       '// cost one load and one compare each when off).',
       '#include <rex/cvar.h>',
       '#include <rex/logging.h>',
       '#include <rex/ppc/context.h>',
       '',
       '#include <atomic>',
       '#include <chrono>',
       '#include <cstdint>',
       '#include <mutex>',
       '',
       'REXCVAR_DEFINE_BOOL(ngpu_trace, false, "GPU", "Log a call census of the Direct3D library entry points every 10 s");',
       'REXCVAR_DEFINE_INT32(ngpu_trace_frame, 600, "GPU", "Frame after which the one verbatim frame transcript is dumped - set past the menus so it captures a real 3D frame, not the title");',
       '',
       'namespace {',
       f'constexpr int kHooks = {len(items)};',
       'constexpr int kSamples = 4;',
       'struct Entry { uint32_t addr; const char* label; };',
       'const Entry kEntries[kHooks] = {']
for a, label in items:
    cpp.append(f'  {{0x{a:08X}, "{label}"}},')
cpp += ['};',
        'std::atomic<uint64_t> g_count[kHooks];',
        'uint64_t g_last[kHooks];',
        'std::atomic<int> g_nsamples[kHooks];',
        'uint32_t g_samples[kHooks][kSamples][8];',
        'std::atomic<uint32_t> g_tick{0};',
        f'constexpr uint32_t kFrameMarkerAddr = 0x{frame_marker:08X};',
        'constexpr int kSeqMax = 8192;',
        'uint16_t g_seq[kSeqMax];',
        'int g_seq_n = 0;',
        'bool g_seq_done = false;',
        'uint64_t g_frame_no = 0;',
        'uint64_t g_frame_base[kHooks];',
        'std::mutex g_dump_mutex;',
        'auto g_t0 = std::chrono::steady_clock::now();',
        'double g_next_dump = 10.0;',
        '',
        'void Dump() {',
        '  std::lock_guard<std::mutex> lock(g_dump_mutex);',
        '  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();',
        '  if (t < g_next_dump) return;',
        '  g_next_dump = t + 10.0;',
        '  REXLOG_INFO("[ngpu] census at {:.1f} s (calls total / last 10 s)", t);',
        '  for (int i = 0; i < kHooks; ++i) {',
        '    const uint64_t c = g_count[i].load(std::memory_order_relaxed);',
        '    if (c == 0) continue;',
        '    const uint64_t d = c - g_last[i];',
        '    g_last[i] = c;',
        '    std::string s;',
        '    const int ns = g_nsamples[i].load(std::memory_order_acquire);',
        '    if (ns > 0 && ns <= kSamples) {',
        '      for (int k = 0; k < ns; ++k) {',
        '        const uint32_t* a = g_samples[i][k];',
        '        s += fmt::format(" [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",',
        '                         a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);',
        '      }',
        '      g_nsamples[i].store(kSamples + 1, std::memory_order_release);  // printed once',
        '    }',
        '    REXLOG_INFO("[ngpu] sub_{:08X} {} calls={} +{}{}", kEntries[i].addr, kEntries[i].label, c, d, s);',
        '  }',
        '}',
        '',
        'void FrameEnd() {',
        '  std::lock_guard<std::mutex> lock(g_dump_mutex);',
        '  ++g_frame_no;',
        '  // One transcript, once, after warmup - the title screen is not the frame',
        '  // worth recording and the first frames are atypical.',
        '  if (!g_seq_done && g_frame_no > REXCVAR_GET(ngpu_trace_frame) && g_seq_n > 0) {',
        '    g_seq_done = true;',
        '    REXLOG_INFO("[ngpu] FRAME TRANSCRIPT, frame {}, {} calls in order", g_frame_no, g_seq_n);',
        '    // Run-length encoded: order is preserved exactly, repeats collapse so',
        '    // the log stays readable. "Ax12" means twelve consecutive calls to A.',
        '    std::string line;',
        '    int run = 1;',
        '    for (int k = 1; k <= g_seq_n; ++k) {',
        '      if (k < g_seq_n && g_seq[k] == g_seq[k - 1]) { ++run; continue; }',
        '      const uint32_t a = kEntries[g_seq[k - 1]].addr;',
        '      line += run > 1 ? fmt::format(" {:08X}x{}", a, run) : fmt::format(" {:08X}", a);',
        '      run = 1;',
        '      if (line.size() > 900) { REXLOG_INFO("[ngpu] seq{}", line); line.clear(); }',
        '    }',
        '    if (!line.empty()) REXLOG_INFO("[ngpu] seq{}", line);',
        '    REXLOG_INFO("[ngpu] PER-FRAME counts for that frame:");',
        '    for (int i = 0; i < kHooks; ++i) {',
        '      const uint64_t c = g_count[i].load(std::memory_order_relaxed);',
        '      const uint64_t d = c - g_frame_base[i];',
        '      if (d) REXLOG_INFO("[ngpu] frame sub_{:08X} {} x{}", kEntries[i].addr, kEntries[i].label, d);',
        '    }',
        '  }',
        '  for (int i = 0; i < kHooks; ++i) g_frame_base[i] = g_count[i].load(std::memory_order_relaxed);',
        '  g_seq_n = 0;',
        '}',
        '',
        'inline void Trace(int i, PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6,',
        '                  PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) {',
        '  if (!REXCVAR_GET(ngpu_trace)) return;',
        '  g_count[i].fetch_add(1, std::memory_order_relaxed);',
        '  int ns = g_nsamples[i].load(std::memory_order_relaxed);',
        '  if (ns < kSamples) {',
        '    uint32_t* a = g_samples[i][ns];',
        '    a[0] = r3.u32; a[1] = r4.u32; a[2] = r5.u32; a[3] = r6.u32;',
        '    a[4] = r7.u32; a[5] = r8.u32; a[6] = r9.u32; a[7] = r10.u32;',
        '    g_nsamples[i].store(ns + 1, std::memory_order_release);',
        '  }',
        '  if (!g_seq_done) {',
        '    std::lock_guard<std::mutex> lock(g_dump_mutex);',
        '    if (g_seq_n < kSeqMax) g_seq[g_seq_n++] = static_cast<uint16_t>(i);',
        '  }',
        '  if (kFrameMarkerAddr && kEntries[i].addr == kFrameMarkerAddr) FrameEnd();',
        '  if ((g_tick.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) Dump();',
        '}',
        '}  // namespace',
        '']
# --dump: these entry points also feed the hand-written draw dump
# (src/native_gpu_dump.cpp) - the names are Fable II TU1's; other games pass
# their own table through --dump-map <json> {addr_hex: callback}.
DUMP = {
    0x8221DFC0: "ngpu::OnDrawIndexed(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32)",
    0x8221C3E8: "ngpu::OnDrawVertices(r3.u32, r4.u32, r5.u32, r6.u32)",
    0x82217DB8: "ngpu::OnDrawUP(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32)",
    0x82221858: "ngpu::OnSetShader(r3.u32, r4.u32, r5.u32)",
    0x82232510: "ngpu::OnSetShader(r3.u32, r4.u32, 0)",   # SetVertexShader (device+0x3198)
    0x82208BB0: "ngpu::OnSetShader(r3.u32, r4.u32, 0)",   # SetPixelShader (device+0x3194)
    0x82221B90: "ngpu::OnLoadConstants(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32)",
    0x822192E8: "ngpu::OnSetRenderTarget(r3.u32, r4.u32, r5.u32)",
    0x82BA34D8: "ngpu::OnPresent(r3.u32)",
    0x82196750: "ngpu::OnResolve(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32)",
    0x82206888: "ngpu::OnResolveXdk(r3.u32, r4.u32, r5.u32, r6.u32, r7.u32, r8.u32, r9.u32, r10.u32)",  # the XDK D3DDevice_Resolve; 0x82196750 is Fable's wrapper
}
if "--dump-map" in sys.argv:
    DUMP = {int(k, 16): v for k, v in json.load(open(sys.argv[sys.argv.index("--dump-map") + 1])).items()}
dump = "--dump" in sys.argv
if dump:
    cpp.insert(cpp.index('#include <rex/cvar.h>'), '#include "native_gpu_dump.h"')
    for a in DUMP:
        if a not in labels:
            items.append((a, "dump"))
    items.sort()
    cpp[cpp.index(next(l for l in cpp if l.startswith("constexpr int kHooks")))] = f"constexpr int kHooks = {len(items)};"
    # the entry table must match the (possibly grown) item list
    start = cpp.index("const Entry kEntries[kHooks] = {") + 1
    end = cpp.index("};", start)
    cpp[start:end] = [f'  {{0x{a:08X}, "{label}"}},' for a, label in items]
    toml = toml[:5]
    for a, label in items:
        toml += ["[[midasm_hook]]", f"address = 0x{a:08X}", f'name = "ngpu_{a:08X}"',
                 "registers = [" + ", ".join(f'"{r}"' for r in regs) + "]", f"# {label}", ""]
    open(f"{game}/config/hooks/native_gpu_trace.toml", "w").write("\n".join(toml))
for i, (a, label) in enumerate(items):
    extra = (" " + DUMP[a] + ";") if dump and a in DUMP else ""
    cpp.append(f'void ngpu_{a:08X}(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5, PPCRegister& r6, '
               f'PPCRegister& r7, PPCRegister& r8, PPCRegister& r9, PPCRegister& r10) '
               f'{{ Trace({i}, r3, r4, r5, r6, r7, r8, r9, r10);{extra} }}')
open(f"{game}/src/native_gpu_trace.cpp", "w").write("\n".join(cpp) + "\n")
print("wrote", f"{game}/config/hooks/native_gpu_trace.toml", f"{game}/src/native_gpu_trace.cpp")
