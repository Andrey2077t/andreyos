# -*- coding: utf-8 -*-
"""Lógica do "Executar .exe" do Andrey OS (umu-launcher + Proton-GE). Sem interface: o /usr/bin/andrey-exe usa isto.

Duplo clique num .exe/.msi/.bat -> detecta se é jogo, programa ou instalador -> monta o comando do umu-run
com o Proton-GE e um prefixo (pasta do "Windows" daquele programa) separado. Tudo roda como o próprio usuário.
"""
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unicodedata

import andrey_games as G

SYSTEM_PROTON_DIR = "/usr/share/andrey-os/proton"   # Proton-GE que já vem na ISO (hook 0155-umu)
RUN_EXTS = (".exe", ".msi", ".bat")
KINDS = ("game", "app", "installer")
KIND_LABEL = {"game": "jogo", "app": "programa", "installer": "instalador"}

# --------------------------------------------------------------------------- caminhos e configuração
def _home(home=None):
    return home or os.path.expanduser("~")


def data_home(home=None):
    if home:
        return os.path.join(home, ".local", "share")
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def state_dir(home=None):
    return os.path.join(data_home(home), "andrey-os", "exe")


def prefixes_dir(home=None):
    return os.path.join(state_dir(home), "prefixes")


def log_path(home=None):
    base = os.path.join(home, ".cache") if home else (os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"))
    return os.path.join(base, "andrey-os", "exe.log")


def conf_path(home=None):
    base = os.path.join(home, ".config") if home else (os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"))
    return os.path.join(base, "andrey-os", "exe.conf")


DEFAULT_CONF = {"ask": "first",       # first = pergunta só na 1ª vez de cada arquivo | always | never
                "proton": "",         # vazio = o Proton-GE mais novo que houver; ou um caminho/versão (GE-Proton10-4)
                "gamemode": "off"}    # on = roda jogos com gamemoderun


def load_conf(home=None):
    """~/.config/andrey-os/exe.conf: linhas 'chave=valor' ('#' comenta). Valor inválido volta ao padrão."""
    conf = dict(DEFAULT_CONF)
    try:
        with open(conf_path(home), encoding="utf-8") as fh:
            for ln in fh:
                ln = ln.strip()
                if not ln or ln.startswith("#") or "=" not in ln:
                    continue
                k, v = (s.strip() for s in ln.split("=", 1))
                if k in conf:
                    conf[k] = v
    except OSError:
        pass
    if conf["ask"] not in ("first", "always", "never"):
        conf["ask"] = DEFAULT_CONF["ask"]
    if conf["gamemode"] not in ("on", "off"):
        conf["gamemode"] = DEFAULT_CONF["gamemode"]
    return conf


def slugify(text, fallback="app"):
    t = unicodedata.normalize("NFKD", text or "").encode("ascii", "ignore").decode("ascii").lower()
    t = re.sub(r"[^a-z0-9]+", "-", t).strip("-")[:40].strip("-")
    return t or fallback


def short_hash(text):
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:6]

def is_rufus(path):
    """Reconhece o executável portátil do Rufus pelo padrão de nome dos downloads."""
    name = os.path.basename(path)
    return bool(re.fullmatch(r"rufus(?:[-_ ][^/\\]*)?\.exe", name, re.I))




# --------------------------------------------------------------------------- leitura do cabeçalho PE
def read_pe(path):
    """Cabeçalho PE mínimo: {'bits','gui','console','dll','dotnet','imports'} ou None se não for um PE válido."""
    try:
        with open(path, "rb") as f:
            dos = f.read(64)
            if len(dos) < 64 or dos[:2] != b"MZ":
                return None
            off = struct.unpack_from("<I", dos, 0x3C)[0]
            if off < 64 or off > (1 << 24):
                return None
            f.seek(off)
            hdr = f.read(24 + 240)
            if len(hdr) < 24 + 96 or hdr[:4] != b"PE\0\0":
                return None
            machine, nsec = struct.unpack_from("<HH", hdr, 4)
            opt_size, chars = struct.unpack_from("<HH", hdr, 20)
            magic = struct.unpack_from("<H", hdr, 24)[0]
            if magic == 0x10B:
                dd_off = 24 + 96
            elif magic == 0x20B:
                dd_off = 24 + 112
            else:
                return None
            subsystem = struct.unpack_from("<H", hdr, 24 + 68)[0]

            def ddir(i):
                o = dd_off + 8 * i
                if o + 8 > len(hdr) or o + 8 > 24 + opt_size:
                    return 0, 0
                return struct.unpack_from("<II", hdr, o)

            info = {"bits": 64 if machine in (0x8664, 0xAA64) else (32 if machine == 0x14C else 0),
                    "gui": subsystem == 2, "console": subsystem == 3, "dll": bool(chars & 0x2000),
                    "dotnet": ddir(14)[0] != 0, "imports": set()}
            f.seek(off + 24 + opt_size)
            raw = f.read(40 * min(nsec, 96))
            sections = []
            for i in range(len(raw) // 40):
                vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", raw, i * 40 + 8)
                sections.append((vaddr, max(vsize, rsize), rptr))

            def rva2off(rva):
                for va, size, rp in sections:
                    if va <= rva < va + size:
                        return rp + rva - va
                return None

            irva, _ = ddir(1)
            o = rva2off(irva) if irva else None
            if o is not None:
                f.seek(o)
                for _ in range(128):
                    d = f.read(20)
                    if len(d) < 20:
                        break
                    name_rva = struct.unpack_from("<I", d, 12)[0]
                    if name_rva == 0:
                        break
                    no = rva2off(name_rva)
                    if no is None:
                        continue
                    pos = f.tell()
                    f.seek(no)
                    nm = f.read(64).split(b"\0", 1)[0]
                    f.seek(pos)
                    info["imports"].add(nm.decode("ascii", "ignore").lower())
            return info
    except (OSError, struct.error):
        return None


INSTALLER_MARKS = ((b"Inno Setup Setup Data", "Inno Setup"), (b"Nullsoft Install System", "NSIS"),
                   (b"InstallShield", "InstallShield"), (b"WixBundle", "WiX"), (b"WiX Burn", "WiX"),
                   (b"Wise Installation Wizard", "Wise"), (b"Advanced Installer", "Advanced Installer"))


def installer_marker(path, limit=4 << 20):
    """Nome do gerador de instalador (Inno Setup, NSIS...) se o arquivo tiver a assinatura dele; '' se não."""
    try:
        with open(path, "rb") as f:
            blob = f.read(limit)
    except OSError:
        return ""
    for needle, label in INSTALLER_MARKS:
        if needle in blob:
            return label
    return ""


# --------------------------------------------------------------------------- detecção: jogo, programa ou instalador
GFX_STRONG = {"d3d8.dll", "d3d9.dll", "d3d10.dll", "d3d10_1.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "ddraw.dll",
              "vulkan-1.dll", "d3dx9_43.dll", "d3dx10_43.dll", "d3dx11_43.dll"}
GAME_MEDIUM = {"xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "dinput8.dll", "dinput.dll", "openal32.dll",
               "xaudio2_7.dll", "xaudio2_8.dll", "xaudio2_9.dll", "x3daudio1_7.dll", "steam_api.dll",
               "steam_api64.dll", "fmod.dll", "fmodex.dll", "fmod64.dll", "bink2w64.dll", "binkw32.dll"}
GAME_DATA_EXTS = (".pak", ".wad", ".bsa", ".ba2", ".vpk", ".rpa", ".ucas", ".utoc", ".bank", ".xp3", ".bsp", ".esm", ".esp",
                  ".forge", ".gcf", ".sar")
ELECTRON_MARKS = {"chrome_100_percent.pak", "libcef.dll", "icudtl.dat", "resources.pak"}
GAME_DIRS = {"steamapps": 4, "gog games": 3, "epic games": 3, "games": 2, "jogos": 2}
GENERIC_DIRS = {"bin", "bin32", "bin64", "binaries", "win32", "win64", "x64", "x86", "game", "release", "shipping",
                "windows", "windowsnoeditor", "downloads", "desktop", "documents", "tmp", "temp", "program files",
                "program files (x86)", "drive_c", "common", "app", "application"}
REDIST_RE = re.compile(r"(vc_?redist|vcredist|dxsetup|dxwebsetup|directx|dotnet|ndp\d|windowsdesktop-runtime|oalinst|"
                       r"physx|xnafx|netfx)")
INSTALLER_NAME_RE = re.compile(r"(^|[^a-z0-9])(setup|install|installer|instalador|instalar)([^a-z]|$)|(setup|installer)$")
SETUP_WORDS_RE = re.compile(r"(^|[^a-z0-9])(setup|install|installer|instalador|instalar|x64|x86|win64|win32)(?=[^a-z]|$)")


def steam_appid(directory):
    try:
        with open(os.path.join(directory, "steam_appid.txt"), encoding="ascii", errors="ignore") as fh:
            v = fh.readline().strip()
    except OSError:
        return None
    return v if re.fullmatch(r"[0-9]{1,10}", v) else None


def classify(path):
    """Decide o que é o arquivo. Devolve {'kind': game|app|installer, 'reasons': [...], 'bits', 'appid', 'redist', 'dotnet'}."""
    p = os.path.abspath(path)
    stem, ext = os.path.splitext(os.path.basename(p))
    ext, low, d = ext.lower(), stem.lower(), os.path.dirname(p)
    out = {"kind": "app", "reasons": [], "bits": 0, "appid": None, "redist": False, "dotnet": False, "rufus": is_rufus(p)}
    if ext == ".msi":
        out.update(kind="installer", reasons=["pacote .msi"])
        return out
    if ext == ".bat":
        out["reasons"] = ["script .bat"]
        return out
    pe = read_pe(p) if ext == ".exe" else None
    if pe:
        out["bits"], out["dotnet"] = pe["bits"], pe["dotnet"]
    if out["rufus"]:
        out["reasons"] = ["Rufus; executável Windows portátil"]
        return out

    if REDIST_RE.search(low):
        out.update(kind="installer", redist=True, reasons=["componente do Windows (runtime/redistribuível)"])
        return out
    mark = installer_marker(p) if pe else ""
    if mark:
        out.update(kind="installer", reasons=["instalador " + mark])
        return out

    try:
        names = os.listdir(d)[:800]
    except OSError:
        names = []
    ln = {n.lower() for n in names}
    score, why = 0, []

    def add(points, reason):
        nonlocal score
        score += points
        why.append(reason)

    if ln & {"steam_api.dll", "steam_api64.dll", "steam_appid.txt", "steamclient.dll", "steamclient64.dll"}:
        add(4, "usa a Steam")
    if any(n.startswith("goggame-") for n in ln) or any(n.startswith("galaxy") and n.endswith(".dll") for n in ln):
        add(4, "jogo da GOG")
    if ln & {"uplay_r1_loader.dll", "upc_r2_loader.dll"}:
        add(4, "jogo da Ubisoft")
    if any(n.startswith("eossdk") for n in ln):
        add(3, "usa Epic Online Services")
    if "unityplayer.dll" in ln or any(
            any(os.path.exists(os.path.join(d, n, m)) for m in ("globalgamemanagers", "data.unity3d", "level0"))
            for n in names if n.lower().endswith("_data")):
        add(5, "motor Unity")
    if low.endswith(("-win64-shipping", "-win32-shipping", "-shipping")) or re.search(r"[\\/]binaries[\\/]win(32|64)$", d, re.I) \
            or ("engine" in ln and os.path.isdir(os.path.join(d, "Engine"))):
        add(5, "motor Unreal")
    if any(n.endswith(".pck") for n in ln):
        add(3, "motor Godot")
    if "renpy" in ln and os.path.isdir(os.path.join(d, "renpy")):
        add(4, "Ren'Py")
    electron = bool(ln & ELECTRON_MARKS)
    if electron:
        score -= 4
        why.append("app Electron/Chromium")
    elif any(n.endswith(GAME_DATA_EXTS) for n in ln):
        add(3, "arquivos de jogo")
    if pe:
        imp = pe["imports"]
        if imp & GFX_STRONG:
            add(3, "usa DirectX/Vulkan")
        if imp & GAME_MEDIUM:
            add(2, "usa entrada/áudio de jogo")
    parts = [x.lower() for x in p.split(os.sep)[:-1]]
    for name, points in GAME_DIRS.items():
        if name in parts:
            add(points, "está numa pasta de jogos")
            break
    out["appid"] = steam_appid(d)
    if score >= 4:
        out.update(kind="game", reasons=why)
        return out
    if INSTALLER_NAME_RE.search(low):
        out.update(kind="installer", reasons=["nome de instalador"])
        return out
    # (sinais fracos de jogo, como estar numa pasta "Games", não entram aqui: confundiriam quem lê "programa")
    out["reasons"] = ["programa de terminal"] if pe and pe["console"] else ["não parece jogo nem instalador"]
    return out


def display_name(path, kind="app"):
    """Nome para mostrar/atalho: jogos usam a pasta do jogo (Steam/GOG), o resto o nome do arquivo."""
    p = os.path.abspath(path)
    if is_rufus(p):
        return "Rufus"
    stem = os.path.splitext(os.path.basename(p))[0]
    stem = re.sub(r"-win(64|32)-shipping$", "", stem, flags=re.I)
    pretty = re.sub(r"[_]+", " ", stem).strip() or "Programa"
    if kind == "game":
        d = os.path.dirname(p)
        home = os.path.expanduser("~")
        for _ in range(4):
            nm = os.path.basename(d)
            if not nm or d in (home, os.sep):
                break
            if nm.lower() not in GENERIC_DIRS:
                return nm
            d = os.path.dirname(d)
    return pretty


# --------------------------------------------------------------------------- prefixo e Proton
def prefix_for(kind, path, home=None, redist=False):
    """Pasta do "Windows" (WINEPREFIX) deste programa. Jogo e instalador têm a deles; programa comum divide uma só."""
    root = prefixes_dir(home)
    p = os.path.abspath(path)
    if is_rufus(p):
        return os.path.join(root, "rufus")
    if kind == "game":
        d = os.path.dirname(p)
        return os.path.join(root, "jogo-%s-%s" % (slugify(display_name(p, "game"), "jogo"), short_hash(d)))
    if kind == "installer" and not redist:
        stem = os.path.splitext(os.path.basename(p))[0].lower()
        name = slugify(SETUP_WORDS_RE.sub(" ", stem), "")
        return os.path.join(root, "app-" + (name or "setup-" + short_hash(p)))
    return os.path.join(root, "programas")


def _ver_key(name):
    return [(0, int(t)) if t.isdigit() else (1, t) for t in re.split(r"(\d+)", name) if t != ""]


def proton_candidates(home=None):
    """Pastas de Proton-GE usáveis: as da pessoa (andrey-games --proton-ge) e a que veio na ISO."""
    found = []
    for base in (G.compat_dir(home), SYSTEM_PROTON_DIR):
        for d in glob.glob(os.path.join(base, "GE-Proton*")):
            if os.path.isfile(os.path.join(d, "proton")):
                found.append(d)
    return found


def find_proton(conf=None, home=None):
    """PROTONPATH para o umu-run: o configurado, senão o GE-Proton mais novo instalado; sem nenhum, 'GE-Proton'
    (o próprio umu-run baixa o mais novo na primeira vez)."""
    want = (conf or {}).get("proton", "").strip()
    if want:
        return os.path.expanduser(want)
    cands = proton_candidates(home)
    if not cands:
        return "GE-Proton"
    return max(cands, key=lambda d: _ver_key(os.path.basename(d)))


# --------------------------------------------------------------------------- comando
def build_run(path, kind, prefix, proton, args=(), umu=None, appid=None, gamemode=False, environ=None):
    """(comando, ambiente, pasta de trabalho). Sem umu-run (umu=None) cai no wine puro, só com o prefixo."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".msi":
        target = ["msiexec", "/i", path]
    elif ext == ".bat":
        target = ["cmd", "/c", path]
    else:
        target = [path]
    env = dict(os.environ if environ is None else environ)
    env["WINEPREFIX"] = prefix
    if umu:
        env["PROTONPATH"] = proton
        if appid and kind == "game":
            env["GAMEID"] = "umu-" + appid
        cmd = [umu] + target + list(args)
        if gamemode and kind == "game" and shutil.which("gamemoderun"):
            cmd = ["gamemoderun"] + cmd
    else:
        cmd = ["wine"] + target + list(args)
    return cmd, env, os.path.dirname(path) or "."


# --------------------------------------------------------------------------- arquivos já conhecidos
def _known_file(home=None):
    return os.path.join(state_dir(home), "known.json")


def _load_known(home=None):
    try:
        with open(_known_file(home), encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def known_entry(path, home=None):
    e = _load_known(home).get(os.path.abspath(path))
    return e if isinstance(e, dict) else None


def mark_known(path, kind, prefix, home=None):
    data = _load_known(home)
    data[os.path.abspath(path)] = {"kind": kind, "prefix": prefix}
    d = state_dir(home)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".known-")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False)
    os.replace(tmp, _known_file(home))


def first_run_marker(home=None):
    return os.path.join(state_dir(home), "umu-ready")


# --------------------------------------------------------------------------- atalhos (.desktop)
def desktop_quote(arg):
    """Um argumento do Exec= de um .desktop: entre aspas, com a dupla camada de escape da especificação."""
    s = arg.replace("%", "%%")
    s = s.replace("\\", "\\\\\\\\").replace('"', '\\\\"').replace("`", "\\\\`").replace("$", "\\\\$")
    return '"' + s + '"'


def desktop_unquote(value):
    """Inverso de desktop_quote para uma linha Exec= (usado nos testes): devolve a lista de argumentos."""
    s = value.replace("\\\\", "\\")           # camada 1: escape de string do arquivo
    out, cur, i, inq, have = [], [], 0, False, False
    while i < len(s):
        c = s[i]
        if inq:
            if c == "\\" and i + 1 < len(s):
                cur.append(s[i + 1])
                i += 1
            elif c == '"':
                inq = False
            else:
                cur.append(c)
        elif c == '"':
            inq, have = True, True
        elif c == " ":
            if have or cur:
                out.append("".join(cur))
            cur, have = [], False
        else:
            cur.append(c)
        i += 1
    if have or cur:
        out.append("".join(cur))
    return [a.replace("%%", "%") for a in out]


def shortcut_exec(kind, prefix, path):
    return ["andrey-exe", "--no-ask", "--kind", kind, "--prefix", prefix, "--", path]


def desktop_entry(name, kind, prefix, path, icon):
    name = " ".join(name.split()) or "Programa"
    lines = ["[Desktop Entry]", "Type=Application", "Version=1.0", "Name=" + name,
             "Comment=Windows (%s) com Proton-GE no Andrey OS" % KIND_LABEL.get(kind, "programa"),
             "Exec=" + " ".join(desktop_quote(a) for a in shortcut_exec(kind, prefix, path)),
             "Path=" + os.path.dirname(path).replace("\n", " "), "Icon=" + icon, "Terminal=false",
             "Categories=Game;" if kind == "game" else "Categories=Utility;", "StartupNotify=true",
             "X-Andrey-Exe=true"]
    return "\n".join(lines) + "\n"


def desktop_dir(home=None):
    h = _home(home)
    out = ""
    if not home:
        try:
            out = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True, timeout=5).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            out = ""
    return out if out and out != h else os.path.join(h, "Desktop")


def _first_icon(root):
    best, best_px = None, -1
    for f in glob.glob(os.path.join(root, "*.png")):
        m = re.search(r"_(\d+)x(\d+)x\d+\.png$", f)
        px = int(m.group(1)) * int(m.group(2)) if m else 0
        if px > best_px:
            best, best_px = f, px
    return best


def extract_icon(exe, slug, home=None):
    """Ícone do .exe (precisa do icoutils) salvo em PNG; devolve o caminho, ou None (o atalho usa um ícone genérico)."""
    if not (shutil.which("wrestool") and shutil.which("icotool")):
        return None
    dest_dir = os.path.join(state_dir(home), "icons")
    dest = os.path.join(dest_dir, slug + ".png")
    work = tempfile.mkdtemp(prefix="andrey-ico-")
    try:
        ico_dir, png_dir = os.path.join(work, "ico"), os.path.join(work, "png")
        os.makedirs(ico_dir)
        os.makedirs(png_dir)
        subprocess.run(["wrestool", "-x", "--type=14", "-o", ico_dir, exe], capture_output=True, timeout=30)
        icos = sorted(glob.glob(os.path.join(ico_dir, "*.ico")))
        if not icos:
            return None
        subprocess.run(["icotool", "-x", "-o", png_dir, icos[0]], capture_output=True, timeout=30)
        best = _first_icon(png_dir)
        if not best:
            return None
        os.makedirs(dest_dir, exist_ok=True)
        shutil.copyfile(best, dest)
        return dest
    except (OSError, subprocess.SubprocessError):
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def write_shortcuts(name, kind, prefix, path, where=("menu",), home=None):
    """Cria o atalho no menu de aplicativos e/ou na área de trabalho. Devolve os arquivos criados."""
    p = os.path.abspath(path)
    slug = "%s-%s" % (slugify(name), short_hash(p))
    icon = extract_icon(p, slug, home) or ("applications-games" if kind == "game" else "application-x-executable")
    text = desktop_entry(name, kind, prefix, p, icon)
    targets = []
    if "menu" in where:
        targets.append(os.path.join(data_home(home), "applications", "andrey-exe-%s.desktop" % slug))
    if "desktop" in where:
        targets.append(os.path.join(desktop_dir(home), "%s.desktop" % (slugify(name) or slug)))
    made = []
    for t in targets:
        os.makedirs(os.path.dirname(t), exist_ok=True)
        with open(t, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.chmod(t, 0o755)
        if "applications" not in t and not home:   # área de trabalho: marca como confiável para o Caja abrir sem perguntar
            for key in ("metadata::trusted", "metadata::caja-trusted-launcher"):
                try:
                    subprocess.run(["gio", "set", t, key, "true"], capture_output=True, timeout=10)
                except (OSError, subprocess.SubprocessError):
                    pass
        made.append(t)
    return made


# --------------------------------------------------------------------------- depois do instalador
SKIP_EXE_RE = re.compile(r"(unins|uninst|crash|report|redist|vcredist|vc_redist|dxsetup|dotnet|setup|helper|updat|"
                         r"installer|dxwebsetup|oalinst|physx|cefsubprocess|diagnos)", re.I)


def prefix_drive_c(prefix):
    for sub in ("drive_c", os.path.join("pfx", "drive_c")):
        d = os.path.join(prefix, sub)
        if os.path.isdir(d):
            return d
    return None


def prefix_exes(prefix, max_files=3000):
    """Executáveis de programas instalados no prefixo (Program Files e pastas novas na raiz do C:)."""
    c = prefix_drive_c(prefix)
    if not c:
        return set()
    skip_top = {"windows", "users", "programdata", "proton_shared_dir"}
    roots = [os.path.join(c, n) for n in os.listdir(c) if n.lower() not in skip_top]
    found = set()
    for root in roots:
        base_depth = root.count(os.sep)
        for dirpath, dirs, files in os.walk(root):
            if dirpath.count(os.sep) - base_depth >= 4:
                dirs[:] = []
            for fn in files:
                if fn.lower().endswith(".exe") and not SKIP_EXE_RE.search(fn):
                    found.add(os.path.join(dirpath, fn))
                    if len(found) >= max_files:
                        return found
    return found


def new_programs(before, after):
    return sorted(after - before)


# --------------------------------------------------------------------------- aviso na tela
def notify(title, body=""):
    exe = shutil.which("notify-send")
    if not exe:
        return
    try:
        subprocess.run([exe, "-a", "Andrey OS", "-i", "applications-games", title, body], capture_output=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        pass


# --------------------------------------------------------------------------- diagnóstico
def diagnose(conf=None, home=None):
    """Lista de (rótulo, ok, dica); ok=None é só informação."""
    umu = shutil.which("umu-run")
    res = [("umu-run (launcher do Proton)", bool(umu),
            "Sem ele os .exe abrem só no Wine puro. Reinstale/atualize o Andrey OS.")]
    sys_ge = sorted(os.path.basename(d) for d in glob.glob(os.path.join(SYSTEM_PROTON_DIR, "GE-Proton*")))
    usr_ge = G.installed_proton(home)
    res.append(("Proton-GE instalado", True if (sys_ge or usr_ge) else None,
                "Sem ele o umu-run baixa o mais novo na 1ª execução (precisa de internet). Ou: andrey-games --proton-ge"))
    res.append(("Proton em uso", None, find_proton(conf, home)))
    res.append(("zenity (janelas)", shutil.which("zenity") is not None, "Instale o zenity."))
    res.append(("icoutils (ícone do .exe no atalho)", True if (shutil.which("wrestool") and shutil.which("icotool")) else None,
                "Opcional: sem ele o atalho usa um ícone genérico."))
    return res


def render_report(results):
    out = ["== Executar .exe no Andrey OS (umu-launcher + Proton-GE)"]
    for label, ok, hint in results:
        tag = "ok   " if ok else ("FALTA" if ok is False else "info ")
        out.append("  [%s] %s%s" % (tag, label, "" if ok else "  -> " + hint))
    return "\n".join(out)
