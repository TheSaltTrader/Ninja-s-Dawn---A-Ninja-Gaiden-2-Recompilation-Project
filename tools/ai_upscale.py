"""Real-ESRGAN upscaling, in batches, with every output checked.

Used by upscale_textures.py and repack_from_decoded.py; not run directly.

Two things here are not optional, both learned by measuring:

**Every output is validated.** Running 30 textures on a busy GPU produced
`vkQueueSubmit failed -4` (device lost) partway through, after which the tool
carried on, wrote a file for every remaining texture, and exited successfully -
16 of the 30 files were blank. Trusting "the process returned 0 and the files
exist" would have packed 16 empty textures into the game. The same batch on an
idle GPU still produced one blank, so this is not a quirk of one card being
busy; it is how this tool fails.

**Work is done in chunks with retry.** A device loss kills the rest of a batch,
so a big batch turns one transient failure into hundreds of blanks. Small
chunks bound the damage, a retry usually clears it, and anything still failing
falls back to Lanczos rather than shipping a hole.

The AI result is BLENDED with the plain resize. Real-ESRGAN sharpens and
invents plausible micro-detail; at full strength it also smooths away grain and
re-draws edges, which on a game texture reads as "someone replaced the art".
Blending keeps the original's character and takes the added definition.
"""

import os
import shutil
import subprocess
import tempfile

from PIL import Image

# Model names as the standalone build knows them.
MODEL_DEFAULT = "realesrgan-x4plus"
MODELS = ("realesrgan-x4plus", "realesrgan-x4plus-anime", "realesr-animevideov3-x4")

# The scale each model actually produces. The x4plus models are 4x networks
# and nothing else: asked for "-s 2" the ncnn tool still runs the 4x network
# and then assembles its tiles into a 2x canvas, which comes out as the picture
# shifted, repeated and mostly black (measured: a starburst texture came back
# with a maximum value of 26 out of 255). So the model is always run at its
# own scale and the result resized down to what was asked for - which is what
# Real-ESRGAN's own reference script does for an "outscale" below the model's.
# The animevideov3 family ships separate x2/x3/x4 networks and is not listed.
NATIVE_SCALE = {"realesrgan-x4plus": 4, "realesrgan-x4plus-anime": 4}

CHUNK = 40          # textures per invocation
RETRIES = 2


def find_upscaler(root):
    """The downloaded executable, or None. Mirrors tools/get_upscaler.py."""
    base = os.path.join(root, "upscaler")
    direct = os.path.join(base, "realesrgan-ncnn-vulkan.exe")
    if os.path.isfile(direct):
        return direct
    for cur, _d, files in os.walk(base):
        if "realesrgan-ncnn-vulkan.exe" in files:
            return os.path.join(cur, "realesrgan-ncnn-vulkan.exe")
    return None


def _looks_blank(img):
    """A device-lost output is uniform. Real textures essentially never are -
    and one that genuinely is loses nothing by being resized instead."""
    ex = img.convert("RGB").getextrema()
    return all(lo == hi for lo, hi in ex)


def _run(exe, in_dir, out_dir, model, scale, gpu):
    cmd = [exe, "-i", in_dir, "-o", out_dir, "-n", model, "-s", str(scale)]
    if gpu is not None:
        cmd += ["-g", str(gpu)]
    # The tool writes progress percentages to stderr; nothing there is worth
    # relaying, and its exit code does not reflect a lost device anyway.
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       cwd=os.path.dirname(exe), timeout=1800, check=False)
    except subprocess.TimeoutExpired:
        return False
    return True


def upscale_many(exe, images, scale, model=MODEL_DEFAULT, gpu=0, log=print):
    """images: list of (key, PIL.Image). Returns {key: PIL.Image} at `scale`.

    Every key in `images` is present in the result: any texture the model could
    not produce a valid output for comes back as a Lanczos resize, so the caller
    never has to think about holes.
    """
    out = {}
    todo = list(images)
    native = NATIVE_SCALE.get(model, scale)
    for start in range(0, len(todo), CHUNK):
        chunk = todo[start:start + CHUNK]
        remaining = {k: im for k, im in chunk}

        for attempt in range(RETRIES + 1):
            if not remaining:
                break
            tmp = tempfile.mkdtemp(prefix="ng2ai_")
            in_dir, out_dir = os.path.join(tmp, "in"), os.path.join(tmp, "out")
            os.makedirs(in_dir); os.makedirs(out_dir)
            order = {}
            for i, (k, im) in enumerate(remaining.items()):
                name = "%04d.png" % i
                order[name] = k
                im.convert("RGBA").save(os.path.join(in_dir, name))

            _run(exe, in_dir, out_dir, model, native, gpu)

            still = {}
            for name, k in order.items():
                p = os.path.join(out_dir, name)
                ok = False
                if os.path.isfile(p):
                    try:
                        res = Image.open(p)
                        res.load()
                        src = remaining[k]
                        want = (src.width * scale, src.height * scale)
                        got = (src.width * native, src.height * native)
                        ok = res.size == got and not _looks_blank(res)
                        if ok and native != scale:
                            res = res.resize(want, Image.Resampling.LANCZOS)
                        if ok:
                            out[k] = res.convert("RGBA")
                    except Exception:                             # noqa: BLE001
                        ok = False
                if not ok:
                    still[k] = remaining[k]
            shutil.rmtree(tmp, ignore_errors=True)

            if still and attempt < RETRIES:
                log("  %d of %d failed the check, retrying" % (len(still), len(remaining)))
            remaining = still

        # Anything that survived every retry gets resized instead. A blank
        # texture in the pack is far worse than one that is merely not enhanced.
        for k, im in remaining.items():
            log("  giving up on %s after %d attempts - using a plain resize" % (k, RETRIES + 1))
            out[k] = im.convert("RGBA").resize(
                (im.width * scale, im.height * scale), Image.Resampling.LANCZOS)
    return out


def blend(ai_img, src_img, strength, radius=1.6):
    """Detail from the model, tone from the original.

    NOT a straight cross-fade, and the difference matters. Real-ESRGAN x4plus is
    photo-trained and denoises hard: on a smoke texture it cut the mean RGB from
    11.3 to 6.7, roughly 40% darker, because faint wisps against black are
    exactly what it treats as noise. Cross-fading that just darkens less.

    So the model's output is high-pass filtered - only what it ADDED at fine
    scale - and laid over a plain resize of the original. The base tone, colour
    and overall brightness stay the game's; the extra definition is the model's.
    That is the brief: more detail, not different art.

    strength scales the detail layer. 0 leaves a plain resize.

    Alpha always comes from the resize. Real-ESRGAN is an RGB model, and an
    invented alpha edge shows as a halo around every cut-out.
    """
    import numpy as np
    from PIL import ImageFilter

    target = (ai_img.width, ai_img.height)
    plain = src_img.convert("RGBA").resize(target, Image.Resampling.LANCZOS)
    if strength <= 0.001:
        return plain

    ai_rgb = ai_img.convert("RGB")
    base = np.asarray(plain.convert("RGB"), dtype=np.float32)
    hi = np.asarray(ai_rgb, dtype=np.float32) - np.asarray(
        ai_rgb.filter(ImageFilter.GaussianBlur(radius)), dtype=np.float32)

    out = np.clip(base + hi * float(strength), 0.0, 255.0).astype(np.uint8)
    res = Image.fromarray(out, "RGB").convert("RGBA")
    res.putalpha(plain.getchannel("A"))
    return res
