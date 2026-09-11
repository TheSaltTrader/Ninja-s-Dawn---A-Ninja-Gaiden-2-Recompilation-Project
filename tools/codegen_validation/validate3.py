"""Validator pass 3 (ReXGlue codegen, shared with Fable 2). Covers the classes
passes 1/2 didn't: extended rotate/mask aliases (clrlwi/rotlwi/clrldi/rotldi),
variable shifts (slw/srw/sld/srd + arithmetic sraw/srad), carry arithmetic
(addze/subfe/subfc/addic), multiply/divide signedness, and record-form CR0.

Exact checks recompute the expected value; structural checks assert the emitted
C++ has the right operator/signedness/guard. Any mismatch is a real mistranslation."""
import glob, re, os, sys, collections
GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
M32 = 0xFFFFFFFF; M64 = 0xFFFFFFFFFFFFFFFF
TAB = "\t"
cnt = collections.Counter(); mism = collections.defaultdict(list)

def code_after(lines, i, n=5):
    out = []
    for j in range(i+1, min(i+1+n, len(lines))):
        s = lines[j]
        if s.strip().startswith('//'):
            break  # next instruction's comment: stop
        out.append((j, s))
    return out

# --- exact: extended aliases ---
clrlwi_c = re.compile(r'^\t// (clrlwi)\.? r\d+,r\d+,(\d+)\s*$')
rotlwi_c = re.compile(r'^\t// (rotlwi)\.? r\d+,r\d+,(\d+)\s*$')
clrldi_c = re.compile(r'^\t// (clrldi)\.? r\d+,r\d+,(\d+)\s*$')
rotldi_c = re.compile(r'^\t// (rotldi)\.? r\d+,r\d+,(\d+)\s*$')
mask_code = re.compile(r'&\s*(0x[0-9A-Fa-f]+)')
rot32_code = re.compile(r'__builtin_rotateleft32\([^,]+,\s*(\d+)\)')
rot64_code = re.compile(r'__builtin_rotateleft64\([^,]+,\s*(\d+)\)')

# --- structural: variable shifts ---
shift_c = re.compile(r'^\t// (slw|srw|sld|srd)\.? r\d+,r\d+,r\d+\s*$')
SHIFT = {'slw':('<<','0x20','.u32'), 'srw':('>>','0x20','.u32'),
         'sld':('<<','0x40','.u64'), 'srd':('>>','0x40','.u64')}
ash_c = re.compile(r'^\t// (sraw|srad)\.? r\d+,r\d+,r\d+\s*$')
ASH = {'sraw':('0x1F','.s32'), 'srad':('0x3F','.s64')}

# --- structural: carry arithmetic (must touch xer.ca) ---
carry_c = re.compile(r'^\t// (addze|subfe|subfc|addic|adde|addc|addme|subfme)\.? ')

# --- structural: multiply / divide signedness ---
md_c = re.compile(r'^\t// (mullw|mulhw|mulhwu|mulhd|mulhdu|mulld|divw|divwu|divd|divdu)\.? ')

# --- record forms: integer '.' variants must set cr0 ---
rec_c = re.compile(r'^\t// ([a-z][a-z0-9]*)\. ')
VEC_REC = None  # vcmp*. set cr6, handled separately
vcmp_c = re.compile(r'^\t// (vcmp[a-z0-9]+)\. ')

