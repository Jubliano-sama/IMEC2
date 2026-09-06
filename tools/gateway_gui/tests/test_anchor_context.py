import tkinter as tk
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

from tools.gateway_gui.anchor_context import AnchorContext, nearest_anchor
from tools.gateway_gui.diagnostic_views import ClickDiagnosticsView
from tools.gateway_gui.survey_view import SurveyGeometryView
from tools.gateway_gui.layout_gestures import bind_layout_gestures


class AnchorContextTests(unittest.TestCase):
    def setUp(self):
        try: self.root = tk.Tk()
        except tk.TclError as exc: self.skipTest(str(exc))
        self.root.geometry('900x750+0+0')
        self.root.update()

    def tearDown(self): self.root.destroy()

    def test_hit_testing_and_background_gesture_dispatch(self):
        canvas = tk.Canvas(self.root, width=400, height=300)
        canvas.pack(); self.root.update()
        rotate, identify = Mock(), Mock()
        bind_layout_gestures(canvas, on_translate=Mock(), on_scale=Mock(), on_rotate=rotate,
                             pixels_per_metre=lambda:100, enabled=lambda:True)
        context = AnchorContext(canvas, hit_test=lambda x,y: 'a' if x<100 else None,
                                identify=identify, can_identify=lambda _:True,
                                hover_text=lambda _: 'Battery: 3.700 V')
        with patch.object(context.menu, 'tk_popup') as popup:
            canvas.event_generate('<ButtonPress-3>', x=50, y=50)
            popup.assert_called_once()
            context.menu.invoke(0)
            identify.assert_called_once_with('a')
            canvas.event_generate('<B3-Motion>', x=60,y=50)
            canvas.event_generate('<ButtonRelease-3>', x=60,y=50)
            rotate.assert_not_called()
            canvas.event_generate('<ButtonPress-3>', x=150,y=50)
            canvas.event_generate('<B3-Motion>', x=160,y=50)
            canvas.event_generate('<ButtonRelease-3>', x=160,y=50)
            self.assertAlmostEqual(sum(call.args[0] for call in rotate.call_args_list), -5)
        self.assertEqual(nearest_anchor({'a':(1,2)}, lambda x,y:(x*10,y*10),10,20), 'a')
        self.assertIsNone(nearest_anchor({'a':(1,2)},lambda x,y:(x*10,y*10),100,200))

    def test_menu_rechecks_survey_guard_and_hover_only_reads_cache(self):
        canvas=tk.Canvas(self.root);canvas.pack();self.root.update()
        identify=Mock(); allowed=[True]; cache=Mock(return_value='Battery: 3.712 V · received 8 s ago')
        context=AnchorContext(canvas,hit_test=lambda x,y:'a', identify=identify,
            can_identify=lambda _:allowed[0],hover_text=cache)
        event=SimpleNamespace(x=20,y=20,x_root=30,y_root=30)
        with patch.object(context.menu,'tk_popup'):
            context.popup(event)
            allowed[0]=False
            context.menu.invoke(0)
            identify.assert_not_called()
            context.popup(event)
            self.assertEqual(str(context.menu.entrycget(0,'state')), 'disabled')
        context.motion(event)
        canvas.after_cancel(context.after_id);context.after_id=None
        context.show('a',30,30)
        self.assertIsNotNone(context.tip)
        self.assertIn('3.712 V',context.tip.winfo_children()[0].cget('text'))
        cache.assert_called_once_with('a');identify.assert_not_called()
        context.hide();self.assertIsNone(context.tip)
        context.motion(event)
        canvas.destroy()  # Pending hover callback and Tcl bindings are retired.
        self.root.update()

    def test_both_views_and_fullscreen_canvases_install_anchor_actions(self):
        for kind in ('click','survey'):
            identify=Mock();can=Mock(return_value=True);hover=Mock(return_value='3.70 V')
            cls=ClickDiagnosticsView if kind=='click' else SurveyGeometryView
            view=cls(self.root,on_identify_anchor=identify,can_identify_anchor=can,anchor_hover_text=hover)
            view.pack(fill='both',expand=True);self.root.update()
            if kind=='click':
                view.positions={'a':(0.,0.),'b':(5.,0.)}
                view.reference_positions=dict(view.positions)
                canvas=view.canvas
                project=view._projection_for_canvas(canvas)
                x,y=project(0.,0.)
                binder=view._bind_layout_gestures
            else:
                view.model=SimpleNamespace(slot_to_anchor={0:1,1:2})
                view._display_positions={'a':(0.,0.),'b':(5.,0.)}
                view._oriented_positions=dict(view._display_positions)
                canvas=view.canvas
                from tools.gateway_gui.survey_view import _canvas_projection
                from tools.gateway_gui.layout_motion import rotation_safe_bounds
                project=_canvas_projection(rotation_safe_bounds(view._oriented_positions),max(canvas.winfo_width(),160),max(canvas.winfo_height(),80))
                x,y=project.project(0.,0.)
                binder=view._bind_anchor_dragging
            self.assertEqual(view._anchor_at(canvas,x,y),'a')
            self.assertTrue(str(canvas.bindtags()[0]).startswith('AnchorContext:'))
            extra=tk.Toplevel(self.root);full=tk.Canvas(extra);full.pack();binder(full)
            self.assertTrue(str(full.bindtags()[0]).startswith('AnchorContext:'))
            extra.destroy();view.destroy();self.root.update()
