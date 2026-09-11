#include "ng2_diagnostics.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "ng2_hwdetect.h"
#include "ng2_menu.h"  // NG2_VERSION

namespace fs = std::filesystem;

namespace ng2 {
namespace {

// The log grows for as long as the session runs, and a report does not need all
// of it. The tail is where the failure is.
constexpr size_t kMaxLogLines = 4000;

std::string TimeStamp(const char* format) {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &now);
  char buffer[64];
  std::strftime(buffer, sizeof(buffer), format, &tm);
  return buffer;
}

// The newest ng2_*.log in `dir`. The runtime numbers them per launch, so the
// newest is this session's - and that is the one a report is about.
fs::path NewestLog(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return {};
  fs::path best;
  fs::file_time_type best_time{};
  for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_regular_file(ec))
      continue;
    const auto name = it->path().filename().string();
    if (name.rfind("ng2_", 0) != 0 || it->path().extension() != ".log")
      continue;
    const auto stamp = fs::last_write_time(it->path(), ec);
    if (ec)
      continue;
    if (best.empty() || stamp > best_time) {
      best = it->path();
      best_time = stamp;
    }
  }
  return best;
}

void AppendFile(std::ostream& out, const fs::path& path, const char* title,
                size_t tail_lines = 0) {
  out << "\n===== " << title << " =====\n";
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    out << "(not present: " << path.string() << ")\n";
    return;
  }
  out << "(" << path.string() << ")\n";
  std::ifstream in(path);
  if (!in) {
    out << "(could not be opened - it may be held by another process)\n";
    return;
  }
  if (tail_lines == 0) {
    out << in.rdbuf();
    out << "\n";
    return;
  }
  // Keep only the last `tail_lines`, and say so when anything was dropped, so
  // nobody reads a truncated log as a complete one.
  std::deque<std::string> tail;
  std::string line;
  size_t total = 0;
  while (std::getline(in, line)) {
    ++total;
    tail.push_back(line);
    if (tail.size() > tail_lines)
      tail.pop_front();
  }
  if (total > tail.size())
    out << "(showing the last " << tail.size() << " of " << total << " lines)\n";
  for (const auto& l : tail)
    out << l << "\n";
}

std::string OsVersion() {
  // GetVersionEx lies without a manifest; the registry keeps the real thing.
  HKEY key{};
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return "Windows (version not readable)";
  }
  auto read = [key](const char* name) -> std::string {
    char buffer[256];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    if (RegQueryValueExA(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
      return std::string(buffer, size ? size - 1 : 0);
    }
    return {};
  };
  std::string product = read("ProductName");
  std::string build = read("CurrentBuildNumber");
  std::string display = read("DisplayVersion");
  RegCloseKey(key);
  std::string result = product.empty() ? "Windows" : product;
  if (!display.empty())
    result += " " + display;
  if (!build.empty())
    result += " (build " + build + ")";
  return result;
}

std::string CpuName() {
  HKEY key{};
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return "unknown";
  }
  char buffer[256];
  DWORD size = sizeof(buffer);
  DWORD type = 0;
  std::string name = "unknown";
  if (RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                       reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
      type == REG_SZ) {
    name.assign(buffer, size ? size - 1 : 0);
  }
  RegCloseKey(key);
  while (!name.empty() && name.back() == ' ')
    name.pop_back();
  return name;
}

double GigabytesOfRam() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status))
    return 0.0;
  return double(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

bool CopyToClipboard(const std::string& text) {
  if (!OpenClipboard(nullptr))
    return false;
  bool ok = false;
  if (EmptyClipboard()) {
    if (HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
      if (void* target = GlobalLock(handle)) {
        std::memcpy(target, text.c_str(), text.size() + 1);
        GlobalUnlock(handle);
        // On success the clipboard owns the handle, so it must not be freed.
        ok = SetClipboardData(CF_TEXT, handle) != nullptr;
        if (!ok)
          GlobalFree(handle);
      } else {
        GlobalFree(handle);
      }
    }
  }
  CloseClipboard();
  return ok;
}

void RevealInExplorer(const fs::path& file) {
  const std::string argument = "/select,\"" + file.string() + "\"";
  ShellExecuteA(nullptr, "open", "explorer.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
}

DiagnosticsResult WriteDiagnostics(const fs::path& settings_file, const fs::path& log_dir) {
  DiagnosticsResult result;
  const fs::path out_dir = rex::filesystem::GetExecutableFolder() / "diagnostics";
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  if (ec) {
    result.error = "Could not create " + out_dir.string();
    return result;
  }

  const fs::path out_file =
      out_dir / ("ng2-diagnostics-" + TimeStamp("%Y%m%d-%H%M%S") + ".txt");
  std::ofstream out(out_file, std::ios::binary);
  if (!out) {
    result.error = "Could not write " + out_file.string();
    return result;
  }

  out << "Ninja Gaiden II recompilation - diagnostics\n";
  out << "Written " << TimeStamp("%Y-%m-%d %H:%M:%S") << "\n";
  out << "Port version: " << NG2_VERSION << "\n";
  out << "Executable:   " << (rex::filesystem::GetExecutableFolder() / "ng2.exe").string() << "\n";

  out << "\n===== Machine =====\n";
  out << "OS:   " << OsVersion() << "\n";
  out << "CPU:  " << CpuName() << "\n";
  char ram[64];
  std::snprintf(ram, sizeof(ram), "%.1f GB", GigabytesOfRam());
  out << "RAM:  " << ram << "\n";
  const GpuInfo gpu = DetectGpu();
  if (gpu.valid) {
    char vram[64];
    std::snprintf(vram, sizeof(vram), "%.1f GB",
                  double(gpu.dedicated_bytes) / (1024.0 * 1024.0 * 1024.0));
    out << "GPU:  " << gpu.name << " (" << vram << " dedicated)\n";
  } else {
    out << "GPU:  not readable through DXGI\n";
  }

  AppendFile(out, settings_file, "ng2_settings.cfg");
  AppendFile(out, rex::filesystem::GetExecutableFolder() / "cache" / "ng2_tuning.toml",
             "cache/ng2_tuning.toml");

  const fs::path log = NewestLog(log_dir);
  if (log.empty()) {
    out << "\n===== log =====\n(no ng2_*.log found in " << log_dir.string() << ")\n";
  } else {
    AppendFile(out, log, "log (this session)", kMaxLogLines);
  }

  out.flush();
  if (!out) {
    result.error = "The file was only partly written - is the disk full?";
    return result;
  }
  out.close();

  REXLOG_INFO("Diagnostics: wrote {}", out_file.string());
  result.ok = true;
  result.file = out_file;
  return result;
}

}  // namespace ng2
