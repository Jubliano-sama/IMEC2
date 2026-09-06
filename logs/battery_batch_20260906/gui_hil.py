import json, sys, time, tkinter as tk, traceback
from pathlib import Path
from dataclasses import asdict
sys.path.insert(0,str(Path.cwd()))
from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.protocol import CMD_IDENTIFY_ANCHOR
from tools.gateway_gui.diagnostic_models import anchor_label
from tools.gateway_gui.survey_view import _canvas_projection
from tools.gateway_gui.layout_motion import rotation_safe_bounds

class Gui(GatewayGui):
 def _append_log(self,tag,message):
  print('GUI_LOG',tag,message,flush=True);super()._append_log(tag,message)
 def _show_error(self,message):
  print('GUI_ERROR',message,flush=True);super()._show_error(message)

root=tk.Tk();gui=Gui(root);root.geometry('1280x720+0+0');gui.assignment_expected_anchors_text.set('3')
started=time.monotonic();phase='connect';outcome=1;first={};ids=[];index=0;wait_until=0

def finish(ok):
 global outcome
 outcome=0 if ok else 1
 print('BATCH_FINAL',json.dumps({'success':ok,'batteries':{str(k):asdict(v) for k,v in gui.anchor_actions.batteries.items()},'elapsed_s':time.monotonic()-started}),flush=True)
 gui.transport.disconnect();root.after(1500,root.destroy)

def map_context(node,kind):
 view=gui.click_diagnostics_view if kind=='click' else gui.survey_geometry_view
 gui.activity_notebook.select(gui.click_location_tab if kind=='click' else gui.survey_geometry_tab)
 root.update_idletasks()
 canvas=view.canvas;key=anchor_label(node)
 if kind=='click':
  project=view._projection_for_canvas(canvas);x,y=project(*view.positions[key])
 else:
  positions=view._display_positions or view._fallback_positions(view.model)
  project=_canvas_projection(rotation_safe_bounds(view._oriented_positions or positions),max(canvas.winfo_width(),160),max(canvas.winfo_height(),80))
  x,y=project.project(*positions[key])
 print('MAP_GEOMETRY',kind,canvas.winfo_geometry(),canvas.winfo_viewable(),x,y,flush=True)
 assert view._anchor_at(canvas,x,y)==key,(kind,key,x,y)
 canvas.event_generate('<Motion>',x=round(x),y=round(y),warp=True)
 return canvas,x,y

def poll():
 global phase,first,ids,index,wait_until
 try:
  if time.monotonic()-started>250:raise AssertionError('deadline')
  if phase=='connect' and gui.connected and gui.gateway_id and not gui.command_orchestrator.active and not gui.assignment_replay_barrier.active:
   assert str(gui.anchor_battery_button['state'])=='disabled'
   gui._run_survey();phase='survey'
  elif phase=='survey' and gui._survey_phase=='idle' and gui.survey_model.phase=='terminal' and not gui.command_orchestrator.active:
   assert len(gui.anchor_actions.anchors)==3
   if len(gui.click_diagnostics_view.positions)!=3:
    root.after(100,poll);return
   ids=list(sorted(gui.anchor_actions.anchors))
   gui.anchor_battery_button.invoke();phase='batch1'
   assert gui.anchor_actions.battery_batch_active
   assert str(gui.survey_button['state'])=='disabled'
  elif phase in ('batch1','batch2') and not gui.anchor_actions.battery_batch_active and gui.anchor_actions.pending is None:
   model=gui.anchor_actions
   assert len(model.batteries)==3 and set(model.battery_outcomes.values())=={'complete'},model.battery_outcomes
   assert len(gui.battery_tree.get_children())==3
   print('BATTERY_BATCH',json.dumps({'phase':phase,'readings':{str(k):asdict(v) for k,v in model.batteries.items()}}),flush=True)
   if phase=='batch1':
    first=dict(model.batteries);gui.anchor_battery_button.invoke();phase='batch2'
   else:
    assert all(model.batteries[k].sampled_at_ms>v.sampled_at_ms for k,v in first.items())
    gui.battery_window.withdraw()
    phase='hover';wait_until=0
  elif phase=='hover':
   kind='survey' if index%2 else 'click'
   if not wait_until:
    map_context(ids[index],kind);wait_until=time.monotonic()+.7
   elif time.monotonic()>=wait_until:
    view=gui.click_diagnostics_view if kind=='click' else gui.survey_geometry_view
    tips=[w for w in view.canvas.winfo_children() if isinstance(w,tk.Toplevel)]
    assert tips,(kind,'hover absent')
    label=tips[-1].winfo_children()[0].cget('text')
    expected=f'{gui.anchor_actions.batteries[ids[index]].battery_mv/1000:.3f} V'
    assert expected in label,(label,expected)
    print('HOVER_OK',kind,ids[index],label,flush=True)
    canvas,x,y=map_context(ids[index],kind)
    canvas.event_generate('<ButtonPress-3>',x=round(x),y=round(y))
    menus=[w for w in canvas.winfo_children() if isinstance(w,tk.Menu)]
    menu=next(m for m in menus if m.index('end') is not None and m.entrycget(0,'label')=='Blink RGB (10 s)')
    menu.invoke(0);menu.unpost()
    assert gui.anchor_actions.pending and gui.anchor_actions.pending.command_id==CMD_IDENTIFY_ANCHOR
    phase='blink'
  elif phase=='blink' and gui.anchor_actions.pending is None:
   result=gui.anchor_actions.replies[ids[index]]
   assert result.status==0 and result.battery_mv is None
   assert ids[index] in gui.anchor_actions.batteries
   print('MAP_BLINK_OK',ids[index],flush=True)
   wait_until=time.monotonic()+11;phase='blink-wait'
  elif phase=='blink-wait' and time.monotonic()>=wait_until:
   index+=1;wait_until=0
   if index==len(ids):finish(True);return
   phase='hover'
  root.after(50,poll)
 except Exception:
  traceback.print_exc();finish(False)

gui.device_text.set('E4:16:B7:C7:E6:95');gui._connect();root.after(100,poll)
root.mainloop();sys.exit(outcome)
