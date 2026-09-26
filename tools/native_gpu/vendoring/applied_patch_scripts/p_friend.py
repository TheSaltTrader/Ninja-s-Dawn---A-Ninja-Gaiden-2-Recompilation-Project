p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
anchor = '''     "memory::BaseHeap* heap = nullptr;   // NATIVE PATCH: the readback landing never writes guest memory", 1),
]'''
if "friend class ::fable2::ngpu::backend::Driver" not in s:
    assert s.count(anchor) == 1
    s = s.replace(anchor, '''     "memory::BaseHeap* heap = nullptr;   // NATIVE PATCH: the readback landing never writes guest memory", 1),
    # The native replay driver calls the protected entry points the PM4 parser would (SetupContext, WriteRegister,
    # LoadShader, IssueDraw, IssueCopy, submission control) and sets the active shaders.
    ("command_processor.h", "class D3D12CommandProcessor : public CommandProcessor {\\n public:",
     "class D3D12CommandProcessor : public CommandProcessor {\\n  friend class ::fable2::ngpu::backend::Driver;   // NATIVE PATCH\\n public:", 1),
]''')
open(p, "w", encoding="utf-8").write(s)
f = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_xlat/rtc_d3d12/facade.h"
h = open(f, encoding="utf-8").read()
if "namespace fable2::ngpu::backend { class Driver; }" not in h:
    h = h.replace("namespace fable2::ngpu::rtc {", "namespace fable2::ngpu::backend { class Driver; }   // the replay driver (friend of the vendored command processor)\n\nnamespace fable2::ngpu::rtc {", 1)
    open(f, "w", encoding="utf-8", newline="\n").write(h)
print("ok")
