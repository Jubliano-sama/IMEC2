import json,hashlib
from pathlib import Path
from pyocd.core.helpers import ConnectHelper
from intelhex import IntelHex
result={}
for role,uid in [('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('anchor_c','E4645C15CB365D30'),('gateway','E4645C15CB0F3B37')]:
 ih=IntelHex('build/scan-production-'+('gateway' if role=='gateway' else 'anchor')+'/zephyr/zephyr.hex')
 with ConnectHelper.session_with_chosen_probe(unique_id=uid,target_override='nrf52833',connect_mode='attach',options={'frequency':4000000}) as s:
  checks=[]
  for start,end in ih.segments():
   if start>=0x7a000:continue
   expected=bytes(ih.tobinarray(start=start,end=end-1));actual=bytes(s.board.target.read_memory_block8(start,end-start));checks.append({'start':start,'end':end,'sha256':hashlib.sha256(actual).hexdigest(),'match':actual==expected})
  result[role]={'probe':uid,'segments':checks,'matched':all(c['match'] for c in checks),'state':str(s.board.target.get_state())};print(role,result[role]['matched'],flush=True)
Path('logs/charger_rf_deaf_20260906/fixed-firmware-readback.json').write_text(json.dumps(result,indent=2));assert all(r['matched'] for r in result.values())
