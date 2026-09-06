import sys,time
from pathlib import Path
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/charging_postsurvey_20260906/recovery-live');root.mkdir(exist_ok=True)
cohort=[('gateway','E4645C15CB0F3B37'),('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('anchor_c','E4645C15CB365D30')]
probes=[]
try:
 for role,uid in cohort:
  try:probes.append(Probe(role,uid,root/(role+'.log'),False));print(role,'attached',flush=True)
  except Exception as exc:print(role,'ATTACH_ERROR',str(exc),flush=True)
 deadline=time.monotonic()+300
 while time.monotonic()<deadline:
  for p in probes:
   try:p.poll()
   except Exception as exc:print(p.role,'POLL_ERROR',str(exc),flush=True)
  if (root/'stop').exists():break
  time.sleep(.005)
finally:
 for p in reversed(probes):p.close()
