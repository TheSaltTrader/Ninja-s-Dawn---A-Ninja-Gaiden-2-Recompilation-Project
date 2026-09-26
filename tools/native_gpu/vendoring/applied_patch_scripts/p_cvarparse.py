p = r"C:/Users/renoi/.claude/jobs/6397a53c/tmp/vendor_rtc_d3d12.py"
s = open(p, encoding="utf-8").read()
if "def replace_cvars" not in s:
    s = s.replace("bytecode = set()", r'''def replace_cvars(t, kind_macro, make):
    """Replace every KIND_MACRO(name, default, ...); by make(name, default). Scans to the MATCHING close paren,
    skipping string and char literals, so descriptions containing ');' or ',' cannot derail it."""
    out, i, n = [], 0, 0
    key = kind_macro + "("
    while True:
        j = t.find(key, i)
        if j < 0:
            out.append(t[i:]); break
        # only a definition at the start of a line (not inside a comment or another macro)
        line_start = t.rfind("\n", 0, j) + 1
        if t[line_start:j].strip():
            out.append(t[i:j + len(key)]); i = j + len(key); continue
        k, depth, args, cur = j + len(key), 1, [], []
        while depth:
            ch = t[k]
            if ch in "\"'":
                q = ch; cur.append(ch); k += 1
                while t[k] != q:
                    if t[k] == "\\":
                        cur.append(t[k]); k += 1
                    cur.append(t[k]); k += 1
                cur.append(q); k += 1; continue
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if not depth:
                    break
            elif ch == "," and depth == 1:
                args.append("".join(cur).strip()); cur = []; k += 1; continue
            cur.append(ch); k += 1
        args.append("".join(cur).strip())
        k += 1
        while t[k] in " \t":
            k += 1
        if t[k] == ";":
            k += 1
        out.append(t[i:j]); out.append(make(args[0], args[1])); n += 1
        i = k
    return "".join(out), n
bytecode = set()''')
    old_b = s[s.index("    t, nb = cvar_bool.subn("):s.index("\n", s.index("    t, ni = cvar_int.subn(")) + 1]
    new_b = '''    t, nb = replace_cvars(t, "REXCVAR_DEFINE_BOOL", lambda name, dflt: forced("bool", name, dflt) or 'bool& FLAGS_%s_storage_() { static bool s = ::fable2::ngpu::xlat::PluginBool("%s", %s); return s; }' % (name, name, dflt))
    t, ns = replace_cvars(t, "REXCVAR_DEFINE_STRING", lambda name, dflt: forced("std::string", name, dflt) or 'std::string& FLAGS_%s_storage_() { static std::string s = ::fable2::ngpu::xlat::PluginString("%s", %s); return s; }' % (name, name, dflt))
    t, ni = replace_cvars(t, "REXCVAR_DEFINE_INT32", lambda name, dflt: 'int32_t& FLAGS_%s_storage_() { static int32_t s = ::fable2::ngpu::xlat::PluginInt("%s", %s); return s; }' % (name, name, dflt))
    t, nd = replace_cvars(t, "REXCVAR_DEFINE_DOUBLE", lambda name, dflt: 'double& FLAGS_%s_storage_() { static double s = ::fable2::ngpu::xlat::PluginDouble("%s", %s); return s; }' % (name, name, dflt))
    t, nu = replace_cvars(t, "REXCVAR_DEFINE_UINT32", lambda name, dflt: 'uint32_t& FLAGS_%s_storage_() { static uint32_t s = uint32_t(::fable2::ngpu::xlat::PluginInt("%s", int32_t(%s))); return s; }' % (name, name, dflt))
'''
    s = s.replace(old_b, new_b)
    s = s.replace("int32_t PluginInt(const char*, int32_t); }", "int32_t PluginInt(const char*, int32_t); double PluginDouble(const char*, double); }")
open(p, "w", encoding="utf-8").write(s)
print("ok")
