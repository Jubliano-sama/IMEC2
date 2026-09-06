"""Anchor hit testing, cached hover details and a radio-action context menu."""
from __future__ import annotations

import math
import tkinter as tk
from collections.abc import Callable, Mapping


def nearest_anchor(positions: Mapping[str, tuple[float, float]],
                   project: Callable[[float, float], tuple[float, float]],
                   x: float, y: float) -> str | None:
    closest = None
    distance = 14.0
    for anchor, point in positions.items():
        px, py = project(*point)
        candidate = math.hypot(x - px, y - py)
        if candidate <= distance:
            closest, distance = anchor, candidate
    return closest


class AnchorContext:
    """Installed ahead of layout gestures: background right-drag still rotates."""
    def __init__(self, canvas: tk.Canvas, *, hit_test: Callable[[float, float], str | None],
                 identify: Callable[[str], None], can_identify: Callable[[str], bool],
                 hover_text: Callable[[str], str]):
        self.canvas = canvas
        self.hit_test = hit_test
        self.identify = identify
        self.can_identify = can_identify
        self.hover_text = hover_text
        self.anchor: str | None = None
        self.after_id: str | None = None
        self.tip: tk.Toplevel | None = None
        self.menu = tk.Menu(canvas, tearoff=False)
        self.tag = f"AnchorContext:{canvas}"
        canvas.bindtags((self.tag, *canvas.bindtags()))
        self.bindings = []
        for sequence, handler in (("<ButtonPress-3>", self.popup), ("<Motion>", self.motion),
                                  ("<Leave>", self.hide), ("<ButtonPress>", self.hide)):
            self.bindings.append((sequence, canvas.bind_class(self.tag, sequence, handler)))
        canvas.bind("<Destroy>", self.destroy, add="+")

    def popup(self, event):
        anchor = self.hit_test(event.x, event.y)
        self.hide()
        if anchor is None:
            return None
        self.menu.delete(0, "end")
        self.menu.add_command(label="Blink RGB (10 s)",
                              state="normal" if self.can_identify(anchor) else "disabled",
                              command=lambda: self.identify(anchor) if self.can_identify(anchor) else None)
        try:
            self.menu.tk_popup(event.x_root, event.y_root)
        finally:
            self.menu.grab_release()
        return "break"

    def motion(self, event):
        anchor = self.hit_test(event.x, event.y)
        if anchor == self.anchor:
            return
        self.hide()
        self.anchor = anchor
        if anchor is not None:
            self.after_id = self.canvas.after(400, lambda: self.show(anchor, event.x_root, event.y_root))

    def show(self, anchor, x, y):
        self.after_id = None
        if self.anchor != anchor or not self.canvas.winfo_viewable():
            return
        self.tip = tk.Toplevel(self.canvas)
        self.tip.wm_overrideredirect(True)
        tk.Label(self.tip, text=self.hover_text(anchor), justify="left",
                 background="#202630", foreground="#f2f4f8", relief="solid",
                 borderwidth=1, padx=8, pady=5).pack()
        self.tip.update_idletasks()
        # Keep cached readings visible beside anchors at the screen edges.
        width, height = self.tip.winfo_reqwidth(), self.tip.winfo_reqheight()
        x = max(0, min(x + 14, self.canvas.winfo_screenwidth() - width))
        y = max(0, min(y + 18, self.canvas.winfo_screenheight() - height))
        self.tip.wm_geometry(f"+{x}+{y}")

    def hide(self, _event=None):
        if self.after_id is not None:
            self.canvas.after_cancel(self.after_id)
            self.after_id = None
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None
        self.anchor = None

    def destroy(self, event):
        if event.widget is not self.canvas:
            return
        self.hide()
        for sequence, command in self.bindings:
            self.canvas.unbind_class(self.tag, sequence)
            self.canvas._root().deletecommand(command)
