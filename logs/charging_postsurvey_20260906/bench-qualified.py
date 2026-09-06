import sys,time,json,traceback
from pathlib import Path
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/charging_postsurvey_20260906/qualified-live');root.mkdir(exist_ok=True)
cohort=[('gateway','E4645C15CB0F3B37'),('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('anchor_c','E4645C15CB365D30')]
from elftools.elf.elffile import ELFFile
with open('build/scan-production-anchor/zephyr/zephyr.elf','rb') as f:
 table=ELFFile(f).get_section_by_name('.symtab')
 symbols={name:(table.get_symbol_by_name(name)[0]['st_value'],table.get_symbol_by_name(name)[0]['st_size']) for name in ('anchor_discovery_assignment_policy','anchor_discovery_assignment_slot','anchor_discovery_assignment_slot_count')}
def snapshot(probe):
 target=probe.session.board.target
 data={n:bytes(target.read_memory_block8(a,z)) for n,(a,z) in symbols.items()}
 return {'epoch':int.from_bytes(data['anchor_discovery_assignment_policy'][:4],'little'), 'slot':data['anchor_discovery_assignment_slot'][0], 'slot_count':data['anchor_discovery_assignment_slot_count'][0], 'raw_policy':data['anchor_discovery_assignment_policy'].hex()}
probes=[];states={}
try:
 for role,uid in cohort:
  try:
   p=Probe(role,uid,root/(role+'.log'),False);probes.append(p);states[role]='attached'
  except Exception as exc: states[role]=str(exc)
  print(role,states[role],flush=True)
 (root/'ready.json').write_text(json.dumps(states))
 (root/'restored-before-survey.json').write_text(json.dumps(snapshot(next(p for p in probes if p.role=='anchor_a')),indent=2))
 deadline=time.monotonic()+500
 while time.monotonic()<deadline:
  control=root/'reset.txt'
  if control.exists():
   role=control.read_text().strip();control.unlink()
   selected=[p for p in probes if p.role.startswith('anchor')] if role=='all_anchors' else [next(p for p in probes if p.role==role)]
   (root/'assignment-before-reset.json').write_text(json.dumps({p.role:snapshot(p) for p in selected},indent=2))
   for p in selected:p.session.board.target.reset()
   print('RESET',role,time.monotonic(),flush=True)
   time.sleep(5)
   (root/'assignment-after-reset.json').write_text(json.dumps({p.role:snapshot(p) for p in selected},indent=2))
   (root/'reset-done.txt').write_text(role)
  for p in probes:
   try:p.poll()
   except Exception as exc:print('POLL_ERROR',p.role,str(exc),flush=True)
  if (root/'stop').exists():break
  time.sleep(.01)
finally:
 for p in reversed(probes):p.close()
