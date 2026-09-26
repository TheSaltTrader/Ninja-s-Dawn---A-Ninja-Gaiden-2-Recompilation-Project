p = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/rexglue-src/src/graphics/d3d12/command_processor.cpp"
s = open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
def rep(a, b):
    global s
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if s.count(a2) != 1: raise SystemExit("anchor count %d: %r" % (s.count(a2), a[:80]))
    s = s.replace(a2, b2)
rep("REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, \"GPU/D3D12\",",
    """// NATIVE GPU OFFLOAD (2026-09-26): the in-app native renderer has transplanted this backend and is fed from the
// bridge callbacks; with this on, THIS plugin keeps its PM4 parser, register file and the bridge, but performs no GPU
// work of its own - no draws, no resolves, no swaps (its window stops updating) - and the native backend lands the
// readbacks in guest memory instead. Read at startup.
REXCVAR_DEFINE_BOOL(gpu_offload_to_native, false, "GPU/D3D12",
                    "Hand all GPU work to the in-app native renderer (the plugin only parses PM4 and feeds the bridge); needs the native backend enabled")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
static bool OffloadedToNative() {
  static const bool v = REXCVAR_GET(gpu_offload_to_native);
  return v;
}
REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",""")
rep("""bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {""", """bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  if (OffloadedToNative()) return true;   // the native backend drew this one from the bridge callback""")
rep("""bool D3D12CommandProcessor::IssueCopy() {""", """bool D3D12CommandProcessor::IssueCopy() {
  if (OffloadedToNative()) return true;""")
rep("""void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {""", """void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  if (OffloadedToNative()) return;""")
open(p, "w", encoding="utf-8", newline="").write(s)
print("ok")
