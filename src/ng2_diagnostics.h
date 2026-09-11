// One file to attach to a bug report.
//
// A useful report needs the log, the settings, and what the machine is - and
// asking someone to find three files in two folders is asking for a report with
// none of them. This gathers them into a single text file, puts its path on the
// clipboard and opens the folder, so the whole job is one button and one drag.
//
// Plain text rather than a zip: everything worth sending is already text, a zip
// would need either a library the SDK does not ship or a PowerShell child
// process, and a .txt can be read in the browser by whoever picks the report up.

#pragma once

#include <filesystem>
#include <string>

namespace ng2 {

struct DiagnosticsResult {
  bool ok = false;
  std::filesystem::path file;  // what was written
  std::string error;           // why not, when ok is false
};

// Writes the bundle beside the executable, under diagnostics/. `settings_file`
// and `log_dir` are passed in rather than looked up so this stays testable and
// so a launch with non-default paths reports the files it actually used.
DiagnosticsResult WriteDiagnostics(const std::filesystem::path& settings_file,
                                   const std::filesystem::path& log_dir);

// Puts `text` on the Windows clipboard. False if the clipboard could not be
// opened, which is normal when another process is holding it.
bool CopyToClipboard(const std::string& text);

// Opens the containing folder with the file selected.
void RevealInExplorer(const std::filesystem::path& file);

// Diagnostics that live in diag_hooks.cpp.
//
// These were declared in ng2_cheats.h, which is gone - they were never cheats,
// they simply shared a header with them. Definitions are unmoved.
void StartGuestProfiler();

// F7: find guest floats that are oscillating right now.
void ScanOscillating();
void StartOscillationLoop();

}  // namespace ng2
