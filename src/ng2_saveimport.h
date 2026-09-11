// Importing an Xbox 360 save package.
//
// Saves arrive as STFS containers ("CON " for a console-signed one). The SDK's
// ContentManager can already extract those - it is what installs the DLC - but
// it puts the result under content type 00000002 and XUID 0, which is where
// downloadable content lives. A save has to be under the signed-in profile's
// XUID and content type 00000001 or the game never sees it.
//
// So importing is: extract with the SDK, then move the result to the save
// slot. Both were worked out by doing it by hand first and watching LOAD GAME
// go from greyed out to selectable.
//
// This runs after the runtime exists (OnPostSetup) but before the guest starts,
// so a save chosen on the setup screen is already in place by the time the
// game looks for one.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ng2 {

// What a package is, read from its own header. `ok` is false when the file is
// not an STFS container at all.
struct SaveInfo {
  bool ok = false;
  bool right_game = false;   // title id matches Ninja Gaiden II
  bool is_save = false;      // content type is 00000001, a saved game
  uint32_t title_id = 0;
  uint32_t content_type = 0;
  std::string display_name;      // UTF-8, for showing: "Auto / Ch. 4(5) / ..."
  std::u16string display_name_u16;  // what goes back into the content header
  std::string message;           // one line, ready to show
};

SaveInfo InspectSavePackage(const std::filesystem::path& path);

// Every Ninja Gaiden II save in a folder, searched recursively. Anything that
// is not one is skipped silently: a save pack is a folder of mixed files, and
// the useful answer is how many saves are in it, not a complaint per file.
std::vector<std::filesystem::path> FindSavePackages(
    const std::filesystem::path& folder);

// Remember packages to import when the runtime is up. The setup screen runs
// before there is a ContentManager to import with, so it queues instead.
// Queueing replaces whatever was queued before, so choosing a second folder
// does not silently import the first one as well.
void QueueSaveImports(std::vector<std::filesystem::path> packages);
std::vector<std::filesystem::path> TakeQueuedSaveImports();

// Extracts `package` and moves it into the profile's save slot. Returns false
// with `message` set on failure; both are safe to show.
bool ImportSave(const std::filesystem::path& package, std::string& message);

}  // namespace ng2
