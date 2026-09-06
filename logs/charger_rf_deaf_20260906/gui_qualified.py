import sys,time,tkinter as tk,traceback,json
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.protocol import CMD_IDENTIFY_ANCHOR
root=tk.Tk();gui=GatewayGui(root);started=time.monotonic();phase='connect';code=1;cycle=1;wait_until=0;blink_queue=[];blink_count=0;battery_count=0
baseline_boots={}
control=Path('logs/charger_rf_deaf_20260906/qualified');old=gui._append_log

def log(tag,text):print('LOG',tag,text,flush=True);old(tag,text)
gui._append_log=log
root.report_callback_exception=lambda *args:traceback.print_exception(*args)
def finish(ok):
 global code
 code=0 if ok else 1
 print('FINAL',json.dumps({'success':ok,'phase':phase,'elapsed_s':time.monotonic()-started,'blinks':blink_count,'batteries':battery_count,'anchors':list(gui.anchor_actions.anchors),'outcomes':gui.anchor_actions.battery_outcomes}),flush=True)
 gui.transport.disconnect();root.after(500,gui._close)
def blink(identity):assert gui._send_anchor_action(CMD_IDENTIFY_ANCHOR,anchor_id=identity)
def poll():
 global phase,cycle,wait_until,blink_queue,blink_count,battery_count,baseline_boots
 try:
  now=time.monotonic()
  if now-started>300:raise AssertionError('hard test deadline')
  if phase=='connect' and gui.connected and gui.gateway_id and not gui.assignment_replay_barrier.active:
   gui._run_survey();phase='survey'
  elif phase=='survey' and gui._survey_phase=='idle':
   assert gui.survey_model.phase=='terminal',gui.survey_model.error
   assert len(gui.anchor_actions.anchors)==3,gui.anchor_actions.anchors
   assert len(gui.survey_model.results)==3,gui.survey_model.results
   assert all(r.success_count==5 for r in gui.survey_model.results.values())
   print('SURVEY',cycle,'RESULTS',gui.survey_model.results,flush=True)
   gui._read_all_batteries();phase='battery'
  elif phase=='battery' and not gui.anchor_actions.battery_batch_active:
   assert len(gui.anchor_actions.batteries)==3
   assert set(gui.anchor_actions.battery_outcomes.values())=={'complete'}
   boots={k:v.boot_counter for k,v in gui.anchor_actions.batteries.items()}
   if cycle==1:baseline_boots=boots
   else:assert boots==baseline_boots,(boots,baseline_boots)
   battery_count+=3;print('BATTERIES',cycle,gui.anchor_actions.batteries,flush=True)
   blink_queue=sorted(gui.anchor_actions.anchors)*3
   blink(blink_queue[0]);phase='blink'
  elif phase=='blink' and gui.anchor_actions.pending is None:
   identity=blink_queue.pop(0);reply=gui.anchor_actions.replies[identity]
   print('BLINK',cycle,hex(identity),reply,flush=True);assert reply.status==0,reply;blink_count+=1
   if blink_queue:blink(blink_queue[0])
   else:wait_until=now+11;phase='pause'
  elif phase=='pause' and now>=wait_until:
   if cycle==1:
    (control/'inject').touch();phase='inject'
   elif cycle==2:
    cycle=3;gui._run_survey();phase='survey'
   else:
    assert blink_count==27 and battery_count==9
    finish(True);return
  elif phase=='inject' and (control/'injected.json').exists():
   print('INJECTION',(control/'injected.json').read_text(),flush=True)
   cycle=2;wait_until=now+3;phase='recover'
  elif phase=='recover' and now>=wait_until:
   # No re-enumeration or nRF reset between hardware fault and commands.
   gui._read_all_batteries();phase='battery'
  root.after(100,poll)
 except Exception:traceback.print_exc();finish(False)
gui.device_text.set('E4:16:B7:C7:E6:95');gui._connect();root.after(100,poll);root.mainloop();sys.exit(code)
