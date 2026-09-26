P = r"C:/users/renoi/claudecode/Fable 2 Recompile Xbox/wt-fable2-nativegpu/src/native_gpu_present.cpp"
s = open(P, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'
lines = s.split(nl)
i = next(k for k, l in enumerate(lines) if l.startswith('  if ((g_s.frames % 600) == 0)') and 'EDRAM COLOUR CLEAR ALIASING:' in lines[k+1])
assert 'std::string by;' in lines[i+3] and 'applied by target' in lines[i+5], lines[i:i+6]
lines[i] = lines[i] + ' {   // braces added 2026-09-26: the three lines below ran on EVERY clearing resolve (PROF5 log rotated at 5 MB)'
lines.insert(i + 6, '  }')
open(P, 'w', encoding='utf-8', newline='').write(nl.join(lines))
print("ok", i + 1)
