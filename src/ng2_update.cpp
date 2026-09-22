#include "ng2_update.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "ng2_menu.h"      // Fonts()
#include "ng2_platform.h"  // LaunchDetached, FormatBytes

#pragma comment(lib, "winhttp.lib")

namespace ng2 {
namespace update {
namespace {

// The public GitHub repository this build updates from. The path is the "latest
// release" endpoint; the host is the API, not the web site.
constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] =
    L"/repos/TheSaltTrader/"
    L"Ninja-s-Dawn---A-Ninja-Gaiden-2-Recompilation-Project/releases/latest";
constexpr wchar_t kUserAgent[] = L"NinjaGaidenII-Updater/1.0";

// The staged updater. Runs AFTER this process exits (a running exe cannot
// overwrite its own image), copies the new program files over the install, and
// relaunches. game/, dlc/, user/ and the two config files are never touched -
// which is the same contract the release notes state for a manual "unzip over
// it" update.
constexpr char kUpdaterScript[] = R"PS(param(
  [int]$WaitPid = 0,
  [string]$Zip,
  [string]$Install,
  [string]$Exe
)
$ErrorActionPreference = 'Stop'
try {
  if ($WaitPid -gt 0) {
    try { Wait-Process -Id $WaitPid -Timeout 120 -ErrorAction SilentlyContinue } catch {}
    # Wait-Process returns whether or not it actually exited. Copying over a
    # RUNNING install is what produces a half-updated one, so if it is still
    # alive we touch nothing: ng2.exe, rexruntime.dll and rexgpu-xenos.dll are
    # one ABI, and a new exe against an old runtime exits at startup with no
    # error and an empty log.
    $alive = $null
    try { $alive = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue } catch {}
    if ($alive) { throw "the running game did not exit within 120s; update not applied" }
  }
  Start-Sleep -Milliseconds 800
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $extract = Join-Path $env:TEMP ('ng2update_' + [guid]::NewGuid().ToString('N'))
  [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $extract)
  # The release zip has a single top-level folder (ng2recomp-vX.Y.Z).
  $root = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
  if (-not $root) { $root = Get-Item -LiteralPath $extract }
  $base = $root.FullName
  # Never overwrite the player's own data or settings.
  $protect = @('ng2_settings.cfg', 'ng2.toml', 'game', 'dlc', 'user')

  # STAGE EVERYTHING FIRST, COMMIT ONLY IF ALL OF IT LANDED. A copy that throws
  # part way through used to leave some files new and some old, and the script
  # then relaunched that. All-or-nothing is what the comment below has always
  # promised.
  $staged = New-Object System.Collections.ArrayList
  try {
    Get-ChildItem -LiteralPath $base -Recurse -File | ForEach-Object {
      $rel = $_.FullName.Substring($base.Length).TrimStart('\', '/')
      $top = ($rel -split '[\\/]', 2)[0]
      if ($protect -contains $top) { return }
      $dest = Join-Path $Install $rel
      $ddir = Split-Path -Parent $dest
      if (-not (Test-Path -LiteralPath $ddir)) {
        New-Item -ItemType Directory -Path $ddir -Force | Out-Null
      }
      Copy-Item -LiteralPath $_.FullName -Destination ($dest + '.ng2new') -Force
      [void]$staged.Add($dest)
    }
  } catch {
    foreach ($d in $staged) {
      Remove-Item -LiteralPath ($d + '.ng2new') -Force -ErrorAction SilentlyContinue
    }
    throw
  }

  # Commit. Record what has moved so a failure here can be rolled back too.
  $moved = New-Object System.Collections.ArrayList
  try {
    foreach ($d in $staged) {
      if (Test-Path -LiteralPath $d) {
        Move-Item -LiteralPath $d -Destination ($d + '.ng2old') -Force
      }
      Move-Item -LiteralPath ($d + '.ng2new') -Destination $d -Force
      [void]$moved.Add($d)
    }
  } catch {
    foreach ($d in $moved) {
      if (Test-Path -LiteralPath ($d + '.ng2old')) {
        Move-Item -LiteralPath ($d + '.ng2old') -Destination $d -Force -ErrorAction SilentlyContinue
      }
    }
    foreach ($d in $staged) {
      Remove-Item -LiteralPath ($d + '.ng2new') -Force -ErrorAction SilentlyContinue
    }
    throw
  }
  foreach ($d in $moved) {
    Remove-Item -LiteralPath ($d + '.ng2old') -Force -ErrorAction SilentlyContinue
  }

  Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
} catch {
  try { New-Item -ItemType Directory -Path (Join-Path $Install 'update') -Force | Out-Null } catch {}
  try { $_.ToString() | Out-File -FilePath (Join-Path $Install 'update\last_error.txt') -Encoding utf8 } catch {}
}
# Relaunch regardless: a failed copy leaves the old, still-runnable exe in place.
try { Start-Process -FilePath $Exe -WorkingDirectory $Install } catch {}
)PS";

// ---- state -------------------------------------------------------------

std::mutex g_mu;
Phase g_phase = Phase::kIdle;
std::string g_current, g_latest, g_notes, g_error, g_zip_url;
std::filesystem::path g_install, g_exe, g_stage_zip, g_script;
std::function<void()> g_apply;

std::atomic<double> g_progress{0.0};
std::atomic<std::uint64_t> g_done{0};
std::atomic<std::uint64_t> g_total{0};
std::atomic<bool> g_dismissed{false};
std::atomic<bool> g_user_started{false};
std::atomic<bool> g_busy{false};  // a worker thread is active

void SetPhase(Phase p) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_phase = p;
}
void SetError(const std::string& e) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_error = e;
  g_phase = Phase::kError;
  REXLOG_WARN("Update: {}", e);
}

