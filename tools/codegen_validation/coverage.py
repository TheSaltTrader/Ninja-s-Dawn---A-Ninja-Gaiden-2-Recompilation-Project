"""Coverage census: for every distinct PPC mnemonic in the disassembly comments,
count instances and mark whether a validator pass checks it. Prints the
unvalidated remainder grouped by family so we can see exactly what's left."""
import glob, re, os, sys, collections
GEN = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
TAB = "\t"
cm = re.compile(r'^\t// ([a-z][a-z0-9._]*)')

# validated by pass 1/2/3 + earlier-session sign-extend
VALID = set("""
bne beq blt bge bgt ble bso bns
cmpw cmpwi cmpd cmpdi cmplw cmplwi cmpld cmpldi
lwz lwzx lwzu lwzux lhz lhzx lhzu lhzux lbz lbzx lbzu lbzux ld ldx ldu ldux
stw stwx stwu stwux sth sthx sthu sthux stb stbx stbu stbux std stdx stdu stdux
rlwinm rlwinm. rlwnm rldicl rldicr srawi srawi.
lwbrx lhbrx stwbrx sthbrx ldbrx stdbrx cntlzw cntlzd
clrlwi clrlwi. clrldi rotlwi rotlwi. rotldi
slw slw. srw srw. sld srd sraw sraw. srad sradi
addze addze. subfe subfc subfc. addic addic. adde addc addme subfme
mullw mullw. mulhw mulhwu mulhd mulhdu mulld divw divw. divwu divwu. divd divdu
extsb extsb. extsh extsh. extsw
""".split())
# record forms (any '.') are checked structurally by pass 3
def is_record(m): return m.endswith('.')

cnt = collections.Counter()
for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
    for line in open(path, encoding="utf-8", errors="replace"):
        m = cm.match(line)
        if m:
            cnt[m.group(1)] += 1

total = sum(cnt.values())
vcount = sum(c for m,c in cnt.items() if m in VALID or (is_record(m) and m.rstrip('.') ))
# refine: record-form '.' handled by pass3, count those too
rec = sum(c for m,c in cnt.items() if is_record(m) and m not in VALID)
valid_instr = sum(c for m,c in cnt.items() if m in VALID)
rec_instr   = sum(c for m,c in cnt.items() if is_record(m) and m not in VALID)

# classify the remainder
def fam(m):
    if m in VALID: return None
    if is_record(m): return None  # covered by record-form pass
    if m.startswith(('lf','stf','fa','fs','fm','fd','fneg','fabs','frsp','fcti','fcmp','fsel','fre','frsqrte','fnm','fmr','fnabs','fctid','fcfid')) or m[0]=='f':
        return 'float'
    if m.startswith('v') or m.startswith('lv') or m.startswith('stv') or m in ('mfvscr','mtvscr'):
        return 'vector'
    if m in ('addi','addis','li','lis','la','subi','subis','addic','subfic','mulli','add','subf','sub','neg','nego','addo','subfo'):
        return 'int-arith'
    if m in ('and','andi','andis','or','ori','oris','xor','xori','xoris','nand','nor','eqv','andc','orc','not','mr'):
        return 'int-logical'
    if m in ('b','ba','bl','bla','blr','blrl','bctr','bctrl','bdnz','bdz','bdnzf','bdnzt','bne+','bne-','beq+','beq-','blt+','bgt-'):
        return 'uncond-branch'
    if m in ('mtctr','mfctr','mtlr','mflr','mtxer','mfxer','mtcr','mfcr','mcrf','mtspr','mfspr','mftb','mtcrf','mfocrf'):
        return 'spr-move'
    if m in ('rlwimi','rldimi','rldic','rldcl','rldcr','insrdi','insrwi','inslwi','extldi','extrdi','clrrwi','clrrdi','slwi','srwi','sldi','srdi','rotrwi','rotrdi','extlwi','extrwi'):
        return 'rotate-other'
    if m in ('sc','rfid','tw','twi','td','tdi','trap','isync','sync','lwsync','eieio','dcbz','dcbf','dcbt','dcbst','icbi','nop','mbar'):
        return 'sys/nop/trap'
    return 'other'

fams = collections.defaultdict(lambda: [0,0])  # family -> [instrs, distinct]
for m,c in cnt.items():
    f = fam(m)
    if f:
        fams[f][0]+=c; fams[f][1]+=1

print("distinct mnemonics: %d   total instr-comments: %d" % (len(cnt), total))
print("VALIDATED (exact/struct passes): %d instrs across %d mnemonics" % (valid_instr, sum(1 for m in cnt if m in VALID)))
print("RECORD-FORM '.' (cr0/cr6 struct pass): %d instrs across %d mnemonics" % (rec_instr, sum(1 for m in cnt if is_record(m) and m not in VALID)))
print("\n=== UNVALIDATED remainder by family (instrs, #mnemonics) ===")
for f in sorted(fams, key=lambda k:-fams[k][0]):
    print("  %-14s %8d  (%d mnemonics)" % (f, fams[f][0], fams[f][1]))
print("\n=== 'other' / unclassified mnemonics (need eyes) ===")
for m,c in sorted(cnt.items(), key=lambda kv:-kv[1]):
    if fam(m)=='other':
        print("  %-12s %d" % (m, c))
