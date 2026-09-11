#include "ng2_disc.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

#include "ng2_platform.h"

namespace fs = std::filesystem;
namespace rfs = rex::filesystem;

namespace ng2 {
namespace {

uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) |
         uint32_t(p[3]);
}

// XEX2 identity, straight out of the optional header table.
//
//   0x00  'XEX2'
//   0x14  optional header count
//   0x18  {key, value} pairs, big-endian
//
// Key 0x00040006 is XEX_HEADER_EXECUTION_INFO. Its low byte (6) is the size in
// dwords, which means `value` is a file offset rather than an inline value.
// The struct is media_id, version, base_version, title_id, then a byte each of
// platform / executable table / disc number / disc count.
//
// Verified against this disc: media 69F555CB, title 544307D5, disc 1 of 1.
struct XexIdentity {
  bool ok = false;
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;
};

XexIdentity ParseXexIdentity(const uint8_t* data, size_t size) {
  XexIdentity id;
  if (size < 0x18 || std::memcmp(data, "XEX2", 4) != 0)
    return id;
  const uint32_t count = ReadBE32(data + 0x14);
  // A plausible table only; a corrupt count must not walk off the buffer.
  if (count > 256 || 0x18 + size_t(count) * 8 > size)
    return id;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = data + 0x18 + size_t(i) * 8;
    if (ReadBE32(entry) != 0x00040006u)
      continue;
    const uint32_t offset = ReadBE32(entry + 4);
    if (size_t(offset) + 20 > size)
      return id;
    id.media_id = ReadBE32(data + offset);
    id.title_id = ReadBE32(data + offset + 0x0C);
    id.disc_number = data[offset + 0x12];
    id.disc_count = data[offset + 0x13];
    id.ok = true;
    return id;
  }
  return id;
}

// How much of the XEX has to be in hand for the header walk. The execution
// info on this disc sits at 0x5B44, well past the first page - reading only
// 0x1000 bytes would find the table and then fall off the end of the buffer.
constexpr size_t kXexHeaderBytes = 0x20000;

void CollectFiles(rfs::Entry* dir, const std::string& prefix,
                  std::vector<std::pair<std::string, rfs::Entry*>>& out) {
  for (const auto& child : dir->children()) {
    std::string path = prefix.empty() ? child->name() : prefix + "/" + child->name();
    if (child->attributes() & rfs::kFileAttributeDirectory) {
      CollectFiles(child.get(), path, out);
    } else {
      out.emplace_back(std::move(path), child.get());
    }
  }
}

std::string DescribeTitle(const XexIdentity& id) {
  if (!id.ok)
    return "unknown title";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "title %08X", id.title_id);
  if (id.title_id == kNg2TitleId)
    return "Ninja Gaiden II";
  return buf;
}

}  // namespace

DiscInfo InspectDisc(const fs::path& iso_path) {
  DiscInfo info;
  std::error_code ec;
  if (!fs::is_regular_file(iso_path, ec)) {
    info.message = "Not a file.";
    return info;
  }

  // Mount path "" - this device is never registered with the VFS, it is only
  // read directly, exactly as re:Blue's installer opens its discs.
  rfs::DiscImageDevice disc("", iso_path);
  if (!disc.Initialize()) {
    info.message = "Not a readable Xbox 360 disc image.";
    return info;
  }
  info.opened = true;
  info.file_count = disc.file_count();
  info.total_bytes = disc.total_file_size();

  auto* xex = disc.ResolvePath("default.xex");
  if (xex == nullptr) {
    info.message = "Disc has no default.xex.";
    return info;
  }
  info.has_xex = true;

  auto mapped = static_cast<rfs::DiscImageEntry*>(xex)->OpenMapped(
      rex::memory::MappedMemory::Mode::kRead, 0, 0);
  if (mapped) {
    const auto id =
        ParseXexIdentity(mapped->data(), std::min(mapped->size(), kXexHeaderBytes));
    info.title_id = id.title_id;
    info.media_id = id.media_id;
    info.disc_number = id.disc_number;
    info.disc_count = id.disc_count;
    info.message = DescribeTitle(id) + ", " + std::to_string(info.file_count) +
                   " files, " + FormatBytes(info.total_bytes);
  } else {
    info.message = "default.xex could not be read.";
  }
  return info;
}

