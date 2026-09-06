import sys,time,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/charger_rf_deaf_20260906/recovery')
cohort=[('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('anchor_c','E4645C15CB365D30'),('gateway','E4645C15CB0F3B37')]
probes=[]
try:
 for role,uid in cohort:
  p=Probe(role,uid,root/(role+'.log'),False);probes.append(p)
  t=p.session.board.target
  ram=bytes(t.read_memory_block8(0x20000000,0x20000));(root/(role+'-initial-ram.bin')).write_bytes(ram)
  with open('build/scan-production-'+('gateway' if role=='gateway' else 'anchor')+'/zephyr/zephyr.elf','rb') as f:
   syms=ELFFile(f).get_section_by_name('.symtab');result={}
   for s in syms.iter_symbols():
    n=s.name;a=s['st_value'];z=s['st_size']
    if 0x20000000<=a<0x20020000 and z and any(k in n for k in ('radio_','phy_mode','last_rx_debug','discovery_assignment','driver_stats','survey_active')):
     result[n]={'address':a,'size':z,'hex':ram[a-0x20000000:a-0x20000000+z].hex()}
  result['cpu_state']=str(t.get_state());result['spi_frequency']=hex(t.read32(0x4002f524))
  (root/(role+'-initial.json')).write_text(json.dumps(result,indent=2));print(role,result['cpu_state'],flush=True)
 (root/'ready').touch()
 deadline=time.monotonic()+900
 while time.monotonic()<deadline and not (root/'stop').exists():
  for p in probes:
   try:p.poll()
   except Exception as e: print(p.role,repr(e),flush=True)
  time.sleep(.01)
finally:
 for p in reversed(probes):p.close()