// ---- small helpers -----------------------------------------------------

std::wstring Widen(const std::string& s) {
  if (s.empty())
    return {};
  const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                       static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), need);
  return out;
}

std::vector<int> VerParts(const std::string& v) {
  std::vector<int> out;
  std::string cur;
  for (char c : v) {
    if (c == '.') {
      out.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
      cur.clear();
    } else if (c >= '0' && c <= '9') {
      cur.push_back(c);
    } else {
      break;  // stop at a suffix like -rc1
    }
  }
  if (!cur.empty())
    out.push_back(std::atoi(cur.c_str()));
  return out;
}

bool IsNewer(const std::string& latest, const std::string& current) {
  const std::vector<int> a = VerParts(latest);
  const std::vector<int> b = VerParts(current);
  const size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;
    if (x != y)
      return x > y;
  }
  return false;
}

// Pull one JSON string value by key, with the escapes GitHub actually emits.
std::string JsonString(const std::string& body, const std::string& key) {
  const std::string k = "\"" + key + "\"";
  size_t p = body.find(k);
  if (p == std::string::npos)
    return "";
  p = body.find(':', p + k.size());
  if (p == std::string::npos)
    return "";
  ++p;
  while (p < body.size() &&
         (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r'))
    ++p;
  if (p >= body.size() || body[p] != '"')
    return "";
  ++p;
  std::string out;
  while (p < body.size()) {
    const char c = body[p++];
    if (c == '\\' && p < body.size()) {
      const char e = body[p++];
      switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': break;  // drop CR, keep LF
        case 't': out.push_back('\t'); break;
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'u':
          if (p + 4 <= body.size())
            p += 4;
          out.push_back('?');
          break;
        default: out.push_back(e); break;
      }
    } else if (c == '"') {
      break;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// The first release asset whose URL ends in .zip - our win-amd64 bundle.
std::string FindZipUrl(const std::string& body) {
  const std::string key = "\"browser_download_url\":\"";
  size_t p = 0;
  while ((p = body.find(key, p)) != std::string::npos) {
    const size_t s = p + key.size();
    const size_t e = body.find('"', s);
    if (e == std::string::npos)
      break;
    std::string url = body.substr(s, e - s);
    std::string norm;
    norm.reserve(url.size());
    for (size_t i = 0; i < url.size(); ++i) {
      if (url[i] == '\\' && i + 1 < url.size() && url[i + 1] == '/') {
        norm.push_back('/');
        ++i;
      } else {
        norm.push_back(url[i]);
      }
    }
    if (norm.size() >= 4 && norm.compare(norm.size() - 4, 4, ".zip") == 0)
      return norm;
    p = e + 1;
  }
  return "";
}

std::filesystem::path PowerShellPath() {
  const char* sysroot = std::getenv("SystemRoot");
  std::filesystem::path root =
      (sysroot && *sysroot) ? std::filesystem::path(sysroot)
                            : std::filesystem::path("C:\\Windows");
  return root / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
}

// ---- WinHTTP -----------------------------------------------------------

bool HttpsGetString(const wchar_t* host, const wchar_t* path, std::string& out,
                    std::string& err) {
  HINTERNET hs = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) {
    err = "WinHttpOpen failed";
    return false;
  }
  HINTERNET hc = WinHttpConnect(hs, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hc) {
    err = "connect failed";
    WinHttpCloseHandle(hs);
    return false;
  }
  HINTERNET hr =
      WinHttpOpenRequest(hc, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!hr) {
    err = "open request failed";
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return false;
  }
  WinHttpAddRequestHeaders(hr, L"Accept: application/vnd.github+json\r\n",
                           static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
  bool ok = false;
  if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hr, nullptr)) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      DWORD avail = 0;
      do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail))
          break;
        if (avail == 0)
          break;
        std::string buf(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hr, buf.data(), avail, &read))
          break;
        out.append(buf.data(), read);
      } while (avail > 0);
      ok = true;
    } else {
      err = "HTTP " + std::to_string(status);
    }
  } else {
    err = "request failed (" + std::to_string(GetLastError()) + ")";
  }
  WinHttpCloseHandle(hr);
  WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  return ok;
}

