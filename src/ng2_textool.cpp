#include "ng2_textool.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace fs = std::filesystem;

namespace ng2 {
namespace {

// Runs a command line and hands each line of its output to `on_line`.
//
// Not _popen: that runs the child through cmd.exe and gives it a console, so
// packing textures flashed a black window over the game and the interpreter
// probes flashed more before the setup screen had even drawn. CREATE_NO_WINDOW
// with an inherited pipe does the same job invisibly.
//
// The child and everything it starts go into a job object. The texture tool
// is a TREE - py.exe starts python.exe, which starts the upscaler - so killing
// only the process created here left the rest running: Cancel used to set a
// flag nobody read, and closing the game stopped the run only because the
// output pipe broke under it some seconds later. Cancel now terminates the
// job, and so does closing its handle - on return from here, or when the game
// exits with a run still going - so nothing keeps writing into the pack after
// the player asked it to stop.
//
// Returns false only when the process could not be started; the child's own
// exit code comes back in `exit_code`. When `cancel` becomes true the tree is
// killed within about 50 ms and this returns true with a non-zero exit code;
// the caller reads `cancel` to tell that apart from a failure.
bool RunHidden(const std::string& command_line,
               const std::function<void(const std::string&)>& on_line,
               DWORD* exit_code = nullptr,
               const std::atomic<bool>* cancel = nullptr) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0))
    return false;
  // Only the child may inherit the writing end, or the read below never sees
  // end-of-file and the wait hangs forever.
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                            sizeof(limits));
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::string mutable_cmd = command_line;  // CreateProcessA may write to it
  // Suspended, so it is inside the job before it can start anything itself.
  const BOOL ok = CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, nullptr, &si, &pi);
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    if (job != nullptr)
      CloseHandle(job);
    return false;
  }
  if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
    // Not fatal - the run still works - only the tree kill is lost, and the
    // process itself is still terminated on cancel.
    CloseHandle(job);
    job = nullptr;
  }
  ResumeThread(pi.hThread);

  std::string pending;
  char buffer[4096];
  bool killed = false;
  for (;;) {
    if (cancel != nullptr && cancel->load()) {
      if (job != nullptr)
        TerminateJobObject(job, 1);
      else
        TerminateProcess(pi.hProcess, 1);
      killed = true;
      break;
    }
    // Poll rather than block in ReadFile: a blocking read cannot notice a
    // cancel until the child prints something, and the upscaler prints
    // nothing for the length of a chunk.
    DWORD available = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr))
      break;  // every writer has closed: the whole tree is finished
    if (available == 0) {
      Sleep(50);
      continue;
    }
    DWORD got = 0;
    if (!ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) || got == 0)
      break;
    pending.append(buffer, got);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string text = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
      if (on_line)
        on_line(text);
    }
  }
  if (!pending.empty() && on_line && !killed)
    on_line(pending);

  WaitForSingleObject(pi.hProcess, INFINITE);
  if (exit_code != nullptr)
    GetExitCodeProcess(pi.hProcess, exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(read_end);
  if (job != nullptr)
    CloseHandle(job);  // and with it anything still running under the job
  return true;
}

// Whether `exe` runs at all, via `where` so the same rules apply as when the
// player types the name themselves.
bool OnPath(const char* exe, std::string& out) {
  std::string first;
  const std::string cmd = std::string("cmd.exe /c where ") + exe;
  if (!RunHidden(cmd, [&first](const std::string& line) {
        if (first.empty() && !line.empty() && line.find("INFO:") != 0)
          first = line;
      }))
    return false;
  if (first.empty())
    return false;
  out = first;
  return true;
}

