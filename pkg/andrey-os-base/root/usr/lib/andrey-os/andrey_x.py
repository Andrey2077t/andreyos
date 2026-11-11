# -*- coding: utf-8 -*-
"""Pequeno acesso ao X11 via ctypes (sem depender de python-xlib).

Usado por andrey-superkey (XRecord) e andrey-clipboard (colar com XTest).
Tudo aqui é opcional: se as bibliotecas ou o X não existirem, as funções
devolvem False/None em vez de levantar exceção.
"""
import ctypes
from ctypes import (POINTER, Structure, byref, c_int, c_char_p, c_ubyte,
                    c_uint, c_ulong, c_void_p)

KEYSYMS = {
    "Super_L": 0xFFEB, "Super_R": 0xFFEC,
    "Control_L": 0xFFE3, "Shift_L": 0xFFE1,
    "v": 0x0076, "Insert": 0xFF63,
}


def _load(*names):
    for n in names:
        try:
            return ctypes.CDLL(n)
        except OSError:
            continue
    return None


x11 = _load("libX11.so.6")
xtst = _load("libXtst.so.6")

if x11 is not None:
    x11.XInitThreads.restype = c_int
    x11.XOpenDisplay.restype = c_void_p
    x11.XOpenDisplay.argtypes = [c_char_p]
    x11.XCloseDisplay.argtypes = [c_void_p]
    x11.XKeysymToKeycode.restype = c_ubyte
    x11.XKeysymToKeycode.argtypes = [c_void_p, c_ulong]
    x11.XFlush.argtypes = [c_void_p]
    x11.XSync.argtypes = [c_void_p, c_int]
    x11.XFree.argtypes = [c_void_p]
    x11.XGetInputFocus.argtypes = [c_void_p, POINTER(c_ulong), POINTER(c_int)]
    x11.XQueryTree.argtypes = [c_void_p, c_ulong, POINTER(c_ulong), POINTER(c_ulong),
                               POINTER(POINTER(c_ulong)), POINTER(c_uint)]

if xtst is not None:
    xtst.XTestFakeKeyEvent.argtypes = [c_void_p, c_uint, c_int, c_ulong]

_threads_ready = False


def init_threads():
    global _threads_ready
    if x11 is not None and not _threads_ready:
        x11.XInitThreads()
        _threads_ready = True


def open_display():
    """Abre uma conexão nova com o X (ou None)."""
    if x11 is None:
        return None
    init_threads()
    dpy = x11.XOpenDisplay(None)
    return dpy or None


class _ClassHint(Structure):
    _fields_ = [("res_name", c_void_p), ("res_class", c_void_p)]


def _window_class(dpy, win):
    hint = _ClassHint()
    if not x11.XGetClassHint(dpy, win, byref(hint)):
        return None
    try:
        name = ctypes.string_at(hint.res_name).decode("utf-8", "replace") if hint.res_name else ""
        cls = ctypes.string_at(hint.res_class).decode("utf-8", "replace") if hint.res_class else ""
    finally:
        if hint.res_name:
            x11.XFree(hint.res_name)
        if hint.res_class:
            x11.XFree(hint.res_class)
    return name, cls


if x11 is not None:
    x11.XGetClassHint.argtypes = [c_void_p, c_ulong, POINTER(_ClassHint)]


def focused_window_class(dpy):
    """(res_name, res_class) da janela que está com o foco, subindo pelos pais."""
    if x11 is None or not dpy:
        return None
    win, revert = c_ulong(0), c_int(0)
    x11.XGetInputFocus(dpy, byref(win), byref(revert))
    cur = win.value
    for _ in range(12):
        if cur in (0, 1):
            break
        hint = _window_class(dpy, cur)
        if hint and (hint[0] or hint[1]):
            return hint
        root, parent = c_ulong(0), c_ulong(0)
        kids, n = POINTER(c_ulong)(), c_uint(0)
        if not x11.XQueryTree(dpy, cur, byref(root), byref(parent), byref(kids), byref(n)):
            break
        if kids:
            x11.XFree(kids)
        if not parent.value or parent.value == cur:
            break
        cur = parent.value
    return None


def _tap(dpy, keysyms):
    """Aperta as teclas na ordem e solta em ordem inversa (XTest)."""
    codes = [x11.XKeysymToKeycode(dpy, ks) for ks in keysyms]
    if not all(codes):
        return False
    for c in codes:
        xtst.XTestFakeKeyEvent(dpy, c, 1, 0)
    for c in reversed(codes):
        xtst.XTestFakeKeyEvent(dpy, c, 0, 0)
    x11.XFlush(dpy)
    return True


TERMINAL_HINTS = ("terminal", "xterm", "rxvt", "konsole", "kitty", "alacritty", "tilix", "terminator")


def send_paste(dpy=None):
    """Cola na janela focada: Ctrl+V (Ctrl+Shift+V em terminais). True se enviou."""
    if x11 is None or xtst is None:
        return False
    own = dpy is None
    dpy = dpy or open_display()
    if not dpy:
        return False
    try:
        hint = focused_window_class(dpy)
        is_term = bool(hint) and any(t in (hint[0] + " " + hint[1]).lower() for t in TERMINAL_HINTS)
        keys = [KEYSYMS["Control_L"]] + ([KEYSYMS["Shift_L"]] if is_term else []) + [KEYSYMS["v"]]
        return _tap(dpy, keys)
    finally:
        if own:
            x11.XCloseDisplay(dpy)
