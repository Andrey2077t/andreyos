# -*- coding: utf-8 -*-
"""Backend do "Desinstalar Programas" do Andrey OS (a janela é o /usr/bin/andrey-uninstall, em Qt).

  andrey_uninstall.py list              JSON com os programas instalados (os que têm atalho no menu)
  andrey_uninstall.py plan PACOTE...    JSON: o que a remoção levaria junto e se é permitida (não remove nada)
  andrey_uninstall.py check PACOTE...   código 0 = pode remover; 3 = recusado (o apt-helper roda isto como root)

Só lê o sistema; quem remove é o apt-helper (via pkexec), que confere de novo com o `check`.
"""
import glob
import gzip
import json
import os
import re
import subprocess
import sys
import time

APPS_DIRS = ["/usr/share/applications"]
DPKG_STATUS = "/var/lib/dpkg/status"
DPKG_INFO = "/var/lib/dpkg/info"
DPKG_LOGS = "/var/log/dpkg.log"
ICON_ROOT = "/usr/share/icons"
PIXMAPS = "/usr/share/pixmaps"
PKG_RE = re.compile(r"^[a-z0-9][a-z0-9+.-]*$")

# Nunca aparecem na lista e nunca são removidos (nem como "efeito colateral" de outra remoção)
PROTECTED_EXACT = {
    "andrey-os-base", "mate-panel", "mate-menu", "marco", "caja", "mate-terminal", "mate-control-center",
    "mate-system-monitor", "mate-polkit", "mate-notification-daemon", "mate-power-manager",
    "lightdm", "lightdm-gtk-greeter", "xorg", "xserver-xorg", "dbus", "dbus-x11", "systemd", "systemd-sysv",
    "sudo", "pkexec", "polkitd", "policykit-1", "apt", "dpkg", "bash", "coreutils", "network-manager",
    "network-manager-gnome", "pulseaudio", "grub-pc", "grub-efi-amd64", "live-boot", "calamares",
}
PROTECTED_PREFIX = ("mate-desktop-environment", "linux-image", "linux-headers", "libc6", "grub-", "xserver-xorg",
                    "python3", "libgtk", "libglib", "systemd", "network-manager", "lightdm", "mate-session",
                    "mate-settings", "mate-common", "libmate")


# --------------------------------------------------------------------------- dpkg
def parse_status(path=DPKG_STATUS):
    """{pacote: {campo: valor}} só dos instalados. 'Description' vem com as linhas de continuação."""
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            data = fh.read()
    except OSError:
        return out
    for block in data.split("\n\n"):
        f, last = {}, None
        for ln in block.splitlines():
            if ln[:1] in (" ", "\t"):
                if last:
                    f[last] += "\n" + ln[1:]
            elif ":" in ln:
                k, _, v = ln.partition(":")
                last = k.strip()
                f[last] = v.strip()
        name = f.get("Package")
        if name and f.get("Status", "").endswith("installed") and " ok " in f.get("Status", ""):
            out[name] = f
    return out


def split_desc(desc):
    """('resumo', 'descrição longa em uma linha') a partir do campo Description do dpkg."""
    lines = (desc or "").split("\n")
    summary = lines[0].strip()
    paras, cur = [], []
    for ln in lines[1:]:
        if ln.strip() == ".":
            if cur:
                paras.append(" ".join(cur))
                cur = []
        elif ln.strip():
            cur.append(ln.strip())
    if cur:
        paras.append(" ".join(cur))
    return summary, (paras[0] if paras else "")


def shorten(s, n=110):
    s = " ".join((s or "").split())
    if len(s) <= n:
        return s
    cut = s[:n].rsplit(" ", 1)[0]
    return cut.rstrip(",.;:") + "…"


def fmt_size(kb):
    """Tamanho como o desenho da janela mostra: '372 MB', '1,1 GB'."""
    try:
        kb = int(kb)
    except (TypeError, ValueError):
        return "—"
    mb = kb / 1024.0
    if mb < 1:
        return "%d KB" % max(kb, 1)
    if mb < 1000:
        return "%d MB" % round(mb)
    return ("%.1f GB" % (mb / 1024.0)).replace(".", ",")


def is_protected(name, fields=None):
    if name in PROTECTED_EXACT or name.startswith(PROTECTED_PREFIX):
        return True
    f = fields or {}
    return f.get("Essential") == "yes" or f.get("Priority") == "required"


