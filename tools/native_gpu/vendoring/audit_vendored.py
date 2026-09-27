#!/usr/bin/env python3
"""Audit: is every plugin-side (fork rexgpu-xenos) fix present in the native path's vendored copy?

Pairs each vendored file (src/native_gpu_xlat/**) with its fork counterpart (ng2-rexglue/src/**), normalises the
systematic vendoring differences (namespace renames, cvar accessor definitions, VENDORED/NATIVE PATCH comment
lines, include paths) and prints, per pair, the number of differing lines and each remaining hunk's first lines,
so a fix that landed on the plugin after the vendoring shows up as a hunk with real code in it.
Usage: audit_vendored.py [--full]   (--full prints whole hunks)"""
import difflib, os, re, sys

WT = r"D:\ng2_frameinterp\ng2recomp-worktree\src\native_gpu_xlat"
FORK = r"D:\ng2_frameinterp\ng2-rexglue\src"
PAIRS = {
    "rtc_d3d12/command_processor.cpp": "graphics/d3d12/command_processor.cpp",
    "rtc_d3d12/command_processor.h": "../include/rex/graphics/d3d12/command_processor.h",
    "rtc_d3d12/cp_base.h": "../include/rex/graphics/command_processor.h",
    "rtc_d3d12/primitive_processor_base.h": "../include/rex/graphics/primitive_processor.h",
    "rtc_d3d12/shared_memory_base.h": "../include/rex/graphics/shared_memory.h",
    "rtc_d3d12/texture_cache_base.h": "../include/rex/graphics/pipeline/texture/cache.h",
    "ucode.cpp": "graphics/format/ucode.cpp",
    "rtc_d3d12/texture_cache.cpp": "graphics/d3d12/texture_cache.cpp",
    "rtc_d3d12/texture_cache.h": "../include/rex/graphics/d3d12/texture_cache.h",
    "rtc_d3d12/render_target_cache.cpp": "graphics/d3d12/render_target_cache.cpp",
    "rtc_d3d12/render_target_cache.h": "../include/rex/graphics/d3d12/render_target_cache.h",
    "rtc_d3d12/pipeline_cache.cpp": "graphics/d3d12/pipeline_cache.cpp",
    "rtc_d3d12/pipeline_cache.h": "../include/rex/graphics/d3d12/pipeline_cache.h",
    "rtc_d3d12/primitive_processor.cpp": "graphics/d3d12/primitive_processor.cpp",
    "rtc_d3d12/primitive_processor.h": "../include/rex/graphics/d3d12/primitive_processor.h",
    "rtc_d3d12/shared_memory.cpp": "graphics/d3d12/shared_memory.cpp",
    "rtc_d3d12/shared_memory.h": "../include/rex/graphics/d3d12/shared_memory.h",
    "rtc_d3d12/shader.cpp": "graphics/d3d12/shader.cpp",
    "rtc_d3d12/shader.h": "../include/rex/graphics/d3d12/shader.h",
    "rtc_d3d12/deferred_command_list.cpp": "graphics/d3d12/deferred_command_list.cpp",
    "rtc_d3d12/texture_cache_base.cpp": "graphics/pipeline/texture/cache.cpp",
    "rtc_d3d12/shared_memory_base.cpp": "graphics/shared_memory.cpp",
    "rtc_d3d12/primitive_processor_base.cpp": "graphics/primitive_processor.cpp",
    "rtc_d3d12/cp_base.cpp": "graphics/command_processor.cpp",
    "rtc_d3d12/flags.cpp": "graphics/flags.cpp",
    "dxbc_translator.cpp": "graphics/pipeline/shader/dxbc_translator.cpp",
    "dxbc_translator_alu.cpp": "graphics/pipeline/shader/dxbc_translator_alu.cpp",
    "dxbc_translator_fetch.cpp": "graphics/pipeline/shader/dxbc_translator_fetch.cpp",
    "dxbc_translator_om.cpp": "graphics/pipeline/shader/dxbc_translator_om.cpp",
    "dxbc_translator_memexport.cpp": "graphics/pipeline/shader/dxbc_translator_memexport.cpp",
    "shader.cpp": "graphics/pipeline/shader/shader.cpp",
    "translator.cpp": "graphics/pipeline/shader/translator.cpp",
    "translator_disasm.cpp": "graphics/pipeline/shader/translator_disasm.cpp",
        "draw_util_vendored.cpp": "graphics/util/draw.cpp",
    "draw_extent_estimator_vendored.cpp": "graphics/util/draw_extent_estimator.cpp",
    "rt_cache_vendored.cpp": "graphics/pipeline/render_target/cache.cpp",
    "shader_interpreter_vendored.cpp": "graphics/pipeline/shader/interpreter.cpp",
    "register_file.cpp": "graphics/register_file.cpp",
    "registers.cpp": "graphics/registers.cpp",
}
NOISE = [
    (r"rex::graphics::ngpu_d3d12", "NS"), (r"rex::graphics::d3d12", "NS"), (r"ngpu_d3d12::", "NS::"), (r"\bd3d12::", "NS::"),
    (r"ngpu_d3d12", "NS"), (r"ui::NS", "NS"), (r"ui::d3d12", "NS"),
    (r"::ng2::ngpu::xlat::Plugin\w+\([^)]*\)", "PB"), (r"::ng2::ngpu::xlat::Refresh\w+\([^)]*\);", ""),
    (r'#include "rtc_d3d12/', '#include "'), (r"#include <rex/graphics/", "#include <"), (r'#include "graphics/d3d12/', '#include "'), (r'#include <rex/graphics/d3d12/', '#include <'),
    (r"\bfable2::ngpu\b", "NS2"), (r"\bng2::ngpu\b", "NS2"),
]
SKIP_LINE = re.compile(r"VENDORED|NATIVE PATCH|REXCVAR_DEFINE_|FLAGS_\w+_storage_\(\)\s*\{|^\s*//|^\s*$|namespace NS2::xlat|^#include")


def norm(path):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        l = line.rstrip("\r\n")
        for a, b in NOISE:
            l = re.sub(a, b, l)
        if SKIP_LINE.search(l):
            continue
        out.append(l.strip())
    return out


full = "--full" in sys.argv
total = 0
for v, f in PAIRS.items():
    vp, fp = os.path.join(WT, v), os.path.join(FORK, f)
    if not os.path.exists(vp) or not os.path.exists(fp):
        print("%-46s %s" % (v, "MISSING FILE: " + ("vendored" if not os.path.exists(vp) else "fork " + f)))
        continue
    a, b = norm(vp), norm(fp)
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    hunks = [op for op in sm.get_opcodes() if op[0] != "equal"]
    diff_lines = sum((i2 - i1) + (j2 - j1) for _, i1, i2, j1, j2 in hunks)
    total += diff_lines
    print("%-46s vs %-48s %4d hunks %5d lines" % (v, f, len(hunks), diff_lines))
    for tag, i1, i2, j1, j2 in hunks:
        va, fb = a[i1:i2], b[j1:j2]
        if full:
            for l in va: print("      - " + l[:150])
            for l in fb: print("      + " + l[:150])
        else:
            print("    [%s] vendored %d lines, fork %d lines: -%s | +%s" % (tag, len(va), len(fb), (va[0][:70] if va else ""), (fb[0][:70] if fb else "")))
print("total differing lines (normalised): %d" % total)
