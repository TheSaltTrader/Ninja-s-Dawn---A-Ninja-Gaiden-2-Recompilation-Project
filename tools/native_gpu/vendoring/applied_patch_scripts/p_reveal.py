R = r'C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/'
def patch(p, old, new, count=1):
    s = open(R + p, encoding='utf-8').read()
    n = s.count(old)
    assert n == count, (p, old[:70], n)
    open(R + p, 'w', encoding='utf-8').write(s.replace(old, new))

# 1. The scene machine counts loading -> world entries.
patch('patch_hooks.cpp', '''void SetScene(Scene s) {
  const int was = g_scene.exchange(int(s), std::memory_order_relaxed);''', '''std::atomic<uint32_t> g_world_entries{0};   // loading -> world transitions (fable2::WorldEntriesFromLoading)
void SetScene(Scene s) {
  const int was = g_scene.exchange(int(s), std::memory_order_relaxed);
  if (was == int(Scene::kLoading) && s == Scene::kWorld) g_world_entries.fetch_add(1, std::memory_order_relaxed);''')
patch('patch_hooks.cpp', '''double fable2::SecondsSinceWorldCameraBuild() {''', '''uint32_t fable2::WorldEntriesFromLoading() { return g_world_entries.load(std::memory_order_relaxed); }

double fable2::SecondsSinceWorldCameraBuild() {''')
patch('fable2_viewstate.h', '''bool PauseMenuOpen();''', '''bool PauseMenuOpen();
// How many times the scene has gone loading -> world (a load finished and the stage is up). The native presenter's
// reveal hold starts on a change of this count.
unsigned WorldEntriesFromLoading();''')
s = open(R + 'fable2_viewstate.h', encoding='utf-8').read()
s = s.replace('unsigned WorldEntriesFromLoading();', 'uint32_t WorldEntriesFromLoading();')
if '<cstdint>' not in s:
    s = s.replace('#pragma once\n', '#pragma once\n\n#include <cstdint>\n', 1)
open(R + 'fable2_viewstate.h', 'w', encoding='utf-8').write(s)

# 2. Backend readiness: draws skipped for a pipeline still compiling, and whether any is compiling.
patch('native_gpu_backend.h', '''Stats GetStats();''', '''Stats GetStats();
// Readiness for the reveal hold: draws skipped so far because their pipeline was still being compiled (async shader
// compilation), and whether the pipeline cache is compiling right now.
struct Readiness { uint64_t pipeline_not_ready_draws = 0; bool creating_pipelines = false; };
Readiness GetReadiness();''')
patch('native_gpu_backend.cpp', '''Stats GetStats() { return g_driver.stats_; }''', '''Stats GetStats() { return g_driver.stats_; }
Readiness GetReadiness() { return g_driver.GetReadiness(); }''')
patch('native_gpu_backend.cpp', '''  void EndFrameNoSwap() { cp_->EndSubmission(false); }''', '''  void EndFrameNoSwap() { cp_->EndSubmission(false); }
  Readiness GetReadiness() {
    Readiness r;
    if (!cp_) return r;
    r.pipeline_not_ready_draws = cp_->draw_census_.pipeline_not_ready;
    r.creating_pipelines = cp_->pipeline_cache_ && cp_->pipeline_cache_->IsCreatingPipelines();
    return r;
  }''')

# 3. The hold, at the backend's swap (plugin GPU thread).
P = 'native_gpu_present.cpp'
patch(P, '''REXCVAR_DEFINE_INT32(ngpu_backend_selfcheck_every,''', '''REXCVAR_DEFINE_BOOL(ngpu_reveal_hold, true, "GPU", "Native-GPU BACKEND: after a load (scene loading -> world) keep showing the last loading-screen frame until the stage is complete - no draw skipped for a pipeline still compiling, none compiling, and a steady draw count for ngpu_reveal_hold_frames swaps - so the first world frame shown is the finished stage (user request 2026-09-26)");
REXCVAR_DEFINE_INT32(ngpu_reveal_hold_frames, 6, "GPU", "Native-GPU BACKEND: consecutive complete swaps that end the reveal hold");
REXCVAR_DEFINE_INT32(ngpu_reveal_hold_max_ms, 2500, "GPU", "Native-GPU BACKEND: the reveal hold never lasts longer than this (ms)");
REXCVAR_DEFINE_INT32(ngpu_backend_selfcheck_every,''')
patch(P, '''void BackendLockstepSync(const uint32_t* regs);   // defined with OnBridgeDraw''', '''void BackendLockstepSync(const uint32_t* regs);   // defined with OnBridgeDraw
// REVEAL HOLD (user, 2026-09-26: "the game loads partially polygons then shows the full loaded screen within less
// than a second, can we hide the partial load so the first thing they see is a fully loaded stage?"). The partial
// frames are draws the backend SKIPS while their pipeline compiles (async_shader_compilation) plus the stage's own
// first frames. Called at every backend swap: true = do not present this frame (the window keeps the last loading
// frame). Starts when the scene goes loading -> world; ends after ngpu_reveal_hold_frames complete swaps in a row
// (no skipped draw, nothing compiling, draw count within 3% of the previous swap), or at ngpu_reveal_hold_max_ms.
bool RevealHold() {
  static uint32_t seen_entries = fable2::WorldEntriesFromLoading();
  static bool holding = false;
  static int64_t hold_start_ms = 0;
  static uint64_t last_not_ready = 0, last_draws = 0, prev_frame_draws = 0;
  static int complete_run = 0, held_frames = 0;
  const auto rd = fable2::ngpu::backend::GetReadiness();
  const uint64_t draws = fable2::ngpu::backend::GetStats().draws;
  const uint64_t frame_draws = draws - last_draws;
  const uint64_t skipped = rd.pipeline_not_ready_draws - last_not_ready;
  last_draws = draws;
  last_not_ready = rd.pipeline_not_ready_draws;
  const int64_t now_ms = int64_t(GetTickCount64());
  const uint32_t entries = fable2::WorldEntriesFromLoading();
  if (entries != seen_entries) {
    seen_entries = entries;
    if (REXCVAR_GET(ngpu_reveal_hold)) {
      holding = true;
      hold_start_ms = now_ms;
      complete_run = 0;
      held_frames = 0;
      REXLOG_INFO("[ngpu] REVEAL: load finished (scene loading -> world) - holding the loading screen until the stage is complete");
    }
  }
  const uint64_t prev = prev_frame_draws;
  prev_frame_draws = frame_draws;
  if (!holding) return false;
  const bool steady = prev && frame_draws &&
                      (frame_draws > prev ? frame_draws - prev : prev - frame_draws) * 100 <= prev * 3;
  const bool complete = skipped == 0 && !rd.creating_pipelines && steady;
  complete_run = complete ? complete_run + 1 : 0;
  ++held_frames;
  const int64_t held_ms = now_ms - hold_start_ms;
  const bool done = complete_run >= std::max(1, REXCVAR_GET(ngpu_reveal_hold_frames));
  const bool cap = held_ms >= REXCVAR_GET(ngpu_reveal_hold_max_ms);
  if (done || cap) {
    holding = false;
    REXLOG_INFO("[ngpu] REVEAL: showing the stage after {} held swaps / {} ms ({})", held_frames, held_ms,
                done ? "complete" : "the cap - the stage was still changing");
    return false;
  }
  return true;
}''')
patch(P, '''      RequestNativePresent();
      BackendDirtyCoverageAtSwap();''', '''      if (!RevealHold()) RequestNativePresent();
      BackendDirtyCoverageAtSwap();''')
print('ok')
