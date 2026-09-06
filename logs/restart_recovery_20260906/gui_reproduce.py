import sys,time,json,tkinter as tk,traceback
from pathlib import Path
sys.path.insert(0,str(Path.cwd()))
from tools.gateway_gui.app import GatewayGui
root=tk.Tk();gui=GatewayGui(root);start=time.monotonic();phase='connect';roster=[];finish_at=0
old=gui._append_log
def log(tag,message):print('LOG',tag,message,flush=True);old(tag,message)
gui._append_log=log
root.report_callback_exception=lambda *args: traceback.print_exception(*args)
def finish():
 print('FINAL',phase,'elapsed',time.monotonic()-start,flush=True)
 gui.transport.disconnect();root.after(500,gui._close)
def poll():
 global phase,roster,finish_at
 now=time.monotonic()
 if now-start>150:finish();return
 if phase=='connect' and gui.connected and gui.gateway_id and not gui.assignment_replay_barrier.active:
  gui.assignment_expected_anchors_text.set('3');gui._send_assign_discovery_slots(ram_only_iteration=True);phase='enumeration'
 elif phase=='enumeration' and not gui.command_orchestrator.active:
  roster=list(gui.anchor_actions.anchors);print('ROSTER',roster,flush=True)
  if not roster:finish();return
  gui._read_all_batteries();phase='battery-before'
 elif phase=='battery-before' and not gui.anchor_actions.battery_batch_active:
  print('BATTERY_BEFORE',gui.anchor_actions.battery_outcomes,flush=True)
  Path('logs/restart_recovery_20260906/reproduce/reset.txt').write_text('anchor_a')
  finish_at=now+3;phase='reset'
 elif phase=='reset' and now>=finish_at:
  gui._read_all_batteries();phase='battery-after'
 elif phase=='battery-after' and not gui.anchor_actions.battery_batch_active:
  print('BATTERY_AFTER',gui.anchor_actions.battery_outcomes,flush=True);print('CONTROLS',gui.command_availability_text.get(),flush=True);finish();return
 pending=gui.command_request_tracker.pending
 print('TICK',round(now-start,1),phase,gui._survey_phase,'pending',pending,'batch',gui.anchor_actions.battery_queue,flush=True)
 root.after(1000,poll)
gui.device_text.set('E4:16:B7:C7:E6:95');gui._connect();root.after(100,poll);root.mainloop()
