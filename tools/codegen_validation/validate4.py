"""Validator pass 4 (ReXGlue codegen). Control-flow-relevant classes the census
flagged unvalidated: immediate arithmetic (li/lis/addi/addis/mulli), subf operand
order, logical ops + immediates, sign-extending loads (lha/lwa/lhax/lwax),
insert-rotates (rlwimi/rldimi) masks, conditional returns (b<cc>lr), bdnz counter,
spr moves. Exact where recomputable, structural otherwise."""
import glob, re, os, sys, collections
GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
M32=0xFFFFFFFF; M64=0xFFFFFFFFFFFFFFFF
cnt=collections.Counter(); mism=collections.defaultdict(list)

def ppc_mask(mb,me):
    m=0
    for k in range(32):
        i=(mb+k)&31; m|=1<<(31-i)
        if i==me: break
    return m&M32
def ppc_mask64(mb,me):
    m=0
    for k in range(64):
        i=(mb+k)&63; m|=1<<(63-i)
        if i==me: break
    return m&M64

def code_after(L,i,n=3):
    for j in range(i+1,min(i+1+n,len(L))):
        s=L[j]
        if s.strip().startswith('//'): return None,None
        if s.strip(): return j,s
    return None,None

# exact immediates
li_c   = re.compile(r'^\t// li r(\d+),(-?\d+)\s*$')
li_v   = re.compile(r'\.s64 = (-?\d+);')
lis_c  = re.compile(r'^\t// lis r(\d+),(-?\d+)\s*$')
addis_c= re.compile(r'^\t// addis r\d+,r\d+,(-?\d+)\s*$')
addis_v= re.compile(r'\+ (-?\d+);')
mulli_c= re.compile(r'^\t// mulli r\d+,r\d+,(-?\d+)\s*$')
mulli_v= re.compile(r'uint64_t>\((-?\d+)\)')
# subf operand order
subf_c = re.compile(r'^\t// subf r(\d+),r(\d+),r(\d+)\s*$')
# logical immediates (unsigned 16-bit) : andi. ori xori (+ oris/xoris/andis. shifted)
logi = {'ori':'|','xori':'^','andi.':'&','oris':'|','xoris':'^','andis.':'&'}
logi_c = re.compile(r'^\t// (ori|xori|andi\.|oris|xoris|andis\.) r\d+,r\d+,(\d+)\s*$')
logi_v = re.compile(r'([&|^]) (\d+);')
# logical reg ops
logr = {'and':'&','or':'|','xor':'^'}
logr_c = re.compile(r'^\t// (and|or|xor) r(\d+),r(\d+),r(\d+)\s*$')
# sign-extend loads
sxl = {'lha':'int16_t','lhax':'int16_t','lhau':'int16_t','lhaux':'int16_t','lwa':'int32_t','lwax':'int32_t','lwaux':'int32_t'}
sxl_c = re.compile(r'^\t// (lha|lhax|lhau|lhaux|lwa|lwax|lwaux) ')
# insert rotates
rlwimi_c = re.compile(r'^\t// rlwimi r\d+,r\d+,(\d+),(\d+),(\d+)\s*$')
rldimi_c = re.compile(r'^\t// rldimi r\d+,r\d+,(\d+),(\d+)\s*$')
twomask = re.compile(r'&\s*(0x[0-9A-Fa-f]+)\).*?&\s*(0x[0-9A-Fa-f]+)\)')
# conditional returns
BR={'bne':'!cr{n}.eq','beq':'cr{n}.eq','blt':'cr{n}.lt','bge':'!cr{n}.lt','bgt':'cr{n}.gt','ble':'!cr{n}.gt','bso':'cr{n}.so','bns':'!cr{n}.so'}
clr_c = re.compile(r'^\t// (bne|beq|blt|bge|bgt|ble|bso|bns)lr[+-]? cr(\d+)\s*$')
clr_v = re.compile(r'^\tif \((.*?)\) return;')
# bdnz
bdnz_c = re.compile(r'^\t// bdnz ')
# spr moves (monolithic dest). mtxer is field-decomposed -> handled separately.
spr = {'mtctr':('ctr','r'),'mfctr':('r','ctr'),'mtlr':('lr','r'),'mflr':('r','lr')}
spr_c = re.compile(r'^\t// (mtctr|mtlr) r\d+\s*$')
mtxer_c = re.compile(r'^\t// mtxer r\d+\s*$')

