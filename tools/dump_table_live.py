"""Make a RUNNING ng2.exe write out its guest->host function table.

The table is what turns a host RIP into a guest function address. The port can
write it itself, but only at startup - and restarting the game to learn the name
of a function we already caught would throw away the reproduction we spent an
hour getting to.

So instead: find the exported dumper inside the already-loaded rexruntime.dll,
allocate a path string in the target, and call it with CreateRemoteThread. No
restart, no rebuild, and the game keeps running afterwards.

    python tools/dump_table_live.py
"""

import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                               wt.DWORD, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                                   ctypes.POINTER(wt.DWORD)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD,
                                     ctypes.POINTER(wt.DWORD)]
psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p,
                                       wt.DWORD]

MEM_COMMIT_RESERVE = 0x3000
PAGE_READWRITE = 0x04


def export_rva(dll_path, want):
    """RVA of an exported symbol, parsed straight out of the PE on disk.

    Loading the DLL into this process to use GetProcAddress would also work but
    runs its DllMain, which is not something to do casually next to a live game.
    """
    data = open(dll_path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, _ = struct.unpack_from("<II", data, dd)

    secs = struct.unpack_from("<H", data, pe + 6)[0]
    sec0 = opt + struct.unpack_from("<H", data, pe + 20)[0]

    def to_off(rva):
        for i in range(secs):
            s = sec0 + i * 40
            va = struct.unpack_from("<I", data, s + 12)[0]
            sz = struct.unpack_from("<I", data, s + 8)[0]
            raw = struct.unpack_from("<I", data, s + 20)[0]
            if va <= rva < va + sz:
                return raw + (rva - va)
        return None

    e = to_off(exp_rva)
    n_names = struct.unpack_from("<I", data, e + 24)[0]
    funcs = struct.unpack_from("<I", data, e + 28)[0]
    names = struct.unpack_from("<I", data, e + 32)[0]
    ords = struct.unpack_from("<I", data, e + 36)[0]
    for i in range(n_names):
        nrva = struct.unpack_from("<I", data, to_off(names) + i * 4)[0]
        off = to_off(nrva)
        end = data.index(b"\0", off)
        if data[off:end].decode() == want:
            o = struct.unpack_from("<H", data, to_off(ords) + i * 2)[0]
            return struct.unpack_from("<I", data, to_off(funcs) + o * 4)[0]
    return None


def main():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq ng2.exe", "/FO", "CSV"],
                         capture_output=True, text=True).stdout
    lines = out.splitlines()
    if len(lines) < 2:
        print("no running ng2.exe")
        return 1
    pid = int([p.strip('"') for p in lines[1].split('","')][1])
    h = k32.OpenProcess(0x1F0FFF, False, pid)
    print("target PID %d" % pid)

    mods = (ctypes.c_void_p * 512)()
    need = wt.DWORD()
    psapi.EnumProcessModules(h, ctypes.byref(mods), ctypes.sizeof(mods),
                             ctypes.byref(need))
    rt_base = rt_path = None
    for i in range(need.value // ctypes.sizeof(ctypes.c_void_p)):
        buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleFileNameExW(h, mods[i], buf, 260)
        if buf.value.lower().endswith("rexruntime.dll"):
            rt_base, rt_path = mods[i], buf.value
            break
    if not rt_base:
        print("rexruntime.dll not loaded")
        return 1
    print("rexruntime.dll at %016X\n  %s" % (rt_base, rt_path))

    rva = export_rva(rt_path, "Ng2DumpFunctionTableC")
    if rva is None:
        print("!! this runtime does not export Ng2DumpFunctionTableC")
        return 1
    proc = rt_base + rva
    print("Ng2DumpFunctionTableC at %016X (rva %X)" % (proc, rva))

    path = b"C:/ng2dump/functable.txt\0"
    remote = k32.VirtualAllocEx(h, None, len(path), MEM_COMMIT_RESERVE, PAGE_READWRITE)
    wrote = ctypes.c_size_t()
    k32.WriteProcessMemory(h, ctypes.c_void_p(remote), path, len(path),
                           ctypes.byref(wrote))
    tid = wt.DWORD()
    th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(proc),
                                ctypes.c_void_p(remote), 0, ctypes.byref(tid))
    if not th:
        print("CreateRemoteThread failed: %d" % ctypes.get_last_error())
        return 1
    k32.WaitForSingleObject(th, 15000)
    print("done - game still running")
    return 0


if __name__ == "__main__":
    sys.exit(main())
