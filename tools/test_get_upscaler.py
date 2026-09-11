"""Adversarial checks on the upscaler downloader.

    python tools/test_get_upscaler.py

The downloader fetches a third-party archive over the network and unpacks it,
which is the exact shape of two well-known weaknesses. Both were present in the
first version of that file, so these are regression tests, not theatre: each one
is written so that it FAILS if the guard is removed.
"""

import io
import os
import shutil
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import get_upscaler as G  # noqa: E402

FAILURES = []


def check(name, ok, detail=""):
    print("  %-58s %s%s" % (name, "PASS" if ok else "FAIL",
                            "" if ok else "  <- " + detail))
    if not ok:
        FAILURES.append(name)


def zip_with(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in entries:
            z.writestr(name, data)
    buf.seek(0)
    return zipfile.ZipFile(buf)


def test_extract():
    print("zip slip (CWE-22):")
    root = tempfile.mkdtemp(prefix="ng2zs_")
    dest = os.path.join(root, "upscaler")
    os.makedirs(dest)
    outside = os.path.join(root, "escaped.txt")

    # A benign archive must still unpack, or the guard is useless.
    with zip_with([("realesrgan-ncnn-vulkan.exe", b"ok"),
                   ("models/x.bin", b"m")]) as z:
        try:
            G.safe_extract(z, dest)
            good = (os.path.isfile(os.path.join(dest, "realesrgan-ncnn-vulkan.exe")) and
                    os.path.isfile(os.path.join(dest, "models", "x.bin")))
        except Exception as exc:                                  # noqa: BLE001
            good, _ = False, exc
    check("a normal archive still extracts", good)

    for evil in ("../escaped.txt",
                 "../../escaped.txt",
                 "models/../../escaped.txt"):
        with zip_with([(evil, b"pwned")]) as z:
            try:
                G.safe_extract(z, dest)
                blocked = False
            except ValueError:
                blocked = True
            except Exception:                                     # noqa: BLE001
                blocked = True
        check("refuses traversal entry %-28s" % evil,
              blocked and not os.path.exists(outside),
              "wrote outside the destination")

    shutil.rmtree(root, ignore_errors=True)


def test_url():
    print("download location:")
    ok_urls = ["https://github.com/x/y/releases/download/v1/a.zip",
               "https://objects.githubusercontent.com/a/b"]
    for u in ok_urls:
        try:
            G.check_url(u)
            passed = True
        except Exception:                                         # noqa: BLE001
            passed = False
        check("accepts %s" % u[:46], passed)

    bad_urls = ["http://github.com/x/y.zip",                 # not TLS
                "https://evil.example.com/x.zip",            # wrong host
                "https://github.com.evil.com/x.zip",         # suffix trick
                "file:///C:/Windows/System32/evil.dll"]      # not even http
    for u in bad_urls:
        try:
            G.check_url(u)
            blocked = False
        except ValueError:
            blocked = True
        check("refuses %s" % u[:46], blocked, "accepted a bad URL")


def main():
    test_extract()
    test_url()
    print()
    if FAILURES:
        print("%d FAILED: %s" % (len(FAILURES), ", ".join(FAILURES)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
