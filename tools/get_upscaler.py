"""Fetch the Real-ESRGAN upscaler. Nothing here ships with the port.

    python tools/get_upscaler.py --dir C:/ng2tex          # download + extract
    python tools/get_upscaler.py --dir C:/ng2tex --check  # is it already there?

Downloads the official standalone build from xinntao/Real-ESRGAN, NOT the
Python package. That choice matters:

  * the Python route needs torch built for the right CUDA version, which on a
    current card is a multi-gigabyte install that has to match the hardware;
  * this machine's Python already pins onnxruntime-gpu for another project, and
    installing a different build into it would break that project;
  * the standalone build is one executable plus its models, runs on Vulkan (so
    no CUDA version matching at all), and touches nothing outside its folder.

It is a separate download on purpose: it is 43 MB of third-party binary under
its own licence, and whether to have it on the machine is the user's call, not
something a game release should decide for them.

Progress is printed as machine-readable lines for the in-game screen:

    PROGRESS <bytes done> <bytes total> <what>
"""

import argparse
import io
import json
import os
import shutil
import sys
import urllib.request
import zipfile

RELEASES_API = "https://api.github.com/repos/xinntao/Real-ESRGAN/releases"
ASSET_MARKER = "realesrgan-ncnn-vulkan"
ASSET_PLATFORM = "windows"
EXE_NAME = "realesrgan-ncnn-vulkan.exe"


def upscaler_dir(root):
    return os.path.join(root, "upscaler")


def find_exe(root):
    """The executable, wherever the zip happened to put it."""
    base = upscaler_dir(root)
    direct = os.path.join(base, EXE_NAME)
    if os.path.isfile(direct):
        return direct
    for cur, _dirs, files in os.walk(base):
        if EXE_NAME in files:
            return os.path.join(cur, EXE_NAME)
    return None


def latest_asset():
    """Newest release that actually carries the Windows build.

    Not simply /releases/latest: at the time of writing the newest release
    (v0.3.0) has no assets at all, so "latest" has to mean "latest that has the
    thing", or this reports failure while the download plainly exists.
    """
    req = urllib.request.Request(RELEASES_API,
                                 headers={"User-Agent": "ng2recomp-get-upscaler"})
    with urllib.request.urlopen(req, timeout=30) as r:
        releases = json.load(r)
    for rel in releases:                      # the API returns newest first
        for asset in rel.get("assets", []):
            name = asset.get("name", "").lower()
            if ASSET_MARKER in name and ASSET_PLATFORM in name and name.endswith(".zip"):
                return rel.get("tag_name", "?"), asset["name"], asset["browser_download_url"], asset["size"]
    return None, None, None, 0


def safe_extract(zf, dest):
    """extractall, minus the path traversal.

    `ZipFile.extractall` writes whatever path the archive asks for, so an entry
    named "../../../evil.dll" escapes the destination entirely - zip slip,
    CWE-22. This is a third-party archive fetched over the network, which is
    precisely the case the weakness exists for, and the cost of not caring is an
    arbitrary file write outside the game folder.

    Absolute paths, drive letters and anything that resolves outside `dest` are
    refused rather than sanitised: an archive containing one is not an archive
    worth unpacking. Symlink entries are refused for the same reason - they
    redirect a later write just as effectively.
    """
    root = os.path.realpath(dest)
    for member in zf.infolist():
        name = member.filename
        if name.endswith("/"):
            continue
        # S_IFLNK in the high bits of external_attr.
        if (member.external_attr >> 16) & 0xF000 == 0xA000:
            raise ValueError("archive contains a symlink: %s" % name)
        target = os.path.realpath(os.path.join(root, name))
        if target != root and not target.startswith(root + os.sep):
            raise ValueError("archive entry escapes the destination: %s" % name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with zf.open(member) as src, open(target, "wb") as out:
            shutil.copyfileobj(src, out)


def check_url(url):
    """The download location comes from the API response, so it is data rather
    than something we chose. Insist it is HTTPS on a GitHub host before
    fetching: a redirect to plain HTTP, or to somewhere else entirely, is not a
    thing to follow while downloading an executable."""
    from urllib.parse import urlparse
    u = urlparse(url)
    if u.scheme != "https":
        raise ValueError("refusing a non-HTTPS download: %s" % url)
    host = (u.hostname or "").lower()
    allowed = ("github.com", "objects.githubusercontent.com")
    if host not in allowed and not any(host.endswith("." + a) for a in allowed):
        raise ValueError("refusing a download from an unexpected host: %s" % host)
    return url


def download(url, total, label):
    req = urllib.request.Request(url, headers={"User-Agent": "ng2recomp-get-upscaler"})
    buf = io.BytesIO()
    done = 0
    with urllib.request.urlopen(req, timeout=60) as r:
        if total <= 0:
            total = int(r.headers.get("Content-Length") or 0)
        while True:
            chunk = r.read(256 * 1024)
            if not chunk:
                break
            buf.write(chunk)
            done += len(chunk)
            print("PROGRESS %d %d %s" % (done, total, label), flush=True)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="the texture folder; the tool goes in <dir>/upscaler")
    ap.add_argument("--check", action="store_true", help="report whether it is installed and exit")
    args = ap.parse_args()

    if args.check:
        exe = find_exe(args.dir)
        print("INSTALLED %s" % exe if exe else "MISSING")
        return 0 if exe else 1

    try:
        tag, name, url, size = latest_asset()
    except Exception as exc:                                     # noqa: BLE001
        print("FAILED could not reach GitHub: %s" % exc)
        return 1
    if not url:
        print("FAILED no %s %s build found in any release" % (ASSET_MARKER, ASSET_PLATFORM))
        return 1
    print("Found %s (%s), %.1f MB" % (name, tag, size / 1048576.0))

    try:
        blob = download(check_url(url), size, name)
    except Exception as exc:                                     # noqa: BLE001
        print("FAILED download: %s" % exc)
        return 1

    dest = upscaler_dir(args.dir)
    # Replace rather than merge: a half-extracted previous attempt left in
    # place is how you get an executable beside the wrong models.
    if os.path.isdir(dest):
        shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            safe_extract(z, dest)
    except Exception as exc:                                     # noqa: BLE001
        print("FAILED extract: %s" % exc)
        return 1

    exe = find_exe(args.dir)
    if not exe:
        print("FAILED %s is not in the archive" % EXE_NAME)
        return 1
    with open(os.path.join(dest, "VERSION.txt"), "w") as f:
        f.write("%s\n%s\n" % (tag, name))
    print("DONE %s" % exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