DiscInfo InspectFolder(const fs::path& folder) {
  DiscInfo info;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) {
    info.message = "Not a folder.";
    return info;
  }
  info.opened = true;

  const auto xex = folder / "default.xex";
  if (!fs::is_regular_file(xex, ec)) {
    info.message = "No default.xex in this folder.";
    return info;
  }
  info.has_xex = true;

  std::ifstream in(xex, std::ios::binary);
  std::vector<uint8_t> buffer(kXexHeaderBytes);
  in.read(reinterpret_cast<char*>(buffer.data()),
          static_cast<std::streamsize>(buffer.size()));
  const auto id = ParseXexIdentity(buffer.data(), static_cast<size_t>(in.gcount()));
  info.title_id = id.title_id;
  info.media_id = id.media_id;
  info.disc_number = id.disc_number;
  info.disc_count = id.disc_count;

  // Totals for the folder, so the two routes read alike.
  for (auto it = fs::recursive_directory_iterator(
           folder, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (it->is_regular_file(ec)) {
      ++info.file_count;
      info.total_bytes += it->file_size(ec);
    }
  }
  info.message = DescribeTitle(id) + ", " + std::to_string(info.file_count) +
                 " files, " + FormatBytes(info.total_bytes);
  return info;
}

bool IsStfsPackage(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return false;
  if (fs::file_size(path, ec) < 0x1000)
    return false;
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {};
  if (!in.read(magic, sizeof(magic)))
    return false;
  return std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "PIRS", 4) == 0 ||
         std::memcmp(magic, "CON ", 4) == 0;
}

int CountStfsPackages(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return 0;
  int count = 0;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (IsStfsPackage(it->path()))
      ++count;
  }
  return count;
}

std::thread ExtractDiscAsync(const fs::path& iso_path, const fs::path& dest,
                             ExtractProgress& progress) {
  progress.Reset();
  progress.running = true;
  return std::thread([iso_path, dest, &progress] {
    auto fail = [&progress](const std::string& why) {
      progress.SetError(why);
      progress.failed = true;
      progress.complete = true;
      progress.running = false;
    };

    rfs::DiscImageDevice disc("", iso_path);
    if (!disc.Initialize()) {
      fail("Could not open " + iso_path.filename().string());
      return;
    }

    auto* root = const_cast<rfs::Entry*>(disc.root());
    if (root == nullptr) {
      fail("Disc has no root directory.");
      return;
    }

    std::vector<std::pair<std::string, rfs::Entry*>> files;
    CollectFiles(root, {}, files);
    progress.files_total = files.size();
    uint64_t total = 0;
    for (const auto& [path, entry] : files)
      total += entry->size();
    progress.bytes_total = total;

    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
      fail("Cannot create " + dest.string());
      return;
    }

    for (const auto& [rel_path, entry] : files) {
      if (progress.cancel) {
        progress.SetError("Cancelled.");
        progress.failed = true;
        break;
      }
      progress.SetCurrentFile(rel_path);

      const fs::path out_path = dest / fs::path(rel_path).make_preferred();
      const uint64_t size = entry->size();

      // Resume: a file already there at the right size is not re-copied. This
      // is what makes a cancelled install worth restarting, and it is also
      // what lets the screen point at an existing extraction and only fill in
      // what is missing.
      if (fs::exists(out_path, ec) && fs::file_size(out_path, ec) == size) {
        progress.bytes_done += size;
        ++progress.files_done;
        continue;
      }

      fs::create_directories(out_path.parent_path(), ec);
      auto mapped = static_cast<rfs::DiscImageEntry*>(entry)->OpenMapped(
          rex::memory::MappedMemory::Mode::kRead, 0, 0);
      if (!mapped) {
        fail("Could not read " + rel_path + " from the disc.");
        return;
      }
      std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
      if (!out) {
        fail("Could not write " + out_path.string());
        return;
      }
      out.write(reinterpret_cast<const char*>(mapped->data()),
                static_cast<std::streamsize>(size));
      out.close();
      if (!out) {
        fail("Write failed for " + out_path.string() + " (disk full?)");
        return;
      }
      progress.bytes_done += size;
      ++progress.files_done;
    }

    if (!progress.failed) {
      REXLOG_INFO("Install: extracted {} files ({}) to {}",
                  progress.files_done.load(), FormatBytes(progress.bytes_done.load()),
                  dest.string());
    }
    progress.complete = true;
    progress.running = false;
  });
}

}  // namespace ng2
