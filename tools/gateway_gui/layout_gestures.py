"""Shared mouse gestures for the survey and click registration canvases."""

from __future__ import annotations

import tkinter as tk
from typing import Callable


def bind_layout_gestures(
    canvas: tk.Canvas,
    *,
    on_translate: Callable[[float, float], None],
    on_scale: Callable[[float], None],
    on_rotate: Callable[[float], None],
    pixels_per_metre: Callable[[], float],
    enabled: Callable[[], bool],
    tool: Callable[[], str] = lambda: "select",
) -> None:
    # Capture the metric conversion once: redraws must not change drag speed.
    drag: tuple[str, float, float, float] | None = None
    tag = f"LayoutTools:{canvas}"
    canvas.bindtags((tag, *canvas.bindtags()))

    bindings: list[tuple[str, str]] = []

    def bind(sequence, callback):
        command = canvas.bind_class(tag, sequence, callback)
        bindings.append((sequence, command))

    def cleanup(_event):
        for sequence, command in bindings:
            canvas.unbind_class(tag, sequence)
            canvas._root().deletecommand(command)

    canvas.bind("<Destroy>", cleanup, add="+")


    def start(event: tk.Event, mode: str) -> str:
        nonlocal drag
        if enabled():
            drag = (mode, event.x, event.y, max(pixels_per_metre(), 1e-6))
            canvas.focus_set()
            canvas.configure(cursor="fleur" if mode == "move" else "exchange")
            canvas.grab_set()
        return "break"

    def move(event: tk.Event) -> str | None:
        nonlocal drag
        if drag is None:
            return None
        mode, x, y, scale = drag
        drag = (mode, event.x, event.y, scale)
        if enabled():
            fine = 0.1 if int(event.state) & 0x0004 else 1.0
            if mode == "move":
                on_translate((event.x - x) / scale * fine, (y - event.y) / scale * fine)
            elif mode == "scale":
                on_scale(1.01 ** ((event.x - x) * fine))
            else:
                on_rotate((x - event.x) * 0.5 * fine)
        return "break"

    def end(event: tk.Event) -> str | None:
        nonlocal drag
        if drag is None:
            return None
        move(event)
        drag = None
        canvas.grab_release()
        canvas.configure(cursor="")
        return "break"

    def wheel(event: tk.Event) -> str:
        if enabled():
            if event.num in (4, 5):
                steps = 1.0 if event.num == 4 else -1.0
            else:
                steps = event.delta / 120.0
                if canvas.tk.call("tk", "windowingsystem") == "aqua":
                    steps = event.delta
            fine = 0.1 if int(event.state) & 0x0004 else 1.0
            on_scale(1.05 ** (max(-20.0, min(20.0, steps)) * fine))
        return "break"

    for sequence, mode in (("<Shift-ButtonPress-1>", "move"),
                           ("<ButtonPress-2>", "move"), ("<ButtonPress-3>", "rotate")):
        bind(sequence, lambda event, mode=mode: start(event, mode))
    for sequence in ("<Shift-B1-Motion>", "<B2-Motion>", "<B3-Motion>"):
        bind(sequence, move)
    # Add handlers before ordinary left dragging so releasing Shift mid-drag
    # still completes the gesture and releases the pointer grab.
    bind("<ButtonPress-1>", lambda event: start(event, tool()) if tool() != "select" else None)
    bind("<B1-Motion>", move)
    for sequence in ("<ButtonRelease-1>", "<ButtonRelease-2>", "<ButtonRelease-3>"):
        bind(sequence, end)
    for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
        bind(sequence, wheel)
