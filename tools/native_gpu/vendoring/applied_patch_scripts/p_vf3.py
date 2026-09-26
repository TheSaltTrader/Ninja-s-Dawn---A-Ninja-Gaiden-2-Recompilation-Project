p=r"C:\users\renoi\claudecode\Fable 2 Recompile Xbox\wt-fable2-nativegpu\src\native_gpu_present.cpp"
s=open(p,encoding='utf-8',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
a="      const bool auto_idx = ((d->draw_initiator >> 6) & 3u) != 0u;   // as the replay's auto_index"
assert s.count(a)==1
b=a+nl+'''      if (rec.vs_hash == 0x8262E1111C273B50ull && count == 3) {   // TEMP: the lake blit at record time
        static uint32_t n = 0;
        if (n < 6) {
          ++n;
          const uint32_t d0 = d->regs[0x4800], d1 = d->regs[0x4801], ph = (d0 & ~3u) & 0x1FFFFFFFu;
          const uint8_t* q = PBase() + ph;
          std::string used_s;
          for (const auto& [fc, st] : used) { if (used_s.size() < 80) used_s += std::to_string(fc) + "/" + std::to_string(st) + " "; }
          REXLOG_INFO("[ngpu] VSNAP BLIT record: prim {} src {} vf0 {:08X} {:08X} known {} used [{}] bytes@{:08X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", d->draw_initiator & 0x3F,
                      (d->draw_initiator >> 6) & 3, d0, d1, known, used_s, ph, BE32(q), BE32(q + 4), BE32(q + 8), BE32(q + 12), BE32(q + 16), BE32(q + 20));
        }
      }'''
s=s.replace(a,b.replace('\n',nl),1)
open(p,'w',encoding='utf-8',newline='').write(s); print('ok')