bool HttpsDownload(const std::string& url, const std::filesystem::path& dest,
                   std::string& err) {
  const std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[4096] = {0}, extra[4096] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = sizeof(host) / sizeof(*host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = sizeof(path) / sizeof(*path);
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = sizeof(extra) / sizeof(*extra);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    err = "bad download URL";
    return false;
  }
  const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  HINTERNET hs = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hs) {
    err = "WinHttpOpen failed";
    return false;
  }
  HINTERNET hc = WinHttpConnect(hs, host, uc.nPort, 0);
  if (!hc) {
    err = "connect failed";
    WinHttpCloseHandle(hs);
    return false;
  }
  const std::wstring full = std::wstring(path) + extra;
  HINTERNET hr = WinHttpOpenRequest(hc, L"GET", full.c_str(), nullptr,
                                    WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    secure ? WINHTTP_FLAG_SECURE : 0);
  if (!hr) {
    err = "open request failed";
    WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return false;
  }
  bool ok = false;
  if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(hr, nullptr)) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      unsigned long long total = 0;
      DWORD tsz = sizeof(total);
      WinHttpQueryHeaders(
          hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
          WINHTTP_HEADER_NAME_BY_INDEX, &total, &tsz, WINHTTP_NO_HEADER_INDEX);
      g_total.store(total);
      std::ofstream f(dest, std::ios::binary | std::ios::trunc);
      if (!f) {
        err = "cannot write " + dest.string();
      } else {
        std::vector<char> buf(64 * 1024);
        unsigned long long done = 0;
        bool broke = false;
        for (;;) {
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(hr, &avail)) {
            broke = true;
            break;
          }
          if (avail == 0)
            break;
          const DWORD want =
              avail < buf.size() ? avail : static_cast<DWORD>(buf.size());
          DWORD read = 0;
          if (!WinHttpReadData(hr, buf.data(), want, &read)) {
            broke = true;
            break;
          }
          if (read == 0)
            break;
          f.write(buf.data(), read);
          done += read;
          g_done.store(done);
          if (total > 0)
            g_progress.store(double(done) / double(total));
        }
        f.close();
        if (!broke && (total == 0 || done >= total))
          ok = true;
        else if (!broke && done > 0)
          ok = true;  // some mirrors omit Content-Length
        else
          err = "incomplete download";
      }
    } else {
      err = "HTTP " + std::to_string(status);
    }
  } else {
    err = "request failed (" + std::to_string(GetLastError()) + ")";
  }
  WinHttpCloseHandle(hr);
  WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  return ok;
}

