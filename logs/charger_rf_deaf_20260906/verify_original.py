import json,hashlib
from pathlib import Path
from pyocd.core.helpers import ConnectHelper
reference=json.loads(Path('logs/charging_postsurvey_20260906/code-readback-final.json').read_text());result={}
for role,item in reference.items():
 with ConnectHelper.session_with_chosen_probe(unique_id=item['probe'],target_override='nrf52833',connect_mode='attach',options={'frequency':4000000}) as s:
  checks=[]
  for seg in item['segments']:
   digest=hashlib.sha256(bytes(s.board.target.read_memory_block8(seg['start'],seg['end']-seg['start']))).hexdigest();checks.append({'start':seg['start'],'end':seg['end'],'sha256':digest,'match':digest==seg['sha256']})
  result[role]={'segments':checks,'matched':all(c['match'] for c in checks)};print(role,result[role]['matched'],flush=True)
Path('logs/charger_rf_deaf_20260906/original-firmware-readback.json').write_text(json.dumps(result,indent=2));assert all(r['matched'] for r in result.values())
