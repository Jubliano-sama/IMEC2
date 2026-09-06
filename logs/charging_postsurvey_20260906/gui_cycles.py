import sys,time,tkinter as tk,traceback,json
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.protocol import CMD_IDENTIFY_ANCHOR
root=tk.Tk();gui=GatewayGui(root);started=time.monotonic();phase='connect';code=1;cycle=0;terminal_at=0;wait_until=0;blink_queue=[]
anchor_a=0xc1c2306a5138ab2d
control=Path('logs/charging_postsurvey_20260906/final-live')
old=gui._append_log

def log(tag,text):
 print('LOG',tag,text,flush=True);old(tag,text)
gui._append_log=log
root.report_callback_exception=lambda *args:traceback.print_exception(*args)
def finish(ok):
 global code
 code=0 if ok else 1
 print('FINAL',json.dumps({'success':ok,'phase':phase,'elapsed_s':time.monotonic()-started,'anchors':list(gui.anchor_actions.anchors),'batteries':gui.anchor_actions.battery_outcomes}),flush=True)
 gui.transport.disconnect();root.after(500,gui._close)
def blink(identity):
 assert gui._send_anchor_action(CMD_IDENTIFY_ANCHOR,anchor_id=identity)
def poll():
 global phase,cycle,terminal_at,wait_until,blink_queue
 try:
  now=time.monotonic()
  if now-started>360:raise AssertionError('hard test deadline')
  if phase=='connect' and gui.connected and gui.gateway_id and not gui.assignment_replay_barrier.active:
   gui._run_survey();phase='survey';cycle=1
  elif phase=='survey' and gui._survey_phase=='idle':
   assert gui.survey_model.phase=='terminal',gui.survey_model.error
   assert len(gui.anchor_actions.anchors)==3,gui.anchor_actions.anchors
   assert len(gui.survey_model.results)==3,gui.survey_model.results
   assert all(r.usable for r in gui.survey_model.results.values())
   print('SURVEY',cycle,'RESULTS',gui.survey_model.results,flush=True)
   terminal_at=now;gui._read_all_batteries();phase='battery'
  elif phase=='battery' and not gui.anchor_actions.battery_batch_active:
   assert len(gui.anchor_actions.batteries)==3
   assert set(gui.anchor_actions.battery_outcomes.values())=={'complete'}
   print('BATTERIES',cycle,gui.anchor_actions.battery_outcomes,flush=True)
   blink_queue=sorted(gui.anchor_actions.anchors) if cycle==2 else [anchor_a]
   blink(blink_queue[0]);phase='blink'
  elif phase=='blink' and gui.anchor_actions.pending is None:
   identity=blink_queue.pop(0);reply=gui.anchor_actions.replies[identity]
   print('BLINK',cycle,hex(identity),reply,flush=True);assert reply.status==0,reply
   if blink_queue:blink(blink_queue[0])
   elif cycle==2:
    assert str(gui.survey_button['state'])=='normal'
    wait_until=now+11;phase='finish-wait'
   else:wait_until=now+11;phase='reset-wait'
  elif phase=='reset-wait' and now>=wait_until:
   (control/'reset.txt').write_text('anchor_a');wait_until=now+4;phase='reset'
  elif phase=='reset' and now>=wait_until and (control/'reset-done.txt').exists():
   print('RESET_ASSIGNMENT_BEFORE',(control/'assignment-before-reset.json').read_text(),flush=True)
   print('RESET_ASSIGNMENT_AFTER',(control/'assignment-after-reset.json').read_text(),flush=True)
   blink(anchor_a);phase='reset-blink'
  elif phase=='reset-blink' and gui.anchor_actions.pending is None:
   reply=gui.anchor_actions.replies[anchor_a];print('RESET_BLINK',reply,flush=True)
   assert reply.status==7 and reply.stale_assignment,reply
   gui._read_all_batteries();phase='reset-battery'
  elif phase=='reset-battery' and not gui.anchor_actions.battery_batch_active:
   outcomes=gui.anchor_actions.battery_outcomes;print('RESET_BATTERIES',outcomes,flush=True)
   assert outcomes[anchor_a]=='failed' and list(outcomes.values()).count('complete')==2,outcomes
   wait_until=max(terminal_at+65,now);phase='next-wait'
  elif phase=='next-wait' and now>=wait_until:
   assert gui.survey_command_owner.pending is None
   gui._run_survey();phase='survey';cycle=2
  elif phase=='finish-wait' and now>=wait_until:finish(True);return
  root.after(100,poll)
 except Exception:traceback.print_exc();finish(False)
gui.device_text.set('E4:16:B7:C7:E6:95');gui._connect();root.after(100,poll);root.mainloop();sys.exit(code)
