# -*- coding: utf-8 -*-
"""Lógica pura (sem GTK) da busca do Menu Iniciar: calculadora, pontuação e índice de arquivos.

Fica separada do andrey-start para poder ser testada sem interface (veja tests/).
"""
import ast
import operator
import os
import re

# ---------------------------------------------------------------------------
# Calculadora
# ---------------------------------------------------------------------------
_BINOPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
           ast.Div: operator.truediv, ast.FloorDiv: operator.floordiv,
           ast.Mod: operator.mod, ast.Pow: operator.pow}
_UNOPS = {ast.UAdd: operator.pos, ast.USub: operator.neg}

MAX_POW_EXP = 1000        # expoente máximo aceito
MAX_POW_BASE = 1e6        # base máxima aceita numa potência
MAX_RESULT_DIGITS = 60    # resultados maiores que isso não cabem na tela (e estouram str())


def _eval(node):
    if isinstance(node, ast.Expression):
        return _eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)) \
            and not isinstance(node.value, bool):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
        a, b = _eval(node.left), _eval(node.right)
        if isinstance(node.op, ast.Pow) and (abs(b) > MAX_POW_EXP or abs(a) > MAX_POW_BASE):
            raise ValueError("potência grande demais")
        r = _BINOPS[type(node.op)](a, b)
        if isinstance(r, complex):          # ex.: (-8)^0,5 -> Python devolve complexo; não dá para mostrar
            raise ValueError("resultado complexo")
        return r
    if isinstance(node, ast.UnaryOp) and type(node.op) in _UNOPS:
        return _UNOPS[type(node.op)](_eval(node.operand))
    raise ValueError("expressão inválida")


def _format(v):
    """Número -> texto no formato brasileiro, ou None se não der para mostrar."""
    if isinstance(v, float):
        if v != v or v in (float("inf"), float("-inf")):
            return None
        if v == int(v) and abs(v) < 1e15:
            return str(int(v))
        return ("%.10g" % v).replace(".", ",")
    if abs(v).bit_length() > MAX_RESULT_DIGITS * 3.33:
        return None
    return str(v)


def calc(text):
    """Resolve contas simples ('2+2', '3 x 4', '2^10', '1,5*2'). Devolve texto ou None."""
    t = text.strip().replace(",", ".").replace("×", "*").replace("÷", "/").replace("^", "**")
    t = re.sub(r"(?<=\d)\s*x\s*(?=\d)", "*", t)
    if not re.fullmatch(r"[0-9+\-*/().%\s]+", t):
        return None
    if not re.search(r"\d", t) or not re.search(r"[+\-*/%]", t.lstrip("+-")):
        return None
    try:
        return _format(_eval(ast.parse(t, mode="eval")))
    except (ValueError, SyntaxError, ZeroDivisionError, OverflowError, RecursionError, MemoryError):
        return None


# ---------------------------------------------------------------------------
# Pontuação de aplicativos
# ---------------------------------------------------------------------------
def norm(s):
    return (s or "").casefold()


def score_app(entry, q):
    """Pontuação de uma busca por nome (0 = não bate). 'q' já em minúsculas (norm)."""
    name = entry["lname"]
    if name == q:
        return 100
    if name.startswith(q):
        return 90
    if any(w.startswith(q) for w in re.split(r"[\s\-_.]+", name)):
        return 75
    if q in name:
        return 60
    if q in entry["lextra"]:
        return 30
    return 0


# ---------------------------------------------------------------------------
# Arquivos da pasta pessoal
# ---------------------------------------------------------------------------
SKIP_DIRS = {"node_modules", "__pycache__", "venv", ".venv", "site-packages", "snap", "Trash"}


def index_home(limit=60000, max_depth=6, home=None):
    """Lista (nome, caminho, é_pasta) de arquivos e pastas da pasta pessoal, sem ocultos."""
    home = home or os.path.expanduser("~")
    out = []
    stack = [(home, 0)]
    while stack and len(out) < limit:
        path, depth = stack.pop()
        try:
            it = os.scandir(path)
        except OSError:
            continue
        with it:
            for e in it:
                if e.name.startswith("."):
                    continue
                try:
                    is_dir = e.is_dir(follow_symlinks=False)
                except OSError:
                    continue
                out.append((e.name, e.path, is_dir))
                if is_dir and depth < max_depth and e.name not in SKIP_DIRS:
                    stack.append((e.path, depth + 1))
    return out


def match_files(index, ql, max_hits=5):
    """Melhores arquivos/pastas do índice para a busca 'ql' (minúsculas)."""
    if not index or len(ql) < 2:
        return []
    hits = [(0 if n.casefold().startswith(ql) else 1, len(n), n, p, d)
            for n, p, d in index if ql in n.casefold()]
    hits.sort()
    return [(n, p, d) for _, _, n, p, d in hits[:max_hits]]
