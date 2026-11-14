# -*- coding: utf-8 -*-
"""Lógica do "Jogos" do Andrey OS (Steam, Proton e Wine). Sem interface: o /usr/bin/andrey-games usa isto.

Tudo aqui roda como o próprio usuário (nada de root); instalar pacotes passa pelo apt-helper.
"""
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

PROTON_GE_API = "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest"
PROTON_GE_PREFIX = "https://github.com/GloriousEggroll/proton-ge-custom/releases/download/"
TAG_RE = re.compile(r"^GE-Proton[0-9][0-9A-Za-z._-]{0,40}$")
MIN_MAP_COUNT = 1048576

# O que "Reparar" instala (nomes aceitos pelo apt-helper install-i386)
REPAIR_PACKAGES = ["steam-installer", "wine", "wine32:i386", "libwine:i386", "mesa-vulkan-drivers:i386",
                   "libgl1-mesa-dri:i386", "libglx-mesa0:i386", "libvulkan1:i386", "libvulkan1"]


# --------------------------------------------------------------------------- caminhos
def steam_root(home=None):
    if home:
        base = os.path.join(home, ".local", "share")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "Steam")


def compat_dir(home=None):
    return os.path.join(steam_root(home), "compatibilitytools.d")


# --------------------------------------------------------------------------- Proton-GE
def installed_proton(home=None):
    """Versões de Proton instaladas à mão (compatibilitytools.d)."""
    out = []
    for vdf in glob.glob(os.path.join(compat_dir(home), "*", "compatibilitytool.vdf")):
        out.append(os.path.basename(os.path.dirname(vdf)))
    return sorted(out)


def parse_release(data):
    """(tag, nome_tar, url_tar, url_sha512) do JSON da última release do Proton-GE, ou None se algo não bater.

    Só aceita endereços do repositório oficial e nomes simples: nada do JSON vira caminho ou comando sem filtro."""
    try:
        tag = data["tag_name"]
        assets = {a["name"]: a["browser_download_url"] for a in data["assets"]}
    except (KeyError, TypeError):
        return None
    if not isinstance(tag, str) or not TAG_RE.match(tag):
        return None
    tar = tag + ".tar.gz"
    sha = tag + ".sha512sum"
    if tar not in assets or sha not in assets:
        return None
    for u in (assets[tar], assets[sha]):
        if not isinstance(u, str) or not u.startswith(PROTON_GE_PREFIX):
            return None
    return tag, tar, assets[tar], assets[sha]


def parse_checksum(text, filename):
    """SHA-512 (hex) de `filename` num arquivo no formato do sha512sum; None se não houver."""
    for ln in (text or "").splitlines():
        parts = ln.split()
        if len(parts) == 2 and parts[1].lstrip("*") == filename and re.fullmatch(r"[0-9a-fA-F]{128}", parts[0]):
            return parts[0].lower()
    return None