// Any of our Python helpers. They sit beside the executable in a release and
// under tools/ in the source tree, so both are tried.
// Where the tools live. The port is run from its build folder during
// development and from an install folder afterwards, so both are tried
// rather than assuming one.
fs::path ToolsDir() {
  const auto exe = rex::filesystem::GetExecutableFolder();
  for (const auto& candidate :
       {exe / "tools", exe.parent_path().parent_path().parent_path() / "tools"}) {
    std::error_code ec;
    if (fs::is_directory(candidate, ec))
      return candidate;
  }
  return exe / "tools";
}

// Whether `base` holds the upscaler executable, wherever in the archive it
// landed - the layout has changed between releases, and "the folder exists"
// is not the same as "the tool is there".
bool UpscalerUnder(const fs::path& base) {
  std::error_code ec;
  if (!fs::is_directory(base, ec))
    return false;
  for (fs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
    if (ec)
      break;
    if (it->path().filename() == "realesrgan-ncnn-vulkan.exe")
      return true;
  }
  return false;
}

fs::path FindToolScript(const std::string& name) {
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  for (const auto& rel : {"tools/" + name, name, "../../../tools/" + name}) {
    const auto candidate = exe_dir / rel;
    if (fs::is_regular_file(candidate, ec)) {
      auto canonical = fs::weakly_canonical(candidate, ec);
      return (ec || canonical.empty()) ? candidate : canonical;
    }
  }
  return {};
}

int CountFiles(const fs::path& dir, const char* extension) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return 0;
  int n = 0;
  for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_regular_file(ec))
      continue;
    auto ext = it->path().extension().string();
    for (auto& c : ext)
      c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (ext == extension)
      ++n;
  }
  return n;
}

}  // namespace

TextureTools FindTextureTools() {
  TextureTools t;
  // "py" is the Windows launcher and is what a normal Python install leaves on
  // PATH; "python" and "python3" cover the rest.
  for (const char* candidate : {"py", "python", "python3"}) {
    if (OnPath(candidate, t.python))
      break;
  }
  // Said once, in the log, so "the AI option is greyed out" can be read
  // against what was actually on disk.
  const fs::path bundled = ToolsDir() / "upscaler";
  REXLOG_INFO("Texture tools: python '{}'; bundled AI upscaler {} at {}",
              t.python, UpscalerUnder(bundled) ? "present" : "MISSING",
              bundled.string());
  return t;
}

int CountDumpedTextures(const fs::path& texture_dir) {
  // Whichever source is richer. The dump folder holds the raw guest bytes AND
  // the decoded PNGs, and the two drift apart: the decoded art outlives a
  // cleared .bin and can still be packed, so counting only .bin reports "you
  // have nothing" to someone holding a full set of textures.
  const fs::path dir = texture_dir / "dump";
  return std::max(CountFiles(dir, ".bin"), CountFiles(dir, ".png"));
}

int CountPackedTextures(const fs::path& texture_dir) {
  // .tex, not .png. The pack stopped being PNG when decoding one cost ~16ms on
  // the render thread; this counter did not follow, so a working 1290-texture
  // pack reported as empty and the menu DISABLED the control that uses it.
  return CountFiles(texture_dir / "pack", ".tex");
}

