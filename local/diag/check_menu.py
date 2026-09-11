import io
import re

P = r"C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\src\ng2_menu.cpp"
src = io.open(P, encoding="utf-8").read()

# Strip comments and string/char literals before counting delimiters.
s = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
s = re.sub(r"//[^\r\n]*", "", s)
s = re.sub(r"'(?:[^'\\]|\\.)'", "' '", s)
s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s)

ok = True
for label, o, c in (("braces", "{", "}"), ("parens", "(", ")")):
    d = s.count(o) - s.count(c)
    if d:
        ok = False
    print("  %-7s %d open %d close  delta %+d  %s"
          % (label, s.count(o), s.count(c), d, "OK" if d == 0 else "UNBALANCED"))

print("  BeginDisabled(!live) remaining: %d (want 0)" % src.count("BeginDisabled(!live)"))
print("  BeginDisabled(live) remaining:  %d (want 0)" % src.count("BeginDisabled(live)"))
print("  'restart required' tag: %d (want 1)" % src.count('"restart required"'))
print("  Browse button:          %d (want 1)" % src.count("Browse...##tex"))
print("  BeginDisabled / EndDisabled: %d / %d (must match)"
      % (src.count("BeginDisabled("), src.count("EndDisabled(")))
if src.count("BeginDisabled(") != src.count("EndDisabled("):
    ok = False
raise SystemExit(0 if ok else 1)