def _fetch(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": "andrey-os-games"})
    return urllib.request.urlopen(req, timeout=timeout)


def safe_extract(tar_path, dest, expected_top):
    """Extrai o tar.gz em `dest`; recusa caminhos fora da pasta e qualquer pasta de topo diferente da esperada."""
    real_dest = os.path.realpath(dest)
    with tarfile.open(tar_path, "r:gz") as tf:
        members = tf.getmembers()
        for m in members:
            top = m.name.split("/", 1)[0]
            target = os.path.realpath(os.path.join(dest, m.name))
            if top != expected_top or not (target == real_dest or target.startswith(real_dest + os.sep)):
                raise ValueError("arquivo com caminho inesperado: " + m.name)
            if m.isdev():
                raise ValueError("arquivo especial no pacote: " + m.name)
        try:
            tf.extractall(dest, members=members, filter="data")
        except TypeError:  # Python antigo sem 'filter' (os checks acima já valem)
            tf.extractall(dest, members=members)


def install_proton_ge(home=None, say=print):
    """Baixa o Proton-GE mais novo, confere o SHA-512 e instala em compatibilitytools.d. Devolve a tag instalada."""
    say("Consultando a última versão do Proton-GE…")
    with _fetch(PROTON_GE_API) as r:
        rel = parse_release(json.load(r))
    if not rel:
        raise RuntimeError("Resposta inesperada do GitHub (versão do Proton-GE não reconhecida).")
    tag, tar_name, tar_url, sha_url = rel
    dest = compat_dir(home)
    if os.path.isdir(os.path.join(dest, tag)):
        say(f"{tag} já está instalado.")
        return tag
    os.makedirs(dest, exist_ok=True)
    with _fetch(sha_url) as r:
        want = parse_checksum(r.read(65536).decode("utf-8", "replace"), tar_name)
    if not want:
        raise RuntimeError("Não achei a soma de verificação (SHA-512) da versão " + tag)
    work = tempfile.mkdtemp(prefix=".andrey-ge-", dir=dest)
    try:
        tar_path = os.path.join(work, tar_name)
        h = hashlib.sha512()
        say(f"Baixando {tag}… (cerca de 450 MB)")
        with _fetch(tar_url, timeout=60) as r, open(tar_path, "wb") as out:
            for chunk in iter(lambda: r.read(1 << 20), b""):
                h.update(chunk)
                out.write(chunk)
        if h.hexdigest() != want:
            raise RuntimeError("Soma de verificação não confere: download corrompido ou adulterado. Nada foi instalado.")
        say("Conferido. Instalando…")
        stage = os.path.join(work, "x")
        os.makedirs(stage)
        safe_extract(tar_path, stage, tag)
        if not os.path.isfile(os.path.join(stage, tag, "compatibilitytool.vdf")):
            raise RuntimeError("Pacote sem compatibilitytool.vdf: não parece um Proton válido.")
        os.replace(os.path.join(stage, tag), os.path.join(dest, tag))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    say(f"{tag} instalado. Reinicie o Steam e escolha em Configurações > Compatibilidade.")
    return tag


# --------------------------------------------------------------------------- diagnóstico
def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def pkg_installed(name):
    return "install ok installed" in _run(["dpkg-query", "-W", "-f=${Status}", name])


def read_int(path):
    try:
        with open(path, encoding="ascii") as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


def diagnose(home=None):
    """Lista de (rótulo, ok, dica). ok=None significa 'não se aplica' (só informativo)."""
    res = []
    arch = "i386" in _run(["dpkg", "--print-foreign-architectures"]).split()
    res.append(("Arquitetura de 32 bits (i386)", arch, "Rode 'Reparar' para habilitar e instalar as bibliotecas."))
    res.append(("Steam instalado", pkg_installed("steam-installer") or shutil.which("steam") is not None,
                "Instale pela Loja ou rode 'Reparar'."))
    wine = shutil.which("wine") is not None
    res.append(("Wine instalado", wine, "Instale pela Loja ou rode 'Reparar'."))
    res.append(("Wine de 32 bits (wine32)", pkg_installed("wine32:i386") or pkg_installed("wine32"),
                "Sem ele, programas .exe de 32 bits não abrem. Rode 'Reparar'."))
    res.append(("Vulkan 32 bits (jogos antigos e Steam)", os.path.exists("/usr/lib/i386-linux-gnu/libvulkan.so.1"),
                "Falta libvulkan1:i386. Rode 'Reparar'."))
    res.append(("OpenGL/Mesa 32 bits", os.path.isdir("/usr/lib/i386-linux-gnu/dri"),
                "Falta libgl1-mesa-dri:i386. Rode 'Reparar'."))
    if os.path.isdir("/proc/driver/nvidia"):
        res.append(("Bibliotecas NVIDIA de 32 bits", pkg_installed("nvidia-driver-libs-i386")
                    or bool(glob.glob("/usr/lib/i386-linux-gnu/libGLX_nvidia.so*")),
                    "Com NVIDIA o Steam precisa de nvidia-driver-libs-i386 (abra 'Drivers e hardware')."))
    mc = read_int("/proc/sys/vm/max_map_count")
    res.append(("vm.max_map_count alto (jogos pesados)", mc is not None and mc >= MIN_MAP_COUNT,
                "Rode: sudo sysctl --system   (ou reinicie)."))
    ns = read_int("/proc/sys/user/max_user_namespaces")
    res.append(("Namespaces de usuário (o Steam Runtime usa)", ns is None or ns > 0,
                "Sem isso o Proton não abre jogos. Rode: sudo sysctl user.max_user_namespaces=63000"))
    ge = installed_proton(home)
    res.append(("Proton-GE instalado (opcional)", True if ge else None,
                "Opcional: o Steam já baixa o Proton oficial sozinho."))
    return res


def render_report(results, home=None):
    out = ["== Jogos no Andrey OS"]
    for label, ok, hint in results:
        tag = "ok   " if ok else ("info " if ok is None else "FALTA")
        out.append(f"  [{tag}] {label}" + ("" if ok else f"  -> {hint}"))
    ge = installed_proton(home)
    if ge:
        out.append("  Proton-GE: " + ", ".join(ge))
    out.append("")
    out.append("Proton oficial: o Steam baixa sozinho (Steam Play). "
               "Para Proton-GE: andrey-games --proton-ge")
    return "\n".join(out)