namespace {

// pack_reason() from tools/upscale_textures.py, ported. The two MUST agree, or
// the menu counts textures the tool will never write and reports them as
// waiting forever. Guest texture format codes as the dump index records them.
constexpr int kFmt8 = 2, kFmt1555 = 3, kFmt565 = 4, kFmt8888 = 6, kFmt88 = 10,
              kFmt8888A = 14, kFmt4444 = 15, kFmtDXT1 = 18, kFmtDXT23 = 19,
              kFmtDXT45 = 20;

bool FormatSupported(int f) {
  switch (f) {
    case kFmt8: case kFmt1555: case kFmt565: case kFmt8888: case kFmt88:
    case kFmt8888A: case kFmt4444: case kFmtDXT1: case kFmtDXT23: case kFmtDXT45:
      return true;
    default:
      return false;
  }
}
bool IsDxt(int f) { return f == kFmtDXT1 || f == kFmtDXT23 || f == kFmtDXT45; }
bool PowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

bool PackCandidate(int w, int h, int fmt) {
  if (!FormatSupported(fmt))
    return false;
  if (fmt == kFmt8 || fmt == kFmt88)                 // masks, ramps, fonts, video
    return false;
  if (w <= 64 || h <= 64)                            // too small: HUD, icons
    return false;
  if (double(std::max(w, h)) / double(std::min(w, h)) >= 8.0)   // thin strip
    return false;
  if (!IsDxt(fmt) && !(PowerOfTwo(w) && PowerOfTwo(h)))         // framebuffer / video plane
    return false;
  return true;
}

// The format code from the name a decoded PNG carries: <id>_<w>x<h>_<name>.png
int FormatFromName(const std::string& n) {
  static const std::pair<const char*, int> kNames[] = {
      {"k_8_8_8_8_A", kFmt8888A}, {"k_8_8_8_8", kFmt8888}, {"k_1_5_5_5", kFmt1555},
      {"k_5_6_5", kFmt565},       {"k_4_4_4_4", kFmt4444}, {"k_8_8", kFmt88},
      {"k_8", kFmt8},             {"k_DXT1", kFmtDXT1},    {"k_DXT2_3", kFmtDXT23},
      {"k_DXT4_5", kFmtDXT45}};
  for (const auto& [name, code] : kNames)
    if (n == name)
      return code;
  return -1;
}

}  // namespace

PackCensus CountPack(const fs::path& texture_dir) {
  PackCensus c;
  const fs::path dump = texture_dir / "dump";
  const fs::path pack = texture_dir / "pack";
  std::error_code ec;

  // Every id in the pack, from one walk - not one stat per candidate.
  std::set<std::string> in_pack;
  if (fs::is_directory(pack, ec)) {
    for (fs::directory_iterator it(pack, ec), end; it != end; it.increment(ec)) {
      if (ec)
        break;
      if (!it->is_regular_file(ec))
        continue;
      // <id>-<hash>.tex since content hashes. An <id>.tex from before carries
      // no hash, the game ignores it, and it counts for nothing here - the
      // census is of what the game will actually load.
      if (it->path().extension() == ".tex") {
        const std::string stem = it->path().stem().string();
        if (stem.size() == 25 && stem[16] == '-')
          in_pack.insert(stem);
      }
    }
  }
  c.tex_files = int(in_pack.size());

  // The pack's own record of its settings (tools/upscale_textures.py writes
  // it as key=value lines).
  if (std::ifstream mf(pack / "pack.txt"); mf) {
    c.manifest = true;
    std::string line;
    while (std::getline(mf, line)) {
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      while (!val.empty() && (val.back() == '\r' || val.back() == '\n'))
        val.pop_back();
      if (key == "scale")
        c.pack_scale = std::atoi(val.c_str());
      else if (key == "upscaler")
        c.pack_upscaler = val;
      else if (key == "strength")
        c.pack_strength = float(std::atof(val.c_str()));
      else if (key == "complete")
        c.pack_complete = val == "1";
    }
  }

  // What was dumped: the index first (first entry per id wins, as in the
  // tool), then decoded PNGs the index no longer covers - a cleared .bin
  // leaves art the tool still packs, and it counts those too.
  struct Entry { int w, h, fmt; };
  std::map<std::string, Entry> dumped;
  if (std::ifstream in(dump / "index.txt"); in) {
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream fields(line);
      std::string tid, hash;
      int w = 0, h = 0, fmt = -1, tiled = 0, pitch = 0, endian = 0, dim = 0;
      unsigned size = 0;
      if (!(fields >> tid >> w >> h >> fmt))
        continue;
      // The tenth column is the content hash. A line without one has no raw
      // dump left to hash from, so the tool cannot pack it: not a candidate.
      // Two lines that differ only in the hash are two textures that share an
      // address, and each needs its own pack file - so the key is id+hash.
      if (!(fields >> tiled >> pitch >> endian >> dim >> size >> hash) || hash.size() != 8)
        continue;
      dumped.emplace(tid + "-" + hash, Entry{w, h, fmt});
    }
  }
  if (fs::is_directory(dump, ec)) {
    for (fs::directory_iterator it(dump, ec), end; it != end; it.increment(ec)) {
      if (ec)
        break;
      if (!it->is_regular_file(ec))
        continue;
      const std::string fn = it->path().filename().string();
      if (fn.size() < 24 || fn.compare(fn.size() - 4, 4, ".png") != 0)
        continue;
      // <id>-<hash>_<w>x<h>_<format>.png only. A PNG named without the hash
      // has no raw dump behind it and cannot be packed, so it is not counted.
      if (!(fn.size() > 26 && fn[16] == '-' && fn[25] == '_'))
        continue;
      const size_t dims = 26;
      const std::string tid = fn.substr(0, 25);
      if (dumped.count(tid))
        continue;
      const size_t x = fn.find('x', dims);
      const size_t us = fn.find('_', dims);
      if (x == std::string::npos || us == std::string::npos || x > us)
        continue;
      const int w = std::atoi(fn.c_str() + dims);
      const int h = std::atoi(fn.c_str() + x + 1);
      const int fmt = FormatFromName(fn.substr(us + 1, fn.size() - 4 - (us + 1)));
      if (w > 0 && h > 0 && fmt >= 0)
        dumped.emplace(tid, Entry{w, h, fmt});
    }
  }
  c.dumped = int(dumped.size());
  for (const auto& [tid, e] : dumped) {
    if (!PackCandidate(e.w, e.h, e.fmt)) {
      ++c.excluded;
      continue;
    }
    ++c.candidates;
    if (in_pack.count(tid))
      ++c.packed;
    else
      ++c.waiting;
  }
  return c;
}

