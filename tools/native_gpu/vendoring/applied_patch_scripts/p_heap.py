p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
anchor = '''    ("command_processor.cpp", "presenter->RefreshGuestOutput(", "::fable2::ngpu::rtc::NativeRefreshGuestOutput(", 1),
]'''
assert s.count(anchor) == 1
s = s.replace(anchor, '''    ("command_processor.cpp", "presenter->RefreshGuestOutput(", "::fable2::ngpu::rtc::NativeRefreshGuestOutput(", 1),
    # The readback LANDING (a copy into guest memory) checks the physical heap first; no heap = it never lands. A
    # second layer behind the forced-off readback cvars.
    ("command_processor.cpp", "memory::BaseHeap* heap = memory_->physical_heap();",
     "memory::BaseHeap* heap = nullptr;   // NATIVE PATCH: the readback landing never writes guest memory", 1),
]''')
open(p, "w", encoding="utf-8").write(s)
print("ok")
