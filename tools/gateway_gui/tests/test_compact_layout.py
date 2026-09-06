"""Real Tk layout checks for maps, actions, and editors on a 720p desktop."""
import tkinter as tk
from tkinter import ttk
import unittest
from unittest.mock import patch

from tools.gateway_gui.app import GatewayGui
from tools.gateway_gui.anchor_actions import EnumeratedAnchor
from tools.gateway_gui.compact_dialog import show_dialog


class CompactLayoutTests(unittest.TestCase):
    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(str(exc))
        self.transport = patch('tools.gateway_gui.app.BleTransport').start()
        self.addCleanup(patch.stopall)
        self.gui = GatewayGui(self.root)
        self.root.update()

    def tearDown(self):
        for callback in self.root.tk.call("after", "info"):
            self.root.after_cancel(callback)
        self.gui._close()

    def assert_visible_inside(self, widget, window):
        self.assertTrue(widget.winfo_viewable(), str(widget))
        x = widget.winfo_rootx() - window.winfo_rootx()
        y = widget.winfo_rooty() - window.winfo_rooty()
        self.assertGreaterEqual(x, 0, str(widget))
        self.assertGreaterEqual(y, 0, str(widget))
        self.assertLessEqual(x + widget.winfo_width(), window.winfo_width(), str(widget))
        self.assertLessEqual(y + widget.winfo_height(), window.winfo_height(), str(widget))

    def test_maps_and_primary_actions_fit_with_room_for_desktop_decorations(self):
        gui = self.gui
        for size in ('1280x720', '1240x660'):
            self.root.geometry(size)
            for tab, view in ((gui.click_location_tab, gui.click_diagnostics_view),
                              (gui.survey_geometry_tab, gui.survey_geometry_view)):
                gui.activity_notebook.select(tab)
                self.root.update()
                self.assertGreaterEqual(view.canvas.winfo_width(), 800)
                self.assertGreaterEqual(view.canvas.winfo_height(), 250)
                for widget in (view.canvas, view.settings_button, gui.anchor_battery_button,
                               gui.anchor_identify_button, gui.assignment_button,
                               gui.survey_button, gui.survey_cancel_button):
                    self.assert_visible_inside(widget, self.root)
                self.assertNotIn(str(gui.packet_inspector), gui.activity_notebook.master.panes())
            gui.activity_notebook.select(0)
            self.root.update()
            self.assertIn(str(gui.packet_inspector), gui.activity_notebook.master.panes())

    def test_every_editor_page_and_all_battery_rows_remain_accessible(self):
        gui = self.gui
        self.root.geometry('1240x660+0+0')
        self.root.update()
        windows = (gui.click_diagnostics_view.settings_window,
                   gui.survey_geometry_view.settings_window,
                   gui.survey_geometry_view.details_window,
                   gui.more_actions_window, gui.battery_window)
        def check_children(parent, window):
            for child in parent.winfo_children():
                if isinstance(child, (tk.Toplevel, tk.Menu)) or not child.winfo_viewable():
                    continue
                self.assert_visible_inside(child, window)
                check_children(child, window)
        for window in windows:
            show_dialog(window)
            self.root.update()
            self.assertLessEqual(window.winfo_width(), 1200)
            self.assertLessEqual(window.winfo_height(), 600)
            notebooks = [w for w in window.winfo_children() if isinstance(w, ttk.Notebook)]
            if notebooks:
                for tab in notebooks[0].tabs():
                    notebooks[0].select(tab)
                    self.root.update()
                    check_children(window, window)
            else:
                check_children(window, window)
            window.withdraw()
        gui.anchor_actions.anchors = {n + 1: EnumeratedAnchor(n + 1, n, 1) for n in range(50)}
        gui._update_command_state()
        show_dialog(gui.battery_window)
        self.root.update()
        last = gui.battery_tree.get_children()[-1]
        gui.battery_tree.see(last)
        self.root.update()
        self.assertTrue(gui.battery_tree.bbox(last))

    def test_fullscreen_reuses_editors_and_leaves_map_space(self):
        for view in (self.gui.click_diagnostics_view, self.gui.survey_geometry_view):
            view._open_fullscreen()
            window = view._fullscreen_window
            window.attributes('-fullscreen', False)
            window.geometry('1280x720')
            self.root.update()
            self.assertGreaterEqual(view._fullscreen_canvas.winfo_height(), 550)
            self.assert_visible_inside(view._fullscreen_canvas, window)
            header = next(w for w in window.winfo_children() if isinstance(w, ttk.Frame))
            edit = next(w for w in header.winfo_children()
                        if isinstance(w, ttk.Button) and str(w.cget('text')).startswith('Edit'))
            edit.invoke()
            self.root.update()
            self.assertTrue(view.settings_window.winfo_viewable())
            view.settings_window.withdraw()
            view._close_fullscreen()
