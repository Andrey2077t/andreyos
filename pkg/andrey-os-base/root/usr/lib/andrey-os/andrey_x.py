# -*- coding: utf-8 -*-
"""Pequeno acesso ao X11 via ctypes (sem depender de python-xlib).

Usado por andrey-superkey (XRecord) e andrey-clipboard (colar com XTest).
Tudo aqui é opcional: se as bibliotecas ou o X não existirem, as funções
devolvem False/None em vez de levantar exceção.
"""
import ctypes
import time
from ctypes import (CFUNCTYPE, POINTER, Structure, byref, c_int, c_char_p, c_ubyte,
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


# ---------------------------------------------------------------------------
# Tecla Super e teclas "engolidas"
# ---------------------------------------------------------------------------
# Se a tecla Super nao estiver ligada a nenhum modificador real (Mod4...), o MATE (Marco e
# mate-settings-daemon) transforma o atalho "<Super>e" em apenas "e" e captura a letra E em
# TODOS os programas. Por isso:
#   - super_is_modifier()  confere o mapa de modificadores do X (sem depender do GTK)
#   - fix_super_modifier() liga Super_L/Super_R ao Mod4 (o mesmo que "xmodmap -e 'add mod4 = Super_L Super_R'")
#   - probe_grabbed_keys() tenta capturar cada tecla de digitacao sozinha: se o X recusar (BadAccess),
#     e porque outro programa ja a capturou, ou seja, a tecla esta sendo engolida.
MOD_INDEX = {"shift": 0, "lock": 1, "control": 2, "mod1": 3, "mod2": 4, "mod3": 5, "mod4": 6, "mod5": 7}
BAD_ACCESS = 10


class _ModMap(Structure):
    _fields_ = [("max_keypermod", c_int), ("modifiermap", POINTER(c_ubyte))]


class _XErrorEvent(Structure):
    _fields_ = [("type", c_int), ("display", c_void_p), ("resourceid", c_ulong), ("serial", c_ulong),
                ("error_code", c_ubyte), ("request_code", c_ubyte), ("minor_code", c_ubyte)]


_ERRH = CFUNCTYPE(c_int, c_void_p, POINTER(_XErrorEvent))

if x11 is not None:
    x11.XGetModifierMapping.restype = POINTER(_ModMap)
    x11.XGetModifierMapping.argtypes = [c_void_p]
    x11.XFreeModifiermap.argtypes = [POINTER(_ModMap)]
    x11.XInsertModifiermapEntry.restype = POINTER(_ModMap)
    x11.XInsertModifiermapEntry.argtypes = [POINTER(_ModMap), c_ubyte, c_int]
    x11.XSetModifierMapping.restype = c_int
    x11.XSetModifierMapping.argtypes = [c_void_p, POINTER(_ModMap)]
    x11.XDefaultRootWindow.restype = c_ulong
    x11.XDefaultRootWindow.argtypes = [c_void_p]
    x11.XDisplayKeycodes.argtypes = [c_void_p, POINTER(c_int), POINTER(c_int)]
    x11.XKeycodeToKeysym.restype = c_ulong
    x11.XKeycodeToKeysym.argtypes = [c_void_p, c_ubyte, c_int]
    x11.XKeysymToString.restype = c_char_p
    x11.XKeysymToString.argtypes = [c_ulong]
    x11.XGrabKey.argtypes = [c_void_p, c_int, c_uint, c_ulong, c_int, c_int, c_int]
    x11.XUngrabKey.argtypes = [c_void_p, c_int, c_uint, c_ulong]
    x11.XSetErrorHandler.restype = c_void_p


def _super_codes(dpy):
    """Todas as teclas fisicas que produzem Super_L/Super_R (nas 4 primeiras colunas do mapa de teclas)."""
    codes = {x11.XKeysymToKeycode(dpy, KEYSYMS["Super_L"]), x11.XKeysymToKeycode(dpy, KEYSYMS["Super_R"])}
    lo, hi = c_int(0), c_int(0)
    x11.XDisplayKeycodes(dpy, byref(lo), byref(hi))
    for code in range(lo.value, hi.value + 1):
        for col in range(4):
            if x11.XKeycodeToKeysym(dpy, code, col) in (KEYSYMS["Super_L"], KEYSYMS["Super_R"]):
                codes.add(code)
                break
    return codes - {0}


def _modifier_rows(mm):
    """{indice do modificador: {keycodes}} a partir do XModifierKeymap."""
    per = mm.contents.max_keypermod
    return {i: {mm.contents.modifiermap[i * per + j] for j in range(per)} - {0} for i in range(8)}


def super_is_modifier():
    """True se Super_L/Super_R estao ligadas a um modificador real (Mod1..Mod5);
    False se nao estao (ou nao existem); None se nao deu para checar (sem X/bibliotecas)."""
    if x11 is None:
        return None
    dpy = open_display()
    if not dpy:
        return None
    try:
        codes = _super_codes(dpy)
        if not codes:
            return False
        mm = x11.XGetModifierMapping(dpy)
        if not mm:
            return None
        try:
            rows = _modifier_rows(mm)
        finally:
            x11.XFreeModifiermap(mm)
        return any(rows[i] & codes for i in range(3, 8))
    finally:
        x11.XCloseDisplay(dpy)


def fix_super_modifier():
    """Liga Super_L/Super_R ao Mod4. Devolve True se, no fim, a Super e um modificador real."""
    if x11 is None:
        return False
    dpy = open_display()
    if not dpy:
        return False
    try:
        codes = _super_codes(dpy)
        if not codes:
            return False
        for _ in range(6):                      # MappingBusy: alguma tecla estava apertada
            mm = x11.XGetModifierMapping(dpy)
            if not mm:
                return False
            rows = _modifier_rows(mm)
            if any(rows[i] & codes for i in range(3, 8)):
                x11.XFreeModifiermap(mm)
                return True
            main = {x11.XKeysymToKeycode(dpy, KEYSYMS["Super_L"]), x11.XKeysymToKeycode(dpy, KEYSYMS["Super_R"])} - {0}
            for c in sorted(main or codes):
                mm = x11.XInsertModifiermapEntry(mm, c, MOD_INDEX["mod4"])
            status = x11.XSetModifierMapping(dpy, mm)
            x11.XFreeModifiermap(mm)
            x11.XSync(dpy, 0)
            if status == 0:
                break
            time.sleep(0.25)
    finally:
        x11.XCloseDisplay(dpy)
    return super_is_modifier() is True


def _typing_keysym(ks):
    return (0x20 <= ks <= 0x7E or 0xA0 <= ks <= 0xFF or 0xFE50 <= ks <= 0xFE5F
            or ks in (0xFF08, 0xFF09, 0xFF0D, 0xFF1B, 0xFFFF, 0xFF50, 0xFF51, 0xFF52, 0xFF53,
                      0xFF54, 0xFF55, 0xFF56, 0xFF57, 0xFF63))


def probe_grabbed_keys(modifiers=0):
    """Teclas de digitacao (letras, numeros, setas...) que outro programa ja captura SEM modificador.
    Devolve [(keycode, nome)]. Lista vazia = nada esta engolindo teclas. None = nao deu para checar."""
    if x11 is None:
        return None
    dpy = open_display()
    if not dpy:
        return None
    hit = {"flag": False}

    def on_error(_dpy, ev):
        if ev.contents.error_code == BAD_ACCESS:
            hit["flag"] = True
        return 0

    handler = _ERRH(on_error)                    # precisa continuar referenciado
    old = x11.XSetErrorHandler(handler)
    bad, seen = [], set()
    try:
        root = x11.XDefaultRootWindow(dpy)
        lo, hi = c_int(0), c_int(0)
        x11.XDisplayKeycodes(dpy, byref(lo), byref(hi))
        for code in range(lo.value, hi.value + 1):
            ks = x11.XKeycodeToKeysym(dpy, code, 0)
            if not ks or not _typing_keysym(ks):
                continue
            hit["flag"] = False
            x11.XGrabKey(dpy, code, modifiers, root, 0, 1, 1)     # GrabModeAsync, GrabModeAsync
            x11.XSync(dpy, 0)
            if hit["flag"]:
                name = x11.XKeysymToString(ks)
                name = name.decode("ascii", "replace") if name else hex(ks)
                if name not in seen:
                    seen.add(name)
                    bad.append((code, name))
            else:
                x11.XUngrabKey(dpy, code, modifiers, root)
                x11.XSync(dpy, 0)
    finally:
        x11.XSetErrorHandler(old)
        x11.XCloseDisplay(dpy)
    return bad
