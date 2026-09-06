"""Small reusable windows that keep editing controls off the live maps."""

import tkinter as tk
from tkinter import ttk


def show_dialog(window: tk.Toplevel) -> None:
    window.deiconify()
    window.update_idletasks()
    owner = window.master.winfo_toplevel()
    width, height = window.winfo_width(), window.winfo_height()
    x = owner.winfo_rootx() + (owner.winfo_width() - width) // 2
    y = owner.winfo_rooty() + (owner.winfo_height() - height) // 2
    x = max(0, min(x, window.winfo_screenwidth() - width))
    y = max(24, min(y, window.winfo_screenheight() - height - 48))
    window.geometry(f"+{x}+{y}")
    window.lift()
    window.focus_set()


def tabbed_dialog(parent: tk.Misc, title: str, labels: tuple[str, ...]):
    window = tk.Toplevel(parent)
    window.title(title)
    window.withdraw()
    window.protocol("WM_DELETE_WINDOW", window.withdraw)
    notebook = ttk.Notebook(window)
    notebook.pack(fill="both", expand=True, padx=8, pady=8)
    pages = []
    for label in labels:
        page = ttk.Frame(notebook, padding=8)
        page.columnconfigure(0, weight=1)
        notebook.add(page, text=label)
        pages.append(page)
    return window, pages
