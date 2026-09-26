p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
old = '''        k += 1
        while t[k] in " \\t":
            k += 1
        if t[k] == ";":
            k += 1'''
new = '''        k += 1
        # chained builder calls (.range(...), .lifecycle(...)) belong to the definition too
        while True:
            m = k
            while t[m] in " \\t\\r\\n":
                m += 1
            if t[m] != ".":
                break
            m = t.index("(", m) + 1
            d = 1
            while d:
                if t[m] in "\\"'":
                    q = t[m]; m += 1
                    while t[m] != q:
                        m += 2 if t[m] == "\\\\" else 1
                elif t[m] == "(":
                    d += 1
                elif t[m] == ")":
                    d -= 1
                m += 1
            k = m
        while t[k] in " \\t":
            k += 1
        if t[k] == ";":
            k += 1'''
assert s.count(old) == 1, s.count(old)
s = s.replace(old, new)
open(p, "w", encoding="utf-8").write(s)
print("ok")
