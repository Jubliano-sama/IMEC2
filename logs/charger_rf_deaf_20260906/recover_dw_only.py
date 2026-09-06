"""Invalidate only cached DW configuration while idle; never reset nRF or NVS."""
import time,json
from pathlib import Path
from pyocd.core.helpers import ConnectHelper
s=ConnectHelper.session_with_chosen_probe(unique_id='E46070D247233537',target_override='nrf52833',connect_mode='attach',options={'frequency':4000000});s.open();t=s.board.target
try:
 for attempt in range(100):
  t.halt()
  if not t.read8(536933579):break
  t.resume();time.sleep(.01)
 else:raise RuntimeError('no idle opportunity')
 before={str(a):t.read8(a) for a in [536933579,536933580,536872662]}
 t.write8(536933580,0);t.write8(536872662,1)
 Path('logs/charger_rf_deaf_20260906/dw-only-recovery.json').write_text(json.dumps({'time':time.monotonic(),'before':before,'writes':{'radio_configured':False,'radio_state_unknown':True},'nrf_reset':False}))
finally:t.resume();s.close()