bool UpscalerInstalled(const fs::path& texture_dir) {
  // The texture folder's own copy first (the Download button puts one there,
  // and it is the override), then the copy the port ships under
  // tools/upscaler. v1.0.14 shipped that copy and looked only here, so a
  // fresh install said "not installed" and packed with Lanczos.
  if (!texture_dir.empty() && UpscalerUnder(texture_dir / "upscaler"))
    return true;
  return UpscalerUnder(ToolsDir() / "upscaler");
}

std::thread DownloadUpscalerAsync(const TextureTools& tools, const fs::path& texture_dir,
                                  ExtractProgress& progress) {
  progress.Reset();
  progress.running = true;
  return std::thread([tools, texture_dir, &progress] {
    auto fail = [&progress](const std::string& why) {
      progress.SetError(why);
      progress.failed = true;
      progress.complete = true;
      progress.running = false;
    };
    if (tools.python.empty()) {
      fail("Python was not found");
      return;
    }
    const fs::path script = FindToolScript("get_upscaler.py");
    if (script.empty()) {
      fail("tools/get_upscaler.py is missing from this install");
      return;
    }
    std::error_code ec;
    fs::create_directories(texture_dir, ec);

    const std::string cmd = "\"" + tools.python + "\" \"" + script.string() +
                            "\" --dir \"" + texture_dir.string() + "\"";
    REXLOG_INFO("Upscaler download: {}", cmd);
    DWORD rc = 0;
    const bool started = RunHidden(
        cmd,
        [&progress](const std::string& text) {
          if (text.empty())
            return;
          REXLOG_INFO("  {}", text);
          // "PROGRESS <done> <total> <name>", in BYTES here rather than files.
          if (text.rfind("PROGRESS ", 0) == 0) {
            long long done = 0, total = 0;
            if (std::sscanf(text.c_str(), "PROGRESS %lld %lld", &done, &total) == 2 &&
                total > 0) {
              progress.files_done = int(done / 1024);
              progress.files_total = int(total / 1024);
            }
          }
        },
        &rc, &progress.cancel);
    if (!started) {
      fail("Could not start Python");
      return;
    }
    if (progress.cancel.load()) {
      fail("Cancelled.");
      return;
    }
    if (rc != 0 || !UpscalerInstalled(texture_dir)) {
      fail("The download did not complete - see the log");
      return;
    }
    progress.complete = true;
    progress.running = false;
  });
}

