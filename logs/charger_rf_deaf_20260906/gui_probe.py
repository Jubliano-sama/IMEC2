import sys,time,tkinter as tk,traceback,json
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.gateway_gui.app import GatewayGui
root=tk.Tk();gui=GatewayGui(root);started=time.monotonic();phase='connect';code=1;connected_at=None
old=gui._append_log
def log(tag,text):print('LOG',tag,text,flush=True);old(tag,text)
gui._append_log=log
root.report_callback_exception=lambda *args:traceback.print_exception(*args)
def finish(ok):
 global code
 code=0 if ok else 1
 print('FINAL',json.dumps({'success':ok,'phase':phase,'elapsed_s':time.monotonic()-started,'anchors':list(gui.anchor_actions.anchors),'batteries':gui.anchor_actions.battery_outcomes}),flush=True)
 gui.transport.disconnect();root.after(500,gui._close)
def poll():
 global phase,connected_at
 try:
  if time.monotonic()-started>140:raise AssertionError('hard test deadline')
  if phase=='connect' and gui.connected and gui.gateway_id and not gui.assignment_replay_barrier.active:
   if connected_at is None:connected_at=time.monotonic();print('DRAINING existing gateway backlog without reset',flush=True)
   if time.monotonic()-connected_at>12:gui._run_survey();phase='survey'
  elif phase=='survey' and gui._survey_phase=='idle' and gui.survey_model.phase=='terminal':
   assert len(gui.anchor_actions.anchors)==3,gui.anchor_actions.anchors
   assert len(gui.survey_model.results)==3,gui.survey_model.results
   print('SURVEY_RESULTS',gui.survey_model.results,flush=True)
   assert all(r.usable for r in gui.survey_model.results.values())
   gui._read_all_batteries();phase='battery'
  elif phase=='battery' and not gui.anchor_actions.battery_batch_active:
   assert len(gui.anchor_actions.batteries)==3
   assert set(gui.anchor_actions.battery_outcomes.values())=={'complete'}
   assert str(gui.survey_button['state'])=='normal'
   finish(True);return
  root.after(100,poll)
 except Exception:
  traceback.print_exc();finish(False)
gui.device_text.set('E4:16:B7:C7:E6:95');gui._connect();root.after(100,poll);root.mainloop();sys.exit(code)
