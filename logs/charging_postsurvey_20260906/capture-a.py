import sys,time
from pathlib import Path
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/charging_postsurvey_20260906/anchor-a-live');root.mkdir(exist_ok=True)
cohort=[('anchor_a','E46070D247233537')]
probes=[]
try:
 for role,uid in cohort:probes.append(Probe(role,uid,root/(role+'.log'),False))
 deadline=time.monotonic()+60
 while time.monotonic()<deadline:
  for p in probes:p.poll()
  time.sleep(.005)
finally:
 for p in reversed(probes):p.close()