std::thread UpscaleTexturesAsync(const TextureTools& tools, const fs::path& texture_dir,
                                 bool upscale, int scale, bool ai, float ai_strength,
                                 bool only_missing, ExtractProgress& progress) {
  progress.Reset();
  progress.running = true;
  return std::thread([tools, texture_dir, upscale, scale, ai, ai_strength, only_missing,
                      &progress] {
    auto fail = [&progress](const std::string& why) {
      progress.SetError(why);
      progress.failed = true;
      progress.complete = true;
      progress.running = false;
    };
    if (tools.python.empty()) {
      fail("Python was not found");
      return;
    }
    const fs::path script = FindToolScript("upscale_textures.py");
    if (script.empty()) {
      fail("tools/upscale_textures.py is missing from this install");
      return;
    }
    progress.files_total = CountDumpedTextures(texture_dir);
    if (progress.files_total.load() == 0) {
      fail("No textures have been dumped yet - turn dumping on and play");
      return;
    }

    std::string cmd = "\"" + tools.python + "\" \"" + script.string() +
                      "\" --dir \"" + texture_dir.string() + "\"";
    if (upscale)
      cmd += " --upscale --scale " + std::to_string(scale);
    if (ai) {
      char strength[32];
      std::snprintf(strength, sizeof(strength), "%.2f", double(ai_strength));
      cmd += " --ai --ai-strength ";
      cmd += strength;
    }
    if (only_missing)
      cmd += " --only-missing";
    REXLOG_INFO("Texture pack: {}", cmd);

    DWORD rc = 0;
    const bool started = RunHidden(
        cmd,
        [&progress](const std::string& text) {
          if (text.empty())
            return;
          REXLOG_INFO("  {}", text);
          // "PHASE <i>/<n> <label>": the run's steps. Decoding every dump and
          // upscaling the art are reported as two bars, and without this the
          // second read as the first failing and starting over.
          if (text.rfind("PHASE ", 0) == 0) {
            int phase = 0, phases = 0;
            char label[200] = {0};
            const int fields = std::sscanf(text.c_str(), "PHASE %d/%d %199[^\n]",
                                           &phase, &phases, label);
            if (fields >= 2) {
              progress.files_done = 0;
              progress.files_total = 0;
              progress.phases = phases;
              progress.phase = phase;
              progress.SetPhaseLabel(fields >= 3 ? label : "");
              progress.SetCurrentFile({});
            }
            return;
          }
          // The script reports progress directly rather than leaving it to be
          // inferred from its chatter: "PROGRESS <done> <total> <name>".
          if (text.rfind("PROGRESS ", 0) == 0) {
            long long done = 0, total = 0;
            char name[260] = {0};
            const int fields =
                std::sscanf(text.c_str(), "PROGRESS %lld %lld %259[^\n]", &done, &total, name);
            if (fields >= 2 && total > 0) {
              progress.files_done = int(done);
              progress.files_total = int(total);
            }
            if (fields >= 3)
              progress.SetCurrentFile(name);
          }
        },
        &rc, &progress.cancel);
    if (!started) {
      fail("Could not start Python");
      return;
    }
    if (progress.cancel.load()) {
      // Every .tex is written whole, so what reached the pack before the stop
      // is usable and the next run overwrites it by id.
      fail("Cancelled. Textures written before the stop stay in the pack.");
      return;
    }
    if (rc != 0) {
      fail("The texture tool reported an error - see the log");
      return;
    }
    progress.complete = true;
    progress.running = false;
  });
}

}  // namespace ng2
