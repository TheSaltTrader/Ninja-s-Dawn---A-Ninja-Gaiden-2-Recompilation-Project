#include "ng2_saveimport.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace ng2 {
namespace {

constexpr uint32_t kTitleId = 0x544307D5;

// The display name in an STFS package.
//
// The documented XContentMetadata layout puts it at 0x3ED, which is wrong for
// these files: every one of the nineteen saves to hand carries it at 0x411,
// big-endian, in one of nine 128-character locale slots. The offset is odd, so
// reading it as aligned 16-bit words gives back mojibake that reads convincingly
// as a different byte order - which is exactly the wrong turn taken first.
//
// Which slot is used varies: a Japanese save fills slot 1 and leaves slot 0
// empty, an English one fills slot 0. So the first non-empty slot wins rather
// than a fixed one.
constexpr size_t kNameOffset = 0x411;
constexpr size_t kNameSlot = 256;  // 128 UTF-16 characters
constexpr size_t kNameSlots = 9;   // one per locale

// Big-endian UTF-16 out of a buffer, stopping at the first NUL.
std::u16string ReadName(const std::vector<uint8_t>& d, size_t offset) {
  std::u16string out;
  for (size_t i = 0; i + 1 < kNameSlot; i += 2) {
    const size_t at = offset + i;
    if (at + 1 >= d.size())
      break;
    const char16_t c =
        static_cast<char16_t>((uint16_t(d[at]) << 8) | uint16_t(d[at + 1]));
    if (c == 0)
      break;
    out.push_back(c);
  }
  return out;
}

// Enough UTF-8 to put a name through ImGui and the log. The names are mostly
// ASCII with a Japanese word or two in them, so this is a real conversion.
std::string ToUtf8(const std::u16string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    uint32_t c = in[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < in.size() &&
        in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
    }
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (c >> 18)));
      out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

uint32_t ReadBe32(const std::vector<uint8_t>& d, size_t at) {
  if (at + 4 > d.size())
    return 0;
  return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) |
         (uint32_t(d[at + 2]) << 8) | uint32_t(d[at + 3]);
}

std::mutex g_queue_mutex;
std::vector<std::filesystem::path> g_queued;

}  // namespace

SaveInfo InspectSavePackage(const std::filesystem::path& path) {
  SaveInfo info;

  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    info.message = "That is not a file.";
    return info;
  }

  // Only the metadata block is needed, and a save is 88 KB, so this reads a
  // bounded prefix rather than the whole package.
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) {
    info.message = "Could not open that file.";
    return info;
  }
  std::vector<uint8_t> d(0x1400);
  const size_t got = std::fread(d.data(), 1, d.size(), f);
  std::fclose(f);
  d.resize(got);

  if (got < 0x1000 || (std::memcmp(d.data(), "CON ", 4) != 0 &&
                       std::memcmp(d.data(), "LIVE", 4) != 0 &&
                       std::memcmp(d.data(), "PIRS", 4) != 0)) {
    info.message = "Not an Xbox 360 content package.";
    return info;
  }

  info.ok = true;
  info.content_type = ReadBe32(d, 0x344);
  info.title_id = ReadBe32(d, 0x360);
  info.right_game = info.title_id == kTitleId;
  info.is_save = info.content_type == 0x00000001;

  for (size_t slot = 0; slot < kNameSlots; ++slot) {
    info.display_name_u16 = ReadName(d, kNameOffset + slot * kNameSlot);
    if (!info.display_name_u16.empty())
      break;
  }
  info.display_name = ToUtf8(info.display_name_u16);

  if (!info.right_game) {
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "That save is for another game (title %08X).", info.title_id);
    info.message = buf;
  } else if (!info.is_save) {
    info.message = "That is Ninja Gaiden II content, but not a saved game.";
  } else if (info.display_name.empty()) {
    info.message = path.filename().string();
  } else {
    info.message = info.display_name;
  }
  return info;
}

std::vector<std::filesystem::path> FindSavePackages(
    const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> found;
  std::error_code ec;

  // A single file is accepted too. Someone who points this at one save rather
  // than at the folder holding it has said what they meant clearly enough.
  if (std::filesystem::is_regular_file(folder, ec)) {
    const SaveInfo info = InspectSavePackage(folder);
    if (info.ok && info.right_game && info.is_save)
      found.push_back(folder);
    return found;
  }

  if (!std::filesystem::is_directory(folder, ec))
    return found;

  for (auto& entry :
       std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (ec)
      break;
    if (!entry.is_regular_file(ec))
      continue;
    const SaveInfo info = InspectSavePackage(entry.path());
    if (info.ok && info.right_game && info.is_save)
      found.push_back(entry.path());
  }
  // Sorted so the order is the folder's, not the filesystem's - the saves are
  // numbered, and importing them in a jumbled order makes the log harder to
  // read for no gain.
  std::sort(found.begin(), found.end());
  return found;
}

