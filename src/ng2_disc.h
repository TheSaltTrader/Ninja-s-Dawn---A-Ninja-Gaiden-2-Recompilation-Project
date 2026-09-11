// Reading an Xbox 360 disc image, for the setup screen's "Install from disc".
//
// The runtime cannot mount an ISO as the game data root: ReXApp::ConstructRuntime
// requires `std::filesystem::is_directory(game_data_root)` and rejects a file
// outright ("--game_data_root does not exist", which is what a .iso produces).
// So an ISO has to be extracted to a folder first, exactly as re:Blue's
// installer does. The extraction itself reads through the SDK's
// DiscImageDevice, so GDF/XGD parsing is the SDK's problem, not ours.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ng2 {

// Ninja Gaiden II, from the disc's own XEX execution info. Anything else is
// still installable - the setup screen just says so plainly rather than
// pretending it is the right game.
inline constexpr uint32_t kNg2TitleId = 0x544307D5;

struct DiscInfo {
  bool opened = false;      // the image parsed as a disc at all
  bool has_xex = false;     // default.xex is present at the root
  uint32_t title_id = 0;    // 0 when the XEX could not be read
  uint32_t media_id = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
  uint64_t file_count = 0;
  uint64_t total_bytes = 0;
  std::string message;      // one line, ready to show

  bool IsNg2() const { return title_id == kNg2TitleId; }
  bool Usable() const { return opened && has_xex; }
};

// Opens the image, reads default.xex's execution info and totals the contents.
// Never throws; a failure comes back as `opened == false` with `message` set.
DiscInfo InspectDisc(const std::filesystem::path& iso_path);

// The same identity check against an already-extracted folder, so both routes
// into the setup screen report the game the same way.
DiscInfo InspectFolder(const std::filesystem::path& folder);

// STFS package detection, by magic rather than extension - the DLC files have
// no extension at all. "LIVE" is a signed download, "PIRS"/"CON " the other
// two package signatures.
bool IsStfsPackage(const std::filesystem::path& path);

// How many packages sit under `dir` (recursively). 0 for a missing folder.
int CountStfsPackages(const std::filesystem::path& dir);

struct ExtractProgress {
  std::atomic<uint64_t> files_done{0};
  std::atomic<uint64_t> files_total{0};
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};
  std::atomic<bool> running{false};
  std::atomic<bool> complete{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> cancel{false};

  // Steps of a run that has more than one ("PHASE <i>/<n> <label>" from a
  // script). Each step's bar restarts from zero, so whoever draws it needs to
  // know a restart is a new step rather than a failure, and to time each step
  // by itself. 0 when the run has no steps.
  std::atomic<int> phase{0};
  std::atomic<int> phases{0};

  std::mutex text_mutex;
  std::string current_file;
  std::string error;
  std::string phase_label;

  void SetPhaseLabel(const std::string& l) {
    std::lock_guard lock(text_mutex);
    phase_label = l;
  }
  std::string PhaseLabel() {
    std::lock_guard lock(text_mutex);
    return phase_label;
  }
  void SetCurrentFile(const std::string& f) {
    std::lock_guard lock(text_mutex);
    current_file = f;
  }
  std::string CurrentFile() {
    std::lock_guard lock(text_mutex);
    return current_file;
  }
  void SetError(const std::string& e) {
    std::lock_guard lock(text_mutex);
    error = e;
  }
  std::string Error() {
    std::lock_guard lock(text_mutex);
    return error;
  }
  void Reset() {
    files_done = 0;
    files_total = 0;
    bytes_done = 0;
    bytes_total = 0;
    running = false;
    complete = false;
    failed = false;
    cancel = false;
    phase = 0;
    phases = 0;
    SetCurrentFile({});
    SetError({});
    SetPhaseLabel({});
  }
};

// Extracts every file on the disc into `dest` (flat, mirroring the disc's own
// directory layout). A file already present at the right size is skipped, so
// re-running after a cancel resumes rather than starting over.
//
// Returns a joinable thread; the caller owns it and must join before the
// progress object dies.
std::thread ExtractDiscAsync(const std::filesystem::path& iso_path,
                             const std::filesystem::path& dest,
                             ExtractProgress& progress);

}  // namespace ng2