for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    b = os.path.basename(path)
    L = open(path, encoding="utf-8", errors="replace").read().splitlines()
    for i in range(len(L)-1):
        line = L[i]
        # extended aliases (exact) --------------------------------------
        m = clrlwi_c.match(line)
        if m:
            cnt['clrlwi'] += 1; n = int(m.group(2)); want = (M32 >> n) & M32
            ca = code_after(L, i, 2)
            if ca:
                cm = mask_code.search(ca[0][1])
                if cm and (int(cm.group(1),16) & M32) != want:
                    mism['clrlwi'].append("%s:%d clrlwi n=%d -> &0x%X EXPECT 0x%X" % (b, ca[0][0]+1, n, int(cm.group(1),16)&M32, want))
            continue
        m = clrldi_c.match(line)
        if m:
            cnt['clrldi'] += 1; n = int(m.group(2)); want = (M64 >> n) & M64
            ca = code_after(L, i, 2)
            if ca:
                cm = mask_code.search(ca[0][1])
                if cm and (int(cm.group(1),16) & M64) != want:
                    mism['clrldi'].append("%s:%d clrldi n=%d -> &0x%X EXPECT 0x%X" % (b, ca[0][0]+1, n, int(cm.group(1),16)&M64, want))
            continue
        m = rotlwi_c.match(line)
        if m:
            cnt['rotlwi'] += 1; n = int(m.group(2))
            ca = code_after(L, i, 2)
            if ca:
                cm = rot32_code.search(ca[0][1])
                if cm and int(cm.group(1)) != n:
                    mism['rotlwi'].append("%s:%d rotlwi n=%d -> rot32(%s)" % (b, ca[0][0]+1, n, cm.group(1)))
                elif not cm:
                    mism['rotlwi'].append("%s:%d rotlwi n=%d -> no rotateleft32 (%s)" % (b, ca[0][0]+1, n, ca[0][1].strip()[:50]))
            continue
        m = rotldi_c.match(line)
        if m:
            cnt['rotldi'] += 1; n = int(m.group(2))
            ca = code_after(L, i, 2)
            if ca:
                cm = rot64_code.search(ca[0][1])
                if cm and int(cm.group(1)) != n:
                    mism['rotldi'].append("%s:%d rotldi n=%d -> rot64(%s)" % (b, ca[0][0]+1, n, cm.group(1)))
            continue
        # variable logical shifts (structural) --------------------------
        m = shift_c.match(line)
        if m:
            cnt['vshift'] += 1; op, guard, sign = SHIFT[m.group(1)]
            ca = code_after(L, i, 2)
            txt = ca[0][1] if ca else ""
            if op not in txt or guard not in txt or sign not in txt:
                mism['vshift'].append("%s:%d %s EXPECT op'%s' guard%s src%s GOT %s" % (b, (ca[0][0]+1 if ca else i+1), m.group(1), op, guard, sign, txt.strip()[:60]))
            continue
        m = ash_c.match(line)
        if m:
            cnt['ashift'] += 1; clamp, sign = ASH[m.group(1)]
            ca = code_after(L, i, 4); blob = " ".join(s for _, s in ca)
            if clamp not in blob or ('>>' not in blob):
                mism['ashift'].append("%s:%d %s EXPECT clamp%s '>>' GOT %s" % (b, i+1, m.group(1), clamp, blob.strip()[:70]))
            continue
        # carry arithmetic (structural) ---------------------------------
        m = carry_c.match(line)
        if m:
            cnt['carry'] += 1
            ca = code_after(L, i, 4); blob = " ".join(s for _, s in ca)
            if 'xer.ca' not in blob:
                mism['carry'].append("%s:%d %s -> no xer.ca (%s)" % (b, i+1, m.group(1), blob.strip()[:60]))
            continue
        # multiply / divide signedness ----------------------------------
        m = md_c.match(line)
        if m:
            cnt['muldiv'] += 1; mn = m.group(1)
            ca = code_after(L, i, 2); blob = " ".join(s for _, s in ca)
            bad = False; why = ""
            if mn == 'mullw' and not ('int64_t' in blob and '.s32' in blob): bad=True; why="signed s32*s32"
            if mn == 'mulhw' and not ('int64_t' in blob and '>> 32' in blob): bad=True; why="signed >>32"
            if mn == 'mulhwu' and not ('uint64_t' in blob and '>> 32' in blob): bad=True; why="unsigned >>32"
            if mn == 'divw' and not ('.s32' in blob and '/' in blob): bad=True; why="signed /"
            if mn == 'divwu' and not ('.u32' in blob and '/' in blob): bad=True; why="unsigned /"
            if mn == 'divd' and not ('.s64' in blob and '/' in blob): bad=True; why="signed64 /"
            if mn == 'divdu' and not ('.u64' in blob and '/' in blob): bad=True; why="unsigned64 /"
            if bad:
                mism['muldiv'].append("%s:%d %s EXPECT %s GOT %s" % (b, i+1, mn, why, blob.strip()[:70]))
            continue
        # record forms: integer '.' must set cr0 ------------------------
        m = vcmp_c.match(line)
        if m:
            cnt['vrec'] += 1
            ca = code_after(L, i, 4); blob = " ".join(s for _, s in ca)
            if 'cr6' not in blob:
                mism['vrec'].append("%s:%d %s. -> no cr6 (%s)" % (b, i+1, m.group(1), blob.strip()[:50]))
            continue
        m = rec_c.match(line)
        if m:
            mn = m.group(1)
            if mn in ('stwcx','stdcx','lwarx','ldarx'):  # store-cond / reserve: cr0 set differently
                cnt['reccond'] += 1
                ca = code_after(L, i, 5); blob = " ".join(s for _, s in ca)
                if mn in ('stwcx','stdcx') and 'cr0' not in blob:
                    mism['reccond'].append("%s:%d %s. -> no cr0 (%s)" % (b, i+1, mn, blob.strip()[:50]))
                continue
            cnt['record'] += 1
            ca = code_after(L, i, 5); blob = " ".join(s for _, s in ca)
            if 'cr0' not in blob:
                mism['record'].append("%s:%d %s. -> NO cr0 update (%s)" % (b, i+1, mn, blob.strip()[:60]))
            continue

print("=== validator pass 3 ===")
for k in ('clrlwi','clrldi','rotlwi','rotldi','vshift','ashift','carry','muldiv','record','vrec','reccond'):
    print("  %-8s checked=%-7d mismatches=%d" % (k, cnt[k], len(mism[k])))
for k in mism:
    for x in mism[k][:40]:
        print("  "+x)