bool WriteUpdaterScript(std::string& err) {
  std::error_code ec;
  std::filesystem::create_directories(g_install / "update", ec);
  const std::filesystem::path script = g_install / "update" / "apply_update.ps1";
  std::ofstream f(script, std::ios::binary | std::ios::trunc);
  if (!f) {
    err = "cannot write " + script.string();
    return false;
  }
  f.write(kUpdaterScript, static_cast<std::streamsize>(sizeof(kUpdaterScript) - 1));
  f.close();
  std::lock_guard<std::mutex> lk(g_mu);
  g_script = script;
  return true;
}

}  // namespace

// ---- public API --------------------------------------------------------

void Init(const std::string& current_version,
          const std::filesystem::path& install_dir,
          const std::filesystem::path& exe_path) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_current = current_version;
  g_install = install_dir;
  g_exe = exe_path;
  g_stage_zip = install_dir / "update" / "ng2-update.zip";
}

void SetApplyHandler(std::function<void()> handler) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_apply = std::move(handler);
}

void CheckAsync() {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true))
    return;  // a worker is already running
  SetPhase(Phase::kChecking);
  std::thread([] {
    std::string body, err;
    if (!HttpsGetString(kApiHost, kApiPath, body, err)) {
      SetError("could not reach the update server (" + err + ")");
      g_busy.store(false);
      return;
    }
    const std::string tag = JsonString(body, "tag_name");
    std::string latest = tag;
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V'))
      latest.erase(0, 1);
    const std::string zip = FindZipUrl(body);
    const std::string notes = JsonString(body, "body");
    std::string cur;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_latest = latest;
      g_notes = notes;
      g_zip_url = zip;
      cur = g_current;
    }
    if (latest.empty()) {
      SetError("the update server did not return a version");
    } else if (IsNewer(latest, cur) && !zip.empty()) {
      REXLOG_INFO("Update: v{} is available (have v{})", latest, cur);
      SetPhase(Phase::kAvailable);
    } else {
      REXLOG_INFO("Update: up to date (v{})", cur);
      SetPhase(Phase::kUpToDate);
    }
    g_busy.store(false);
  }).detach();
}

void BeginUpdate() {
  g_user_started.store(true);
  g_dismissed.store(false);
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true))
    return;
  SetPhase(Phase::kDownloading);
  g_progress.store(0.0);
  g_done.store(0);
  g_total.store(0);
  std::thread([] {
    std::string url;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      url = g_zip_url;
    }
    if (url.empty()) {
      SetError("no download is available - check for updates first");
      g_busy.store(false);
      return;
    }
    std::filesystem::path zip;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      zip = g_stage_zip;
    }
    std::string err;
    if (!HttpsDownload(url, zip, err)) {
      SetError("download failed (" + err + ")");
      g_busy.store(false);
      return;
    }
    if (!WriteUpdaterScript(err)) {
      SetError("could not stage the updater (" + err + ")");
      g_busy.store(false);
      return;
    }
    REXLOG_INFO("Update: downloaded and staged, ready to apply");
    SetPhase(Phase::kReadyToApply);
    g_busy.store(false);
  }).detach();
}

void ApplyNow() {
  static std::atomic<bool> once{false};
  bool expected = false;
  if (!once.compare_exchange_strong(expected, true))
    return;
  std::filesystem::path zip, install, exe, script;
  std::function<void()> apply;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    zip = g_stage_zip;
    install = g_install;
    exe = g_exe;
    script = g_script;
    apply = g_apply;
  }
  const DWORD pid = GetCurrentProcessId();
  const std::string args =
      "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
      script.string() + "\" -WaitPid " + std::to_string(pid) + " -Zip \"" +
      zip.string() + "\" -Install \"" + install.string() + "\" -Exe \"" +
      exe.string() + "\"";
  if (!ng2::LaunchDetached(PowerShellPath(), args)) {
    once.store(false);
    SetError("could not start the installer");
    return;
  }
  SetPhase(Phase::kApplying);
  REXLOG_INFO("Update: installer launched, exiting to apply");
  if (apply)
    apply();  // shuts down cleanly and exits; does not return
}