for path in sorted(glob.glob(os.path.join(GEN,"ng2_recomp.*.cpp"))):
    b=os.path.basename(path); L=open(path,encoding="utf-8",errors="replace").read().splitlines()
    for i in range(len(L)-1):
        line=L[i]
        m=li_c.match(line)
        if m:
            cnt['li']+=1; simm=int(m.group(2)); j,c=code_after(L,i,1)
            v=li_v.search(c) if c else None
            if v and int(v.group(1))!=simm: mism['li'].append("%s:%d li=%d GOT %s"%(b,j+1,simm,v.group(1)))
            continue
        m=lis_c.match(line)
        if m:
            cnt['lis']+=1; simm=int(m.group(2)); want=simm<<16; j,c=code_after(L,i,1)
            v=li_v.search(c) if c else None
            if v and int(v.group(1))!=want: mism['lis'].append("%s:%d lis simm=%d EXPECT %d GOT %s"%(b,j+1,simm,want,v.group(1)))
            continue
        m=addis_c.match(line)
        if m:
            cnt['addis']+=1; simm=int(m.group(1)); want=simm<<16; j,c=code_after(L,i,1)
            v=addis_v.search(c) if c else None
            if v and int(v.group(1))!=want: mism['addis'].append("%s:%d addis simm=%d EXPECT +%d GOT +%s"%(b,j+1,simm,want,v.group(1)))
            continue
        m=mulli_c.match(line)
        if m:
            cnt['mulli']+=1; simm=int(m.group(1)); j,c=code_after(L,i,1)
            v=mulli_v.search(c) if c else None
            if v and int(v.group(1))!=simm: mism['mulli'].append("%s:%d mulli=%d GOT %s"%(b,j+1,simm,v.group(1)))
            continue
        m=subf_c.match(line)
        if m:
            cnt['subf']+=1; rd,ra,rb=m.group(1),m.group(2),m.group(3); j,c=code_after(L,i,1)
            # expect: rD... = rB... - rA...
            if c:
                mm=re.search(r'r%s\.\w+ = (?:ctx\.)?r(\d+)\.\w+ - (?:ctx\.)?r(\d+)\.\w+'%rd,c)
                if mm and (mm.group(1)!=rb or mm.group(2)!=ra):
                    mism['subf'].append("%s:%d subf rD%s,rA%s,rB%s EXPECT r%s - r%s GOT r%s - r%s"%(b,j+1,rd,ra,rb,rb,ra,mm.group(1),mm.group(2)))
            continue
        m=logi_c.match(line)
        if m:
            cnt['logi']+=1; op=logi[m.group(1)]; imm=int(m.group(2)); j,c=code_after(L,i,1)
            v=logi_v.search(c) if c else None
            if v and (v.group(1)!=op or (int(v.group(2))!=imm and int(v.group(2))!=(imm<<16))):
                mism['logi'].append("%s:%d %s imm=%d EXPECT '%s' GOT '%s %s'"%(b,j+1,m.group(1),imm,op,v.group(1),v.group(2)))
            continue
        m=logr_c.match(line)
        if m:
            cnt['logr']+=1; op=logr[m.group(1)]; j,c=code_after(L,i,1)
            if c and (' %s '%op) not in c:
                mism['logr'].append("%s:%d %s EXPECT '%s' GOT %s"%(b,j+1,m.group(1),op,c.strip()[:50]))
            continue
        m=sxl_c.match(line)
        if m:
            cnt['sxl']+=1; want=sxl[m.group(1)]; j,c=code_after(L,i,1)
            if c and want not in c:
                mism['sxl'].append("%s:%d %s EXPECT %s GOT %s"%(b,j+1,m.group(1),want,c.strip()[:50]))
            continue
        m=rlwimi_c.match(line)
        if m:
            # ISA: m = MASK(MB+32, ME+32) as a 64-bit mask; keep = ~m. Wraparound
            # masks (MB>ME) legitimately set the upper 32 bits of the rotate mask.
            cnt['rlwimi']+=1; sh,mb,me=int(m.group(1)),int(m.group(2)),int(m.group(3))
            want=ppc_mask64(mb+32,me+32); j,c=code_after(L,i,1)
            v=twomask.search(c) if c else None
            if v:
                gm=int(v.group(1),16)&M64; gk=int(v.group(2),16)&M64
                if gm!=want or gk!=((~want)&M64):
                    mism['rlwimi'].append("%s:%d rlwimi sh%d mb%d me%d mask0x%X keep0x%X EXPECT 0x%X keep0x%X"%(b,j+1,sh,mb,me,gm,gk,want,(~want)&M64))
            continue
        m=rldimi_c.match(line)
        if m:
            cnt['rldimi']+=1; sh,mb=int(m.group(1)),int(m.group(2)); want=ppc_mask64(mb,(63-sh)&63); j,c=code_after(L,i,1)
            v=twomask.search(c) if c else None
            if v:
                gm=int(v.group(1),16)&M64; gk=int(v.group(2),16)&M64
                if gm!=want or gk!=((~want)&M64):
                    mism['rldimi'].append("%s:%d rldimi sh%d mb%d mask0x%X keep0x%X EXPECT 0x%X keep0x%X"%(b,j+1,sh,mb,gm,gk,want,(~want)&M64))
            continue
        m=clr_c.match(line)
        if m:
            cnt['condret']+=1; exp=BR[m.group(1)].format(n=m.group(2)); j,c=code_after(L,i,1)
            v=clr_v.match(c) if c else None
            if v and v.group(1).strip()!=exp:
                mism['condret'].append("%s:%d %slr cr%s EXPECT if(%s) GOT if(%s)"%(b,j+1,m.group(1),m.group(2),exp,v.group(1).strip()))
            continue
        m=bdnz_c.match(line)
        if m:
            cnt['bdnz']+=1; blob=""
            for j in range(i+1,min(i+4,len(L))):
                if L[j].strip().startswith('//'): break
                blob+=L[j]
            if '--ctr' not in blob or 'goto' not in blob:
                mism['bdnz'].append("%s:%d bdnz EXPECT --ctr+goto GOT %s"%(b,i+2,blob.strip()[:60]))
            continue
        m=spr_c.match(line)
        if m:
            cnt['spr']+=1; dst=spr[m.group(1)][0]; j,c=code_after(L,i,1)
            if c and ('%s.u64 ='%dst) not in c:
                mism['spr'].append("%s:%d %s EXPECT %s.u64= GOT %s"%(b,j+1,m.group(1),dst,c.strip()[:50]))
            continue
        m=mtxer_c.match(line)
        if m:
            cnt['mtxer']+=1; blob=""
            for j in range(i+1,min(i+5,len(L))):
                if L[j].strip().startswith('//'): break
                blob+=L[j]
            if not ('xer.so' in blob and 'xer.ov' in blob and 'xer.ca' in blob):
                mism['mtxer'].append("%s:%d mtxer EXPECT so/ov/ca fields GOT %s"%(b,i+2,blob.strip()[:60]))
            continue

print("=== validator pass 4 ===")
for k in ('li','lis','addis','mulli','subf','logi','logr','sxl','rlwimi','rldimi','condret','bdnz','spr','mtxer'):
    print("  %-8s checked=%-7d mismatches=%d"%(k,cnt[k],len(mism[k])))
for k in mism:
    for x in mism[k][:40]: print("  "+x)
