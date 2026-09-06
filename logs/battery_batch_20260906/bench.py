"""Capture all four probes and observe actual GPIO/RAM during GUI commands."""
import json, os, subprocess, sys, time
from pathlib import Path
from elftools.elf.elffile import ELFFile
sys.path.insert(0,str(Path('firmware/scripts').resolve()))
from bench_rtt import Probe
root=Path('logs/battery_batch_20260906/capture');root.mkdir(exist_ok=True)
cohort=[('gateway','E4645C15CB0F3B37'),('anchor_a','E46070D247233537'),('anchor_b','E46070D247394D36'),('clicker','E4645C15CB365D30')]
if os.environ.get('ACTION_EXPECTED_ANCHORS')=='3':
 cohort[-1]=('anchor_c',cohort[-1][1])
anchor_roles=[role for role,_ in cohort if role.startswith('anchor')]
symbols_by_role={}
for role in anchor_roles:
 build=os.environ.get(role.upper()+'_BUILD',os.environ.get('ACTION_ANCHOR_BUILD','build/scan-fast-anchor'))
 with Path(build+'/zephyr/zephyr.elf').open('rb') as f:
  syms=ELFFile(f).get_section_by_name('.symtab')
  symbols_by_role[role]={n:syms.get_symbol_by_name(n)[0]['st_value'] for n in
    ('status0_identify_active','status0_identify_started_ms','radio_awake','driver_stats')}
trace_symbols={}
probes=[]; proc=None;observations=[];next_observation=0;traces=[];last_trace=0
try:
 for role,uid in cohort:probes.append(Probe(role,uid,root/(role+'.log'),True))
 with (root/'gui.log').open('wb') as log:
  proc=subprocess.Popen([sys.executable,'-u','logs/battery_batch_20260906/gui_hil.py'],stdout=log,stderr=subprocess.STDOUT)
  deadline=time.monotonic()+315
  while time.monotonic()<deadline:
   for p in probes:p.poll()
   now=time.monotonic()
   if now>=next_observation:
    row={'at':now}
    for p in probes:
     if p.role=='gateway' and trace_symbols:
      target=p.session.board.target
      count=target.read32(trace_symbols['mesh_rx_setup_trace_count'])
      if count!=last_trace:
       traces.append({'at':now,'count':count,'ring':target.read_memory_block32(trace_symbols['mesh_rx_setup_trace'],64)})
       last_trace=count
     if not p.role.startswith('anchor'):continue
     target=p.session.board.target
     symbols=symbols_by_role[p.role]
     row[p.role]={'active':target.read8(symbols['status0_identify_active']),
       'started_ms':int.from_bytes(bytes(target.read_memory_block8(symbols['status0_identify_started_ms'],8)),'little'),
       'awake':target.read8(symbols['radio_awake']),
       'gpio0_out':target.read32(0x50000504), 'gpio0_dir':target.read32(0x50000514)}
    observations.append(row);next_observation=now+.035
   if proc.poll() is not None:break
   time.sleep(.005)
  else:raise AssertionError('GUI process exceeded deadline')
  assert proc.returncode==0,'GUI failed, see gui.log'
 checks={}
 for role in anchor_roles:
  active=[row for row in observations if row[role]['active']]
  assert active,role+' never identified'
  assert len({row[role]['started_ms'] for row in active})==1,role+' restarted identification'
  elapsed=active[-1]['at']-active[0]['at']
  assert 9.8<elapsed<10.2,(role,elapsed)
  colors={sum((1<<i) for i,pin in enumerate((20,14,17)) if row[role]['gpio0_out']&(1<<pin)) for row in active}
  assert colors=={0,1,2,4},(role,colors)
  sleeping=sum(not row[role]['awake'] for row in active)
  assert sleeping>len(active)//2,(role,sleeping,len(active))
  assert not any(row[other]['active'] for row in active for other in anchor_roles if other!=role),'another anchor blinked'
  checks[role]={'observed_active_s':elapsed,'samples':len(active),'sleep_samples':sleeping,'colors':sorted(colors)}
 (root/'gpio-checks.json').write_text(json.dumps(checks,indent=2)+'\n')
 print('ANCHOR_ACTIONS_HIL_OK',json.dumps(checks),flush=True)
finally:
 (root/'rx-setup-traces.json').write_text(json.dumps(traces)+'\n')
 (root/'gpio-observations.json').write_text(json.dumps(observations)+'\n')
 if proc and proc.poll() is None:proc.terminate();proc.wait(timeout=5)
 for p in reversed(probes):p.close()
