p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
anchor = '''     "class D3D12CommandProcessor : public CommandProcessor {\\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\\n public:", 1),
]'''
assert s.count(anchor) == 1, s.count(anchor)
if "RenderDoc detection" not in s:
    s = s.replace(anchor, '''     "class D3D12CommandProcessor : public CommandProcessor {\\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\\n public:", 1),
    # The base class's gamma tables are private; the driver copies the plugin's tables in at each swap.
    ("cp_base.h", "class CommandProcessor {\\n public:",
     "class CommandProcessor {\\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\\n public:", 1),
    ("cp_base.h", "namespace rex::graphics {\\n\\nclass GraphicsSystem;",
     "namespace fable2::ngpu::backend { class Driver; }   // NATIVE PATCH\\n\\nnamespace rex::graphics {\\n\\nclass GraphicsSystem;", 1),
    # RenderDoc detection (debug-marker auto-enable) needs renderdoc_app.h, which the native build does not carry:
    # markers follow the plugin's gpu_debug_markers flag only.
    ("flags.cpp", "#include <rex/ui/renderdoc_api.h>\\n", "// NATIVE PATCH: <rex/ui/renderdoc_api.h> not included (no RenderDoc detection)\\n", 1),
    ("flags.cpp", """    } else {
      auto renderdoc_api = rex::ui::RenderDocAPI::CreateIfConnected();
      if (renderdoc_api) {
        result = true;
        REXLOG_INFO("GPU debug markers auto-enabled (RenderDoc detected)");
      }
    }""", "    }   // NATIVE PATCH: no RenderDoc detection", 1),
]''')
open(p, "w", encoding="utf-8").write(s)
d = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_backend.cpp"
t = open(d, encoding="utf-8").read()
for r in ["XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0", "XE_GPU_REG_VGT_DMA_BASE", "XE_GPU_REG_VGT_DMA_SIZE", "XE_GPU_REG_VGT_DRAW_INITIATOR"]:
    t = t.replace("(" + r, "(rex::graphics::" + r).replace(" " + r + " ", " rex::graphics::" + r + " ")
open(d, "w", encoding="utf-8").write(t)
print("ok")
