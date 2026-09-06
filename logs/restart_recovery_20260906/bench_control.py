import sys,time,json,traceback
from pathlib import Path
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/restart_recovery_20260906/reproduce');root.mkdir(exist_ok=True)
cohort=[('gateway','E4645C15CB0F3B37'),('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('anchor_c','E4645C15CB365D30')]
probes=[];states={}
try:
 for role,uid in cohort:
  try:
   p=Probe(role,uid,root/(role+'.log'),False);probes.append(p);states[role]='attached'
  except Exception as exc: states[role]=str(exc)
  print(role,states[role],flush=True)
 (root/'ready.json').write_text(json.dumps(states))
 deadline=time.monotonic()+220
 while time.monotonic()<deadline:
  control=root/'reset.txt'
  if control.exists():
   role=control.read_text().strip();control.unlink()
   p=next(p for p in probes if p.role==role);p.session.board.target.reset()
   print('RESET',role,time.monotonic(),flush=True)
   (root/'reset-done.txt').write_text(role)
  for p in probes:
   try:p.poll()
   except Exception as exc:print('POLL_ERROR',p.role,str(exc),flush=True)
  if (root/'stop').exists():break
  time.sleep(.01)
finally:
 for p in reversed(probes):p.close()