void QueueSaveImports(std::vector<std::filesystem::path> packages) {
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  g_queued = std::move(packages);
}

std::vector<std::filesystem::path> TakeQueuedSaveImports() {
  std::lock_guard<std::mutex> lock(g_queue_mutex);
  std::vector<std::filesystem::path> out;
  out.swap(g_queued);
  return out;
}

bool ImportSave(const std::filesystem::path& package, std::string& message) {
  using rex::system::XContentType;
  using rex::system::xam::XCONTENT_AGGREGATE_DATA;

  const SaveInfo info = InspectSavePackage(package);
  if (!info.ok || !info.right_game || !info.is_save) {
    message = info.message.empty() ? "That file is not a Ninja Gaiden II save."
                                   : info.message;
    return false;
  }

  auto* kernel = REX_KERNEL_STATE();
  auto* content = kernel ? kernel->content_manager() : nullptr;
  if (content == nullptr) {
    message = "The runtime is not ready to take a save yet.";
    return false;
  }
  auto* profile = kernel->user_profile();
  const uint64_t xuid = profile ? profile->xuid() : 0;
  if (xuid == 0) {
    message = "There is no signed-in profile to import the save into.";
    return false;
  }

  // The package's own file name is the content's name: these packages carry no
  // name of their own in their metadata - searched for, and genuinely not
  // there - and the single file inside each one is named the same as the
  // package. Renaming a save before importing it would hand the game a save it
  // does not recognise, so the name is kept exactly as it arrived.
  const std::string name = package.filename().string();

  // Extract. This always lands in the downloadable-content slot - XUID 0,
  // content type 00000002 - because that is what InstallContent is for. The
  // move below is what turns it into a save.
  const auto rc = content->InstallContent(package);
  if (static_cast<uint32_t>(rc) != 0) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Could not read that save (0x%08X).",
                  static_cast<uint32_t>(rc));
    message = buf;
    return false;
  }

  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = XContentType::kSavedGame;
  data.set_display_name(info.display_name_u16);
  data.set_file_name(name);
  data.xuid = xuid;
  data.title_id = kTitleId;

  // Where the save has to end up. ContentManager has no accessor for its own
  // root, so the destination is asked for rather than assembled: CreateContent
  // makes the directory and GetOpenPackagePath names it. That path is also how
  // the root itself is found, four components up.
  const std::string root_name = "ng2import";
  content->CreateContent(root_name, xuid, data);
  std::filesystem::path dest = content->GetOpenPackagePath(root_name);
  if (dest.empty()) {
    // Already imported once: CreateContent refuses, but opening works.
    uint32_t license = 0;
    content->OpenContent(root_name, xuid, data, license);
    dest = content->GetOpenPackagePath(root_name);
  }
  content->CloseContent(root_name);
  if (dest.empty()) {
    message = "Could not work out where saves are kept.";
    return false;
  }

  // <root>/<xuid>/<title id>/<content type>/<name>
  const auto root =
      dest.parent_path().parent_path().parent_path().parent_path();
  char title_dir[16];
  std::snprintf(title_dir, sizeof(title_dir), "%08X", kTitleId);
  const auto src = root / "0000000000000000" / title_dir / "00000002" / name;

  std::error_code ec;
  if (!std::filesystem::is_directory(src, ec)) {
    message = "The save extracted, but not where it was expected.";
    REXLOG_WARN("Save import: nothing at {}", src.string());
    return false;
  }

  std::filesystem::create_directories(dest, ec);
  size_t files = 0;
  for (auto& e : std::filesystem::recursive_directory_iterator(src, ec)) {
    const auto rel = std::filesystem::relative(e.path(), src, ec);
    const auto target = dest / rel;
    if (e.is_directory(ec)) {
      std::filesystem::create_directories(target, ec);
      continue;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(e.path(), target,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      message = "Could not write the save into the profile.";
      REXLOG_WARN("Save import: copy {} -> {} failed: {}", e.path().string(),
                  target.string(), ec.message());
      return false;
    }
    ++files;
  }
  if (files == 0) {
    message = "That package is empty.";
    return false;
  }

  // The header is what XAM enumerates, so without it the save is invisible no
  // matter where its bytes are.
  content->WriteContentHeaderFile(xuid, data);

  // Leave nothing behind in the downloadable-content slot: a save sitting
  // there would be offered to the game as DLC on the next boot.
  std::filesystem::remove_all(src, ec);
  std::filesystem::remove(root / "0000000000000000" / title_dir / "Headers" /
                              "00000002" / (name + ".header"),
                          ec);

  REXLOG_INFO("Save import: {} -> {} ({} file(s))", name, dest.string(), files);
  message = info.display_name.empty() ? name : info.display_name;
  return true;
}

}  // namespace ng2
