"""Validate ReXGlue static-recompilation codegen: compare each PPC instruction
comment (`// <mnemonic> ...`, the disassembly ground truth) against the emitted
C++ on the following line(s). Reports mismatches per class. Shared codegen with
Fable 2, so any finding applies there too.

Usage: python codegen_validate.py <generated/default dir>
"""
import glob, re, os, sys, collections

GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"

# ---- Class 1: conditional branches. Comment `// b<cc>[+-]? crN,ADDR` -> `if (COND) goto` ----
BR_EXP = {
    'bne': '!cr{n}.eq', 'beq': 'cr{n}.eq',
    'blt': 'cr{n}.lt',  'bge': '!cr{n}.lt',
    'bgt': 'cr{n}.gt',  'ble': '!cr{n}.gt',
    'bso': 'cr{n}.so',  'bns': '!cr{n}.so',
}
br_cmt = re.compile(r'^\t// (bne|beq|blt|bge|bgt|ble|bso|bns)[+-]? cr(\d+),')
br_if  = re.compile(r'^\tif \((.*?)\) goto ')

# ---- Class 2: integer compares. signed vs unsigned + width ----
# cmpw/cmpwi/cmpd/cmpdi = SIGNED ; cmplw/cmplwi/cmpld/cmpldi = UNSIGNED
cmp_cmt = re.compile(r'^\t// (cmpw|cmpwi|cmpd|cmpdi|cmplw|cmplwi|cmpld|cmpldi) cr(\d+),')
cmp_code = re.compile(r'^\tcr(\d+)\.compare<(u?int\d+_t)>\(')
def cmp_expect(mn):
    signed = not mn.startswith('cmpl')
    width = 64 if ('d' in mn[3:] or mn in ('cmpd','cmpdi','cmpld','cmpldi')) else 32
    # width: 'w'->32, 'd'->64
    width = 64 if ('cmpd' in mn or 'cmpld' in mn) else 32
    return ('int%d_t' % width) if signed else ('uint%d_t' % width)

# ---- Class 3: integer loads. mnemonic -> REX_LOAD width (unsigned zero-extend variants) ----
LD_EXP = {'lwz':'U32','lwzx':'U32','lwzu':'U32','lwzux':'U32',
          'lhz':'U16','lhzx':'U16','lhzu':'U16','lhzux':'U16',
          'lbz':'U8','lbzx':'U8','lbzu':'U8','lbzux':'U8',
          'ld':'U64','ldx':'U64','ldu':'U64','ldux':'U64'}
ld_cmt = re.compile(r'^\t// (lwz|lwzx|lwzu|lwzux|lhz|lhzx|lhzu|lhzux|lbz|lbzx|lbzu|lbzux|ld|ldx|ldu|ldux) ')
ld_code = re.compile(r'REX_LOAD_(U\d+)\(')

# ---- Class 4: integer stores ----
ST_EXP = {'stw':'U32','stwx':'U32','stwu':'U32','stwux':'U32',
          'sth':'U16','sthx':'U16','sthu':'U16','sthux':'U16',
          'stb':'U8','stbx':'U8','stbu':'U8','stbux':'U8',
          'std':'U64','stdx':'U64','stdu':'U64','stdux':'U64'}
st_cmt = re.compile(r'^\t// (stw|stwx|stwu|stwux|sth|sthx|sthu|sthux|stb|stbx|stbu|stbux|std|stdx|stdu|stdux) ')
st_code = re.compile(r'REX_STORE_(U\d+)\(')

counts = collections.Counter()
mism = collections.defaultdict(list)

def next_code(lines, i, maxskip=3):
    for j in range(i+1, min(i+1+maxskip, len(lines))):
        s = lines[j]
        if s.strip().startswith('//') or s.strip()=='' : continue
        return j, s
    return None, None

for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    base = os.path.basename(path)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    for i in range(len(lines)-1):
        L = lines[i]
        m = br_cmt.match(L)
        if m:
            counts['branch']+=1
            mi = br_if.match(lines[i+1]) if i+1<len(lines) else None
            if mi:
                exp = BR_EXP[m.group(1)].format(n=m.group(2))
                if mi.group(1).strip()!=exp:
                    mism['branch'].append("%s:%d  // %s crN -> if (%s)  EXPECT %s"%(base,i+2,m.group(1),mi.group(1).strip(),exp))
            continue
        m = cmp_cmt.match(L)
        if m:
            counts['compare']+=1
            j,code = next_code(lines,i)
            if code:
                mi = cmp_code.match(code)
                if mi:
                    exp = cmp_expect(m.group(1))
                    if mi.group(2)!=exp or mi.group(1)!=m.group(2):
                        mism['compare'].append("%s:%d  // %s cr%s -> cr%s.compare<%s>  EXPECT cr%s.<%s>"%(base,j+1,m.group(1),m.group(2),mi.group(1),mi.group(2),m.group(2),exp))
            continue
        m = ld_cmt.match(L)
        if m:
            counts['load']+=1
            j,code = next_code(lines,i)
            if code:
                mi = ld_code.search(code)
                if mi and mi.group(1)!=LD_EXP[m.group(1)]:
                    mism['load'].append("%s:%d  // %s -> REX_LOAD_%s  EXPECT %s"%(base,j+1,m.group(1),mi.group(1),LD_EXP[m.group(1)]))
            continue
        m = st_cmt.match(L)
        if m:
            counts['store']+=1
            j,code = next_code(lines,i)
            if code:
                mi = st_code.search(code)
                if mi and mi.group(1)!=ST_EXP[m.group(1)]:
                    mism['store'].append("%s:%d  // %s -> REX_STORE_%s  EXPECT %s"%(base,j+1,m.group(1),mi.group(1),ST_EXP[m.group(1)]))
            continue

print("=== validated instruction counts ===")
for k in ('branch','compare','load','store'):
    print("  %-8s checked=%d  mismatches=%d" % (k, counts[k], len(mism[k])))
for k in ('branch','compare','load','store'):
    if mism[k]:
        print("\n=== %s MISMATCHES (%d) ===" % (k.upper(), len(mism[k])))
        for x in mism[k][:60]:
            print("  "+x)