void Dismiss() { g_dismissed.store(true); }

Snapshot Get() {
  Snapshot s;
  std::lock_guard<std::mutex> lk(g_mu);
  s.phase = g_phase;
  s.current_version = g_current;
  s.latest_version = g_latest;
  s.notes = g_notes;
  s.error = g_error;
  s.dismissed = g_dismissed.load();
  s.user_started = g_user_started.load();
  s.progress = g_progress.load();
  s.bytes_done = g_done.load();
  s.bytes_total = g_total.load();
  return s;
}

// ---- launch prompt -----------------------------------------------------

UpdateOverlay::UpdateOverlay(rex::ui::ImGuiDrawer* drawer)
    : rex::ui::ImGuiDialog(drawer) {}
UpdateOverlay::~UpdateOverlay() = default;

void UpdateOverlay::OnDraw(ImGuiIO& io) {
  const Snapshot s = Get();

  bool show = false;
  if (!s.dismissed) {
    if (s.phase == Phase::kAvailable)
      show = true;
    else if (s.user_started &&
             (s.phase == Phase::kDownloading || s.phase == Phase::kReadyToApply ||
              s.phase == Phase::kApplying || s.phase == Phase::kError))
      show = true;
  }
  if (!show)
    return;

  static bool show_notes = false;

  ImGui::PushFont(Fonts().body, Fonts().body != nullptr ? Fonts().body_size : 0.0f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Always);
  if (ImGui::Begin("Ninja Gaiden II - Update", nullptr,
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (s.phase == Phase::kAvailable) {
      ImGui::TextWrapped("A new version of Ninja Gaiden II is available.");
      ImGui::Spacing();
      ImGui::Text("You have v%s.  v%s is ready to install.",
                  s.current_version.c_str(), s.latest_version.c_str());
      ImGui::Spacing();
      if (ImGui::Button("Update now")) {
        BeginUpdate();
      }
      ImGui::SameLine();
      if (ImGui::Button(show_notes ? "Hide notes" : "What's new")) {
        show_notes = !show_notes;
      }
      ImGui::SameLine();
      if (ImGui::Button("Later")) {
        Dismiss();
      }
      if (show_notes && !s.notes.empty()) {
        ImGui::Spacing();
        ImGui::BeginChild("notes", ImVec2(0, 180), true);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s.notes.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
      }
    } else if (s.phase == Phase::kDownloading) {
      ImGui::TextWrapped("Downloading v%s...", s.latest_version.c_str());
      ImGui::Spacing();
      char label[64] = "";
      if (s.bytes_total > 0) {
        std::snprintf(label, sizeof(label), "%s / %s",
                      ng2::FormatBytes(s.bytes_done).c_str(),
                      ng2::FormatBytes(s.bytes_total).c_str());
      } else {
        std::snprintf(label, sizeof(label), "%s",
                      ng2::FormatBytes(s.bytes_done).c_str());
      }
      ImGui::ProgressBar(s.bytes_total > 0 ? static_cast<float>(s.progress) : 0.0f,
                         ImVec2(-1.0f, 0.0f), label);
      ImGui::Spacing();
      if (ImGui::Button("Hide")) {
        Dismiss();  // keeps downloading; finish from the settings menu
      }
    } else if (s.phase == Phase::kReadyToApply) {
      ImGui::TextWrapped(
          "v%s has been downloaded. The game will close, install it, and "
          "reopen.",
          s.latest_version.c_str());
      ImGui::Spacing();
      if (ImGui::Button("Restart now")) {
        ApplyNow();
      }
      ImGui::SameLine();
      if (ImGui::Button("Later")) {
        Dismiss();
      }
    } else if (s.phase == Phase::kApplying) {
      ImGui::TextWrapped("Installing v%s... the game will restart.",
                         s.latest_version.c_str());
    } else if (s.phase == Phase::kError) {
      ImGui::TextWrapped("The update could not be installed:");
      ImGui::Spacing();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(s.error.c_str());
      ImGui::PopTextWrapPos();
      ImGui::Spacing();
      if (ImGui::Button("Close")) {
        Dismiss();
      }
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

}  // namespace update
}  // namespace ng2