# --------------------------------------------------------------------------- .desktop
def parse_desktop(path):
    """Campos úteis da seção [Desktop Entry]; None se não for um aplicativo que apareça no menu."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return None
    d, inside = {}, False
    for ln in lines:
        s = ln.strip()
        if s.startswith("["):
            inside = (s == "[Desktop Entry]")
            continue
        if inside and "=" in s and not s.startswith("#"):
            k, _, v = s.partition("=")
            d.setdefault(k.strip(), v.strip())
    if d.get("Type", "Application") != "Application":
        return None
    if d.get("NoDisplay", "").lower() == "true" or d.get("Hidden", "").lower() == "true":
        return None
    only, notin = d.get("OnlyShowIn", ""), d.get("NotShowIn", "")
    if only and "MATE" not in only.split(";"):
        return None
    if "MATE" in notin.split(";"):
        return None
    if not d.get("Name"):
        return None

    def loc(key):  # prefere português
        return d.get(key + "[pt_BR]") or d.get(key + "[pt]") or d.get(key, "")

    return {"name": loc("Name"), "comment": loc("Comment") or loc("GenericName"),
            "icon": d.get("Icon", ""), "categories": [c for c in d.get("Categories", "").split(";") if c]}


def category_of(cats, pkg=""):
    c = set(cats)
    if pkg in ("wine", "wine32", "wine64"):
        return "Outros"
    if "Game" in c:
        return "Jogos"
    if "Office" in c:
        return "Escritório"
    if c & {"Network", "WebBrowser", "Email", "InstantMessaging", "Chat", "FileTransfer", "P2P"}:
        return "Internet"
    if c & {"AudioVideo", "Audio", "Video", "Graphics", "Photography", "Player", "Recorder", "2DGraphics",
            "3DGraphics", "RasterGraphics", "VectorGraphics", "Music"}:
        return "Multimídia"
    if c & {"Utility", "Development", "TextEditor", "Archiving", "Compression", "FileTools", "Calculator",
            "Science", "Education"}:
        return "Utilitários"
    return "Outros"


def desktop_owners(info_dir=DPKG_INFO, apps_dirs=APPS_DIRS):
    """{caminho do .desktop: pacote}, lendo os arquivos *.list do dpkg (bem mais rápido que dpkg -S por arquivo)."""
    wanted = tuple(d.rstrip("/") + "/" for d in apps_dirs)
    out = {}
    for lst in glob.glob(os.path.join(info_dir, "*.list")):
        pkg = os.path.basename(lst)[:-5].split(":")[0]
        try:
            with open(lst, encoding="utf-8", errors="replace") as fh:
                for ln in fh:
                    ln = ln.rstrip("\n")
                    if ln.endswith(".desktop") and ln.startswith(wanted):
                        out.setdefault(ln, pkg)
        except OSError:
            continue
    return out


# --------------------------------------------------------------------------- ícones
def resolve_icon(name, icon_root=ICON_ROOT, pixmaps=PIXMAPS):
    """Caminho do arquivo do ícone (svg de preferência, senão o png maior até 256px), ou ''."""
    if not name:
        return ""
    if os.path.isabs(name):
        return name if os.path.isfile(name) else ""
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
        return ""
    themes = sorted(glob.glob(os.path.join(icon_root, "*")), key=lambda p: (os.path.basename(p) != "hicolor", p))
    for ext in ("svg", "png"):
        for th in themes:
            hits = glob.glob(os.path.join(th, "scalable", "apps", name + ".svg")) if ext == "svg" else []
            if ext == "png":
                best, best_sz = "", 0
                for p in glob.glob(os.path.join(th, "*x*", "apps", name + ".png")):
                    m = re.match(r"(\d+)x\d+$", os.path.basename(os.path.dirname(os.path.dirname(p))))
                    sz = int(m.group(1)) if m else 0
                    if 48 <= sz <= 256 and sz > best_sz:
                        best, best_sz = p, sz
                hits = [best] if best else []
            if hits:
                return hits[0]
    for ext in ("svg", "png", "xpm"):
        p = os.path.join(pixmaps, name + "." + ext)
        if os.path.isfile(p):
            return p
    return ""


# --------------------------------------------------------------------------- data de instalação
def install_dates(log=DPKG_LOGS):
    """{pacote: timestamp da primeira instalação registrada no dpkg.log (e nos rotacionados)}."""
    out = {}
    files = sorted(glob.glob(log + "*"), key=lambda p: (p != log, p), reverse=True)  # mais antigo primeiro
    for fp in files:
        try:
            op = gzip.open if fp.endswith(".gz") else open
            with op(fp, "rt", encoding="utf-8", errors="replace") as fh:
                for ln in fh:
                    p = ln.split()
                    if len(p) >= 4 and p[2] == "install":
                        try:
                            ts = time.mktime(time.strptime(p[0] + " " + p[1], "%Y-%m-%d %H:%M:%S"))
                        except ValueError:
                            continue
                        out.setdefault(p[3].split(":")[0], ts)
        except OSError:
            continue
    return out


def fmt_date(ts):
    return time.strftime("%d/%m/%Y", time.localtime(ts)) if ts else "—"


# --------------------------------------------------------------------------- lista
def list_apps(status=None, owners=None, dates=None, apps_dirs=APPS_DIRS, info_dir=DPKG_INFO,
              icon_root=ICON_ROOT, pixmaps=PIXMAPS):
    status = status if status is not None else parse_status()
    owners = owners if owners is not None else desktop_owners(info_dir, apps_dirs)
    dates = dates if dates is not None else install_dates()
    per_pkg = {}
    for path in sorted(owners):
        pkg = owners[path]
        f = status.get(pkg)
        if not f or is_protected(pkg, f):
            continue
        e = parse_desktop(path)
        if not e:
            continue
        cur = per_pkg.get(pkg)
        # um pacote pode ter vários atalhos: fica com o de nome mais curto (o "principal")
        if cur is None or len(e["name"]) < len(cur["name"]):
            per_pkg[pkg] = e
    apps = []
    for pkg, e in per_pkg.items():
        f = status[pkg]
        summary, long_ = split_desc(f.get("Description", ""))
        ts = dates.get(pkg)
        if not ts:
            try:
                ts = os.stat(os.path.join(info_dir, pkg + ".list")).st_mtime
            except OSError:
                ts = None
        try:
            kb = int(f.get("Installed-Size", "0"))
        except ValueError:
            kb = 0
        apps.append({"pkg": pkg, "name": e["name"], "desc": shorten(e["comment"] or summary, 60),
                     "long": shorten(long_ or e["comment"] or summary), "version": f.get("Version", "—"),
                     "size_kb": kb, "size": fmt_size(kb), "date": fmt_date(ts),
                     "category": category_of(e["categories"], pkg), "icon": e["icon"],
                     "iconpath": resolve_icon(e["icon"], icon_root, pixmaps)})
    apps.sort(key=lambda a: a["name"].lower())
    return apps


# --------------------------------------------------------------------------- plano de remoção
REMV_RE = re.compile(r"^Remv (\S+)")


def simulate_remove(pkgs):
    """Nomes dos pacotes que `apt-get remove` tiraria (simulação, não altera nada). None se o apt falhar."""
    try:
        r = subprocess.run(["apt-get", "-s", "-o", "Debug::NoLocking=1", "remove", "--", *pkgs],
                           capture_output=True, text=True, timeout=120, env=dict(os.environ, LC_ALL="C"))
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    return [m.group(1).split(":")[0] for m in (REMV_RE.match(ln) for ln in r.stdout.splitlines()) if m]


def plan(pkgs, status=None, simulate=None):
    """Decide se a remoção é permitida. Devolve:
    {ok, remove:[todos que saem], extra:[os que saem além dos pedidos], blocked:[protegidos atingidos], error}"""
    res = {"ok": False, "remove": [], "extra": [], "blocked": [], "error": ""}
    simulate = simulate or simulate_remove
    if not pkgs or any(not PKG_RE.match(p) for p in pkgs):
        res["error"] = "nome de pacote inválido"
        return res
    status = status if status is not None else parse_status()
    # o que já não está instalado não tem o que remover (a Loja manda grupos como "libreoffice + idioma")
    pkgs = [p for p in pkgs if p in status]
    if not pkgs:
        res["ok"] = True
        return res
    out = simulate(list(pkgs))
    if out is None:
        res["error"] = "o apt não conseguiu simular a remoção"
        return res
    res["remove"] = sorted(set(out) | set(pkgs))
    res["extra"] = [p for p in res["remove"] if p not in pkgs]
    res["blocked"] = [p for p in res["remove"] if is_protected(p, status.get(p))]
    res["ok"] = not res["blocked"]
    if res["blocked"]:
        res["error"] = "faz parte do sistema: " + ", ".join(res["blocked"])
    return res


def main(argv):
    cmd = argv[0] if argv else ""
    if cmd == "list":
        json.dump({"apps": list_apps()}, sys.stdout, ensure_ascii=False)
        return 0
    if cmd in ("plan", "check"):
        r = plan(argv[1:])
        if cmd == "plan":
            json.dump(r, sys.stdout, ensure_ascii=False)
            return 0
        if not r["ok"]:
            print("Recusado: " + r["error"], file=sys.stderr)
            return 3
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
