// The one translation unit that compiles stb_image's implementation.
//
// The SDK already exposes `rex::ui::DecodeImageRGBA`, which also wraps
// stb_image - but it is built with STBI_ONLY_PNG (re:Blue's copy of the same
// header is compiled exactly that way). Handed a perfectly good JPEG it
// returns 0x0 and an empty buffer, silently, which is what made the video
// overlay draw nothing at all while every other part of it worked.
//
// So the frames are decoded here instead, with JPEG and nothing else: PNG
// would be lossless and roughly twenty times the size for a 600-frame video.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_JPEG
#include "stb_image.h"
