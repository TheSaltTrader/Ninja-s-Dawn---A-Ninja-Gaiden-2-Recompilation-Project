P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
old = "    {0x4000, 2048},                                          // ALU float constants"
new = ("    // BACKEND TRANSPLANT (2026-09-26): the WHOLE 0x2000-0x23FF block, because the transplanted command processor's"
       + nl + "    // IssueDraw / IssueCopy read state outside the ranges above - RB_COPY_* (0x2318..), RB_DEPTH/COLOR_CLEAR, the MSAA"
       + nl + "    // and sample registers. Overlaps the ranges above; a register is compared (and recorded) once per range it is"
       + nl + "    // in, so the overlap only costs compares, never a duplicate delta (the prev value is updated on the first)."
       + nl + "    {0x2000, 0x400},"
       + nl + old)
assert s.count(old) == 1
s = s.replace(old, new, 1)
open(P, "w", encoding="utf-8", newline="").write(s)
print("ok")
