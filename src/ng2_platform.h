// Host OS bits the settings menu needs: native file pickers and the
// launch-time modifier check.
//
// The SDK ships SDL3, which has SDL_ShowOpenFileDialog, but SDL is linked
// *statically into rexruntime.dll* and none of its symbols are exported (
// checked: `SDL_ShowFileDialogWithProperties` does not appear in
// rexruntime.lib). Linking SDL3-static.lib into the executable as well would
// put a second, uninitialised copy of SDL in the process. This title is
// Windows-only, so the picker talks to IFileOpenDialog directly instead.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ng2 {

// Win32-style filter: a display name and a ';'-separated pattern list.
struct FileFilter {
  const char* name;
  const char* pattern;  // e.g. "*.iso;*.img"
};

// Modal open dialogs. Both return nullopt when the user cancels or the dialog
// cannot be shown; a cancel is not an error and is not logged.
std::optional<std::filesystem::path> PickFile(
    const std::string& title, const std::vector<FileFilter>& filters,
    const std::filesystem::path& start_in);

std::optional<std::filesystem::path> PickFolder(
    const std::string& title, const std::filesystem::path& start_in);

// Whether the window with focus belongs to this process. Used to keep guest
// input from being driven by a background application, without needing the
// window handle - which the SDK does not hand out.
bool ThisProcessIsForeground();

// Whether either Shift key is down. Read once at startup to decide whether to
// force the setup screen open on a launch that would otherwise skip it.
bool ShiftHeld();

// An attached display: the space it can actually show a window in - its work
// area, so the taskbar is already subtracted - in PHYSICAL pixels, plus the
// scaling Windows applies to it.
//
// Both halves are needed together. The runtime's window size is LOGICAL, so a
// window asked for in physical pixels has to be divided by `scale` before it is
// handed over, and a display at 150% can only show a 2560-logical window across
// its 3840 physical pixels.
struct MonitorInfo {
  int index = 0;
  // The whole display - what a person calls its resolution, and what the menu
  // shows beside it.
  int full_width = 0;
  int full_height = 0;
  // What a window can actually occupy: the display minus the taskbar. Smaller
  // than the full size, and the reason a "4K" window opens 72 pixels short.
  int width = 0;
  int height = 0;
  float scale = 1.0f;   // 1.25 at 125%
  bool primary = false;
};

// Every attached display, ordered LEFT TO RIGHT by position. Not primary-first:
// that is what this did before, and it disagreed with the runtime's own display
// indices for every display that was not the primary. Empty if they cannot be
// enumerated, in which case nothing should be clamped - refusing to guess beats
// clamping to a number that came from nowhere.
std::vector<MonitorInfo> Monitors();

// The scaling Windows will ACTUALLY apply to this process's windows.
//
// Not the target monitor's: measured, the window comes out at the PRIMARY
// display's scale wherever it is put, which is what a System-DPI-aware process
// gets. Asking for a size converted with the 4K monitor's own 150% therefore
// produced a window a fifth too small on it.
float SystemScale();

// The PHYSICAL work area of one monitor, and its scaling. Falls back to the
// primary when the index is out of range. False if nothing was determined.
bool MonitorWorkArea(int index, int& width, int& height, float& scale);

// The FULL physical bounds of one monitor (rcMonitor, taskbar included) - the
// area a fullscreen window covers, so the right aspect for the ultrawide FOV.
// Falls back to the primary when the index is out of range.
bool MonitorFullSize(int index, int& width, int& height);

// Starts another copy of this application and does NOT wait for it. Used by
// the game's own "Quit Game", which returns the player to the setup screen by
// relaunching - see Ng2App::ReturnToMenu for why it is a relaunch and not a
// teardown. Returns false if the process could not be started, in which case
// the caller should quit rather than pretend a menu is coming.
bool LaunchDetached(const std::filesystem::path& exe, const std::string& args);

// Human-readable byte count ("6.7 GB"), for the disc and progress readouts.
std::string FormatBytes(uint64_t bytes);

}  // namespace ng2
