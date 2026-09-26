R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))

C = 'rexglue-src/src/graphics/d3d12/command_processor.cpp'
patch(C, '''static uint64_t g_ngpu_bodies[8];''', '''static uint64_t g_ngpu_bodies[8];

// REVEAL HOLD (user request 2026-09-26: "the game loads partially polygons then shows the full loaded screen within
// less than a second, can we hide the partial load so the first thing they see is a fully loaded stage?"). The app
// calls RexNgpuRevealAfterLoad when its scene goes loading -> world; from the next swap the presenter keeps its last
// output (the loading screen's fade) until the stage is complete: no draw skipped for a pipeline still compiling
// (async_shader_compilation), none compiling, and a draw count within 3% for reveal_hold_frames swaps in a row -
// or reveal_hold_max_ms at most. The partial frames were exactly those skipped draws (native RV0/RV1 recordings).
REXCVAR_DEFINE_BOOL(reveal_hold, true, "GPU/D3D12",
                    "After a load, show the stage only once it is complete (hides the partially drawn first frames)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(reveal_hold_frames, 6, "GPU/D3D12",
                     "Consecutive complete frames that end the post-load reveal hold")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(reveal_hold_max_ms, 2500, "GPU/D3D12",
                     "The post-load reveal hold never lasts longer than this (ms)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
static std::atomic<uint32_t> g_reveal_requests{0};
extern "C" __declspec(dllexport) void RexNgpuRevealAfterLoad() { g_reveal_requests.fetch_add(1); }''')

H = 'rexglue-src/src/graphics/d3d12/command_processor.h'
s = open(R + H, encoding='utf-8').read()
anchor = '  DeferredCommandList deferred_command_list_;'
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + '''
  // [reveal] the post-load hold (IssueSwap): true = keep the presenter's last output this swap.
  bool RevealHold();
  uint32_t reveal_seen_requests_ = 0;
  bool reveal_holding_ = false;
  uint64_t reveal_start_ms_ = 0, reveal_last_not_ready_ = 0, reveal_last_draws_ = 0, reveal_prev_frame_draws_ = 0;
  int reveal_complete_run_ = 0, reveal_held_frames_ = 0;''')
open(R + H, 'w', encoding='utf-8').write(s)

patch(C, '''  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,''', '''  if (RevealHold()) {
    // [reveal] The stage is still coming in: the presenter keeps showing its last output.
  } else
  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,''')

# The hold itself, before IssueSwap.
patch(C, '''void D3D12CommandProcessor::IssueSwap(''', '''bool D3D12CommandProcessor::RevealHold() {
  const uint64_t not_ready = draw_census_.pipeline_not_ready;
  const uint64_t draws = g_ngpu_bodies[0];
  const uint64_t frame_draws = draws - reveal_last_draws_;
  const uint64_t skipped = not_ready - reveal_last_not_ready_;
  reveal_last_draws_ = draws;
  reveal_last_not_ready_ = not_ready;
  const uint64_t prev = reveal_prev_frame_draws_;
  reveal_prev_frame_draws_ = frame_draws;
  const uint64_t now_ms = GetTickCount64();
  const uint32_t requests = g_reveal_requests.load();
  if (requests != reveal_seen_requests_) {
    reveal_seen_requests_ = requests;
    if (REXCVAR_GET(reveal_hold)) {
      reveal_holding_ = true;
      reveal_start_ms_ = now_ms;
      reveal_complete_run_ = 0;
      reveal_held_frames_ = 0;
      REXLOG_INFO("[reveal] load finished - holding the last frame until the stage is complete");
    }
  }
  if (!reveal_holding_) return false;
  const bool steady =
      prev && frame_draws && (frame_draws > prev ? frame_draws - prev : prev - frame_draws) * 100 <= prev * 3;
  const bool creating = pipeline_cache_ && pipeline_cache_->IsCreatingPipelines();
  reveal_complete_run_ = (skipped == 0 && !creating && steady) ? reveal_complete_run_ + 1 : 0;
  ++reveal_held_frames_;
  const uint64_t held_ms = now_ms - reveal_start_ms_;
  const bool done = reveal_complete_run_ >= std::max(1, int(REXCVAR_GET(reveal_hold_frames)));
  const bool cap = held_ms >= uint64_t(std::max(0, int(REXCVAR_GET(reveal_hold_max_ms))));
  if (done || cap) {
    reveal_holding_ = false;
    REXLOG_INFO("[reveal] showing the stage after {} held frames / {} ms ({})", reveal_held_frames_, held_ms,
                done ? "complete" : "the cap - the stage was still changing");
    return false;
  }
  return true;
}

void D3D12CommandProcessor::IssueSwap(''')

# The app announces the transition to the plugin (and the native window keeps its own counter).
patch('wt-fable2-nativegpu/src/patch_hooks.cpp', '''  if (was == int(Scene::kLoading) && s == Scene::kWorld) g_world_entries.fetch_add(1, std::memory_order_relaxed);''', '''  if (was == int(Scene::kLoading) && s == Scene::kWorld) {
    g_world_entries.fetch_add(1, std::memory_order_relaxed);
    // The plugin's presenter holds its last frame until the stage is complete (rexgpu-xenos RexNgpuRevealAfterLoad;
    // an older plugin without the export simply shows the load as before).
    using RevealFn = void (*)();
    static RevealFn reveal = [] {
      HMODULE m = GetModuleHandleA("rexgpu-xenos.dll");
      return m ? reinterpret_cast<RevealFn>(GetProcAddress(m, "RexNgpuRevealAfterLoad")) : nullptr;
    }();
    if (reveal) reveal();
  }''')
print('ok')
