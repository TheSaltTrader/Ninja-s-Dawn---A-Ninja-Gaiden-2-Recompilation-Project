import re
p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
def rep(a,b):
    global s
    a=a.replace('\n',nl); b=b.replace('\n',nl)
    assert s.count(a)==1,a[:70]; s=s.replace(a,b,1)
rep('''  if (vsnapc && XXH3_64bits(Phys(rec.vs_addr), rec.vs_dwords * 4) != rec.vs_hash) ++g_st.ucode_moved;
  if (psnapc && XXH3_64bits(Phys(rec.ps_addr), rec.ps_dwords * 4) != rec.ps_hash) ++g_st.ucode_moved;''',
'''  auto moved = [](uint32_t addr, uint32_t dw, uint64_t h) {
    const uint8_t* q = Phys(addr);
    return !addr || !PageReadable(q) || !PageReadable(q + dw * 4 - 1) || XXH3_64bits(q, dw * 4) != h;
  };
  if (vsnapc && moved(rec.vs_addr, rec.vs_dwords, rec.vs_hash)) ++g_st.ucode_moved;
  if (psnapc && moved(rec.ps_addr, rec.ps_dwords, rec.ps_hash)) ++g_st.ucode_moved;''')
i=s.index("      if (rec.vs_hash == 0x8262E1111C273B50ull && count == 3) {")
j=s.index("      }"+nl+"      }"+nl, i)+len("      }"+nl+"      }"+nl)
s=s[:i]+s[j:]
open(p,'w',encoding='utf-8',newline='').write(s); print('ok')
