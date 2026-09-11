"""Validator pass 2: 64-bit rotate masks (rldicl/rldicr), srawi carry mask,
byte-reverse loads/stores, cntlzw/cntlzd. Recompute expected value, compare to
emitted C++. Shared ReXGlue codegen (applies to Fable 2)."""
import glob, re, os, sys, collections
GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
M64 = 0xFFFFFFFFFFFFFFFF

# rldicl rA,rS,SH,MB  -> rotateleft64(rS.u64, SH) & ((1<<(64-MB))-1)
rldicl_c = re.compile(r'^\t// rldicl r\d+,r\d+,(\d+),(\d+)\s*$')
# rldicr rA,rS,SH,ME  -> rotateleft64(rS.u64, SH) & (M64<<(63-ME))
rldicr_c = re.compile(r'^\t// rldicr r\d+,r\d+,(\d+),(\d+)\s*$')
rot_code = re.compile(r'__builtin_rotateleft64\([^,]+,\s*(\d+)\)\s*&\s*(0x[0-9A-Fa-f]+)')
# srawi rD,rS,SH -> xer.ca = (rS.s32 < 0) & ((rS.u32 & ((1<<SH)-1)) != 0)
srawi_c = re.compile(r'^\t// srawi r\d+,r\d+,(\d+)\s*$')
srawi_code = re.compile(r'&\s*(0x[0-9A-Fa-f]+)\)\s*!=\s*0')
# byte-reverse
br_c = re.compile(r'^\t// (lwbrx|lhbrx|stwbrx|sthbrx|ldbrx|stdbrx) ')
br_map = {'lwbrx':'bswap32','stwbrx':'bswap32','lhbrx':'bswap16','sthbrx':'bswap16','ldbrx':'bswap64','stdbrx':'bswap64'}
# cntlzw/cntlzd
clz_c = re.compile(r'^\t// (cntlzw|cntlzd) ')

cnt = collections.Counter(); mism = collections.defaultdict(list)
def nxt(lines,i):
    for j in range(i+1,min(i+3,len(lines))):
        s=lines[j]
        if s.strip().startswith('//') or not s.strip(): continue
        return j,s
    return None,None
for path in sorted(glob.glob(os.path.join(GEN,"ng2_recomp.*.cpp"))):
    b=os.path.basename(path); L=open(path,encoding="utf-8",errors="replace").read().splitlines()
    for i in range(len(L)-1):
        m=rldicl_c.match(L[i])
        if m:
            cnt['rldicl']+=1; sh,mb=int(m.group(1)),int(m.group(2)); want=(1<<(64-mb))-1 if mb<64 else 0
            j,c=nxt(L,i); cm=rot_code.search(c) if c else None
            if cm and (int(cm.group(1))!=sh or (int(cm.group(2),16)&M64)!=want):
                mism['rldicl'].append("%s:%d //rldicl sh%d mb%d -> rot%s mask0x%X EXPECT rot%d 0x%X"%(b,j+1,sh,mb,cm.group(1),int(cm.group(2),16)&M64,sh,want))
            continue
        m=rldicr_c.match(L[i])
        if m:
            cnt['rldicr']+=1; sh,me=int(m.group(1)),int(m.group(2)); want=(M64<<(63-me))&M64
            j,c=nxt(L,i); cm=rot_code.search(c) if c else None
            if cm and (int(cm.group(1))!=sh or (int(cm.group(2),16)&M64)!=want):
                mism['rldicr'].append("%s:%d //rldicr sh%d me%d -> rot%s mask0x%X EXPECT rot%d 0x%X"%(b,j+1,sh,me,cm.group(1),int(cm.group(2),16)&M64,sh,want))
            continue
        m=srawi_c.match(L[i])
        if m:
            cnt['srawi']+=1; sh=int(m.group(1)); want=(1<<sh)-1
            j,c=nxt(L,i); cm=srawi_code.search(c) if c else None
            if cm and (int(cm.group(1),16))!=want:
                mism['srawi'].append("%s:%d //srawi sh%d -> carrymask 0x%X EXPECT 0x%X"%(b,j+1,sh,int(cm.group(1),16),want))
            continue
        m=br_c.match(L[i])
        if m:
            cnt['bswap']+=1; j,c=nxt(L,i)
            if c and br_map[m.group(1)] not in c:
                mism['bswap'].append("%s:%d //%s -> %s  EXPECT %s"%(b,j+1,m.group(1),c.strip()[:50],br_map[m.group(1)]))
            continue
        m=clz_c.match(L[i])
        if m:
            cnt['clz']+=1; j,c=nxt(L,i)
            if c and '__builtin_clz' not in c:
                mism['clz'].append("%s:%d //%s -> %s"%(b,j+1,m.group(1),c.strip()[:50]))
            continue
print("=== validator pass 2 ===")
for k in ('rldicl','rldicr','srawi','bswap','clz'):
    print("  %-7s checked=%d mismatches=%d"%(k,cnt[k],len(mism[k])))
for k in mism:
    for x in mism[k][:40]: print("  "+x)
