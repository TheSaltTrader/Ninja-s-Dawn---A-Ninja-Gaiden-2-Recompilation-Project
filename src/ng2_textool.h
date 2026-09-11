// Driving the texture-pack tools from the app.
//
// The work itself lives in tools/upscale_textures.py and tools/get_upscaler.py
// rather than being reimplemented here: decoding the guest's tiled formats and
// running an upscaler are both jobs with real libraries behind them in Python
// and none in this process. The app's part is to find an interpreter, run a
// script without flashing a console over the game, and turn its output into a
// progress bar.
//
// This was once part of a module that also converted the game's videos, back
// when the guest's decoder was broken. That decoder is fixed and the video half
// is gone, so what is left is only the texture side - and it needs Python only,
// never ffmpeg.

#pragma once

#include <filesystem>
#include <string>
#include <thread>

#include "ng2_disc.h"  // ExtractProgress

namespace ng2 {

// What is needed to run the texture scripts, and what was found.
struct TextureTools {
  std::string python;  // the interpreter to use, empty if none was found
};

TextureTools FindTextureTools();

// How many textures have been dumped, and how many are in the finished pack.
// Both walk the folders rather than trusting a flag, so deleting files by hand
// is noticed.
int CountDumpedTextures(const std::filesystem::path& texture_dir);
int CountPackedTextures(const std::filesystem::path& texture_dir);

// The pack counted the way the tool counts it. "7,778 dumped, 5,907 in the
// pack" read as 1,871 textures missing; they were HUD, fonts and video frames
// the tool never packs by design. So the dump is classified here with the
// same rule the tool uses (pack_reason in tools/upscale_textures.py) and the
// numbers shown are the ones that can be enhanced: how many there are, how
// many are in the pack, how many are still waiting.
struct PackCensus {
  int dumped = 0;      // unique textures with a raw dump or a decoded PNG
  int candidates = 0;  // of those, the ones the tool packs - art, not HUD or video
  int packed = 0;      // candidates present in the pack
  int waiting = 0;     // candidates not yet in the pack
  int excluded = 0;    // dumped but never packed, by design
  int tex_files = 0;   // .tex files in the pack, whatever they belong to

  // What the pack was made with, from pack/pack.txt (written by the tool).
  // The menu compares these with the settings it shows: a pack made with
  // other settings needs every texture redone, not just the missing ones.
  bool manifest = false;
  int pack_scale = 0;
  std::string pack_upscaler;   // "lanczos" or "realesrgan"
  float pack_strength = 0.0f;
  bool pack_complete = true;   // false = the last run was stopped halfway
};
PackCensus CountPack(const std::filesystem::path& texture_dir);

// Whether the Real-ESRGAN executable has been downloaded into
// <texture_dir>/upscaler. The AI option stays disabled until it has.
bool UpscalerInstalled(const std::filesystem::path& texture_dir);

// Fetches it from the official Real-ESRGAN releases. Deliberately not shipped
// with the port: 43 MB of third-party binary under its own licence, and whether
// to have it is the player's call.
std::thread DownloadUpscalerAsync(const TextureTools& tools,
                                  const std::filesystem::path& texture_dir,
                                  ExtractProgress& progress);

// Turns the dump into a pack. Returns a joinable thread; the caller owns it and
// must join before `progress` dies. With `only_missing` the textures already
// in the pack are left alone and only the rest are decoded and upscaled.
std::thread UpscaleTexturesAsync(const TextureTools& tools,
                                 const std::filesystem::path& texture_dir,
                                 bool upscale, int scale, bool ai,
                                 float ai_strength, bool only_missing,
                                 ExtractProgress& progress);

}  // namespace ng2
