# -*- coding: utf-8 -*-
"""Lógica do "Jogos" do Cisma OS (Steam, Proton e Wine). Sem interface: o /usr/bin/cisma-games usa isto.

Tudo aqui roda como o próprio usuário (nada de root); instalar pacotes passa pelo apt-helper.

Organização do módulo:
  1. constantes e tipos
  2. caminhos
  3. Proton-GE (release -> checksum -> download -> extração -> instalação)
  4. diagnóstico (checks pequenos e declarativos) e relatório
"""
from __future__ import annotations

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
from typing import Any, Callable, List, NamedTuple, Optional, Tuple

# --------------------------------------------------------------------------- constantes
PROTON_GE_API = "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases/latest"
PROTON_GE_PREFIX = "https://github.com/GloriousEggroll/proton-ge-custom/releases/download/"
PROTON_GE_LATEST = "https://github.com/GloriousEggroll/proton-ge-custom/releases/latest"
TAG_RE = re.compile(r"^GE-Proton[0-9][0-9A-Za-z._-]{0,40}$")
SHA512_RE = re.compile(r"[0-9a-fA-F]{128}")
MIN_MAP_COUNT = 1048576

API_TIMEOUT = 30            # segundos: API do GitHub e arquivo de checksum
DOWNLOAD_TIMEOUT = 60       # segundos: download do tar.gz
CHUNK_SIZE = 1 << 20        # 1 MiB por leitura
CHECKSUM_MAX_BYTES = 65536  # o .sha512sum é minúsculo; limita o que lemos
MAX_DOWNLOAD_BYTES = 2 << 30  # 2 GiB: o pacote tem ~450 MB, isto só barra respostas absurdas
WORK_PREFIX = ".cisma-ge-"
RUN_TIMEOUT = 20

# O que "Reparar" instala (nomes aceitos pelo apt-helper install-i386)
REPAIR_PACKAGES = ["steam-installer", "wine", "wine32:i386", "libwine:i386", "mesa-vulkan-drivers:i386",
                   "libgl1-mesa-dri:i386", "libglx-mesa0:i386", "libvulkan1:i386", "libvulkan1"]

Say = Callable[[str], Any]
Fetcher = Callable[..., Any]   # fetch(url, timeout=...) -> objeto com read() e context manager
Runner = Callable[[List[str]], str]


class ProtonInstallError(RuntimeError):
    """Falha ao instalar o Proton-GE (rede, checksum, pacote inválido...). Continua sendo um RuntimeError."""


class Check(NamedTuple):
    """Resultado de um item do diagnóstico. ok=None significa 'não se aplica' (só informativo).

    Por ser NamedTuple, ainda desempacota como (rótulo, ok, dica), como antes."""
    label: str
    ok: Optional[bool]
    hint: str


# --------------------------------------------------------------------------- caminhos
def steam_root(home: Optional[str] = None) -> str:
    if home:
        base = os.path.join(home, ".local", "share")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "Steam")


def compat_dir(home: Optional[str] = None) -> str:
    return os.path.join(steam_root(home), "compatibilitytools.d")


# --------------------------------------------------------------------------- Proton-GE: leitura
def installed_proton(home: Optional[str] = None) -> List[str]:
    """Versões de Proton instaladas à mão (compatibilitytools.d)."""
    pattern = os.path.join(compat_dir(home), "*", "compatibilitytool.vdf")
    return sorted(os.path.basename(os.path.dirname(vdf)) for vdf in glob.glob(pattern))


def _asset_urls(data: Any) -> Optional[dict]:
    """{nome: url} dos assets da release, ou None se o JSON não tiver o formato esperado."""
    if not isinstance(data, dict) or not isinstance(data.get("assets"), list):
        return None
    urls = {}
    for asset in data["assets"]:
        if not isinstance(asset, dict):
            return None
        name, url = asset.get("name"), asset.get("browser_download_url")
        if not isinstance(name, str) or not isinstance(url, str):
            return None
        urls[name] = url
    return urls


def parse_release(data: Any) -> Optional[Tuple[str, str, str, str]]:
    """(tag, nome_tar, url_tar, url_sha512) do JSON da última release do Proton-GE, ou None se algo não bater.

    Só aceita endereços do repositório oficial e nomes simples: nada do JSON vira caminho ou comando sem filtro."""
    tag = data.get("tag_name") if isinstance(data, dict) else None
    if not isinstance(tag, str) or not TAG_RE.match(tag):
        return None
    assets = _asset_urls(data)
    if assets is None:
        return None
    # Desde o GE-Proton11 os arquivos levam o sufixo da arquitetura (GE-Proton11-6-x86_64.tar.gz);
    # as versões antigas não tinham. Prefere o x86_64 e cai no nome antigo.
    for suffix in ("-x86_64", ""):
        tar, sha = tag + suffix + ".tar.gz", tag + suffix + ".sha512sum"
        if tar in assets and sha in assets:
            break
    else:
        return None
    if not (assets[tar].startswith(PROTON_GE_PREFIX) and assets[sha].startswith(PROTON_GE_PREFIX)):
        return None
    return tag, tar, assets[tar], assets[sha]


def parse_checksum(text: Optional[str], filename: str) -> Optional[str]:
    """SHA-512 (hex) de `filename` num arquivo no formato do sha512sum; None se não houver."""
    for line in (text or "").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == filename and SHA512_RE.fullmatch(parts[0]):
            return parts[0].lower()
    return None


# --------------------------------------------------------------------------- Proton-GE: rede
def _fetch(url: str, timeout: int = API_TIMEOUT):
    req = urllib.request.Request(url, headers={"User-Agent": "cisma-os-games"})
    return urllib.request.urlopen(req, timeout=timeout)


def _latest_release(fetch: Fetcher) -> Tuple[str, str, str, str]:
    api_error = None
    try:
        with fetch(PROTON_GE_API, timeout=API_TIMEOUT) as r:
            rel = parse_release(json.load(r))
        if rel:
            return rel
        api_error = "resposta inesperada da API"
    except Exception as exc:
        api_error = str(exc)

    # A API REST pode responder 403 quando a cota anonima do IP do builder acaba.
    # A pagina /latest redireciona para a tag. Montamos os nomes permitidos e exigimos
    # que o checksum oficial da release confirme o tar antes de iniciar o download.
    try:
        with fetch(PROTON_GE_LATEST, timeout=API_TIMEOUT) as r:
            final_url = r.geturl()
    except Exception as exc:
        raise ProtonInstallError("A API do GitHub falhou (%s) e nao consegui abrir a pagina de releases: %s" %
                                 (api_error, exc)) from exc
    match = re.search(r"/releases/tag/([^/?#]+)$", final_url)
    tag = match.group(1) if match else ""
    if not TAG_RE.fullmatch(tag):
        raise ProtonInstallError("A API do GitHub falhou (%s) e a pagina nao revelou uma tag valida do Proton-GE." % api_error)

    for suffix in ("-x86_64", ""):
        tar_name = tag + suffix + ".tar.gz"
        sha_url = PROTON_GE_PREFIX + tag + "/" + tag + suffix + ".sha512sum"
        try:
            with fetch(sha_url, timeout=API_TIMEOUT) as r:
                checksum = parse_checksum(r.read(CHECKSUM_MAX_BYTES).decode("utf-8", "replace"), tar_name)
        except Exception:
            checksum = None
        if checksum:
            return tag, tar_name, PROTON_GE_PREFIX + tag + "/" + tar_name, sha_url
    raise ProtonInstallError("A API do GitHub falhou (%s) e nao achei checksum SHA-512 valido na release %s." %
                             (api_error, tag))


def _expected_sha512(fetch: Fetcher, sha_url: str, tar_name: str, tag: str) -> str:
    with fetch(sha_url, timeout=API_TIMEOUT) as r:
        want = parse_checksum(r.read(CHECKSUM_MAX_BYTES).decode("utf-8", "replace"), tar_name)
    if not want:
        raise ProtonInstallError("Não achei a soma de verificação (SHA-512) da versão " + tag)
    return want


def _download_verified(fetch: Fetcher, url: str, dest_path: str, expected_sha512: str) -> None:
    """Baixa `url` para `dest_path` e confere o SHA-512 ao fim. Levanta ProtonInstallError se não bater."""
    digest = hashlib.sha512()
    total = 0
    with fetch(url, timeout=DOWNLOAD_TIMEOUT) as r, open(dest_path, "wb") as out:
        for chunk in iter(lambda: r.read(CHUNK_SIZE), b""):
            total += len(chunk)
            if total > MAX_DOWNLOAD_BYTES:
                raise ProtonInstallError("Download maior do que o esperado; abortado. Nada foi instalado.")
            digest.update(chunk)
            out.write(chunk)
    if digest.hexdigest() != expected_sha512:
        raise ProtonInstallError("Soma de verificação não confere: download corrompido ou adulterado. "
                                 "Nada foi instalado.")


# --------------------------------------------------------------------------- Proton-GE: extração
def _is_within(path: str, real_root: str) -> bool:
    return path == real_root or path.startswith(real_root + os.sep)


def _check_member(member: tarfile.TarInfo, dest: str, real_dest: str, expected_top: str) -> None:
    """Recusa o membro se sair da pasta, tiver pasta de topo errada, for arquivo especial ou link para fora."""
    top = member.name.split("/", 1)[0]
    target = os.path.realpath(os.path.join(dest, member.name))
    if top != expected_top or not _is_within(target, real_dest):
        raise ValueError("arquivo com caminho inesperado: " + member.name)
    if member.isdev():
        raise ValueError("arquivo especial no pacote: " + member.name)
    if member.issym() or member.islnk():
        # symlink é relativo à pasta do próprio link; hardlink é relativo à raiz do tar
        base = os.path.dirname(target) if member.issym() else real_dest
        link_target = os.path.realpath(os.path.join(base, member.linkname))
        if not _is_within(link_target, real_dest):
            raise ValueError("link apontando para fora da pasta: " + member.name)


def safe_extract(tar_path: str, dest: str, expected_top: str) -> None:
    """Extrai o tar.gz em `dest`; recusa caminhos fora da pasta e qualquer pasta de topo diferente da esperada."""
    real_dest = os.path.realpath(dest)
    with tarfile.open(tar_path, "r:gz") as tf:
        members = tf.getmembers()
        for member in members:
            _check_member(member, dest, real_dest, expected_top)
        if hasattr(tarfile, "data_filter"):
            tf.extractall(dest, members=members, filter="data")
        else:  # Python antigo sem 'filter' (os checks acima já valem)
            tf.extractall(dest, members=members)


def _extract_and_validate(tar_path: str, work: str, tag: str) -> str:
    """Extrai o formato oficial da release e devolve a pasta Proton validada."""
    name = os.path.basename(tar_path)
    if name == tag + ".tar.gz":
        archive_top = tag
    elif name == tag + "-x86_64.tar.gz":
        # Releases recentes incluem a arquitetura também no diretório raiz.
        archive_top = tag + "-x86_64"
    else:
        raise ProtonInstallError("Nome inesperado para o pacote Proton-GE: " + name)
    stage = os.path.join(work, "x")
    os.makedirs(stage)
    safe_extract(tar_path, stage, archive_top)
    staged_tag = os.path.join(stage, archive_top)
    if not os.path.isfile(os.path.join(staged_tag, "compatibilitytool.vdf")):
        raise ProtonInstallError("Pacote sem compatibilitytool.vdf: não parece um Proton válido.")
    return staged_tag


# --------------------------------------------------------------------------- Proton-GE: instalação
def _install_proton_ge(home: Optional[str], say: Say, dest: Optional[str], fetch: Fetcher) -> str:
    say("Consultando a última versão do Proton-GE…")
    tag, tar_name, tar_url, sha_url = _latest_release(fetch)
    dest = dest or compat_dir(home)
    if os.path.isdir(os.path.join(dest, tag)):
        say(f"{tag} já está instalado.")
        return tag
    os.makedirs(dest, exist_ok=True)
    want = _expected_sha512(fetch, sha_url, tar_name, tag)
    work = tempfile.mkdtemp(prefix=WORK_PREFIX, dir=dest)
    try:
        tar_path = os.path.join(work, tar_name)
        say(f"Baixando {tag}… (cerca de 450 MB)")
        _download_verified(fetch, tar_url, tar_path, want)
        say("Conferido. Instalando…")
        staged_tag = _extract_and_validate(tar_path, work, tag)
        os.replace(staged_tag, os.path.join(dest, tag))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    say(f"{tag} instalado. Reinicie o Steam e escolha em Configurações > Compatibilidade.")
    return tag


def install_proton_ge(home: Optional[str] = None, say: Say = print, dest: Optional[str] = None,
                      *, fetch: Optional[Fetcher] = None) -> str:
    """Baixa o Proton-GE mais novo, confere o SHA-512 e instala em compatibilitytools.d (ou em `dest`, como faz o hook
    da ISO em /usr/share/cisma-os/proton). Devolve a tag instalada.

    `fetch` (opcional) substitui o acesso à rede, útil em testes. Qualquer falha vira ProtonInstallError."""
    try:
        return _install_proton_ge(home, say, dest, fetch or _fetch)
    except ProtonInstallError:
        raise
    except (OSError, ValueError, EOFError, tarfile.TarError) as exc:  # rede, disco, JSON ruim, tar corrompido
        raise ProtonInstallError(f"Falha ao instalar o Proton-GE: {exc}") from exc


# --------------------------------------------------------------------------- sistema
def _run(cmd: List[str]) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=RUN_TIMEOUT).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def pkg_installed(name: str, run: Optional[Runner] = None) -> bool:
    return "install ok installed" in (run or _run)(["dpkg-query", "-W", "-f=${Status}", name])


def read_int(path: str) -> Optional[int]:
    try:
        with open(path, encoding="ascii") as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


# --------------------------------------------------------------------------- diagnóstico
def _optional(present: bool) -> Optional[bool]:
    """Para itens opcionais: True se presente, None ('só informativo') se ausente."""
    return True if present else None


def _nvidia_check(run: Runner) -> Optional[Check]:
    if not os.path.isdir("/proc/driver/nvidia"):
        return None
    ok = pkg_installed("nvidia-driver-libs-i386", run) or bool(glob.glob("/usr/lib/i386-linux-gnu/libGLX_nvidia.so*"))
    return Check("Bibliotecas NVIDIA de 32 bits", ok,
                 "Com NVIDIA o Steam precisa de nvidia-driver-libs-i386 (abra 'Drivers e hardware').")


def diagnose(home: Optional[str] = None, *, run: Optional[Runner] = None) -> List[Check]:
    """Lista de Check(rótulo, ok, dica). ok=None significa 'não se aplica' (só informativo).

    `run` (opcional) substitui a execução de comandos, útil em testes."""
    run = run or _run
    has_i386 = "i386" in run(["dpkg", "--print-foreign-architectures"]).split()
    max_map = read_int("/proc/sys/vm/max_map_count")
    max_ns = read_int("/proc/sys/user/max_user_namespaces")

    checks: List[Optional[Check]] = [
        Check("Arquitetura de 32 bits (i386)", has_i386,
              "Rode 'Reparar' para habilitar e instalar as bibliotecas."),
        Check("Steam instalado", pkg_installed("steam-installer", run) or shutil.which("steam") is not None,
              "Instale pela Loja ou rode 'Reparar'."),
        Check("Wine instalado", shutil.which("wine") is not None,
              "Instale pela Loja ou rode 'Reparar'."),
        Check("Wine de 32 bits (wine32)", pkg_installed("wine32:i386", run) or pkg_installed("wine32", run),
              "Sem ele, programas .exe de 32 bits não abrem. Rode 'Reparar'."),
        Check("Vulkan 32 bits (jogos antigos e Steam)", os.path.exists("/usr/lib/i386-linux-gnu/libvulkan.so.1"),
              "Falta libvulkan1:i386. Rode 'Reparar'."),
        Check("OpenGL/Mesa 32 bits", os.path.isdir("/usr/lib/i386-linux-gnu/dri"),
              "Falta libgl1-mesa-dri:i386. Rode 'Reparar'."),
        _nvidia_check(run),
        Check("vm.max_map_count alto (jogos pesados)", max_map is not None and max_map >= MIN_MAP_COUNT,
              "Rode: sudo sysctl --system   (ou reinicie)."),
        Check("Namespaces de usuário (o Steam Runtime usa)", max_ns is None or max_ns > 0,
              "Sem isso o Proton não abre jogos. Rode: sudo sysctl user.max_user_namespaces=63000"),
        Check("Proton-GE instalado (opcional)", _optional(bool(installed_proton(home))),
              "Opcional: o Steam já baixa o Proton oficial sozinho."),
        Check("umu-run (.exe com duplo clique, via Proton-GE)", _optional(shutil.which("umu-run") is not None),
              "Sem ele os .exe abrem só no Wine puro. Atualize o Cisma OS ou reinstale a ISO mais nova."),
    ]
    return [c for c in checks if c is not None]


def _status_tag(ok: Optional[bool]) -> str:
    return "ok   " if ok else ("info " if ok is None else "FALTA")


def render_report(results, home: Optional[str] = None) -> str:
    lines = ["== Jogos no Cisma OS"]
    for label, ok, hint in results:
        lines.append(f"  [{_status_tag(ok)}] {label}" + ("" if ok else f"  -> {hint}"))
    ge = installed_proton(home)
    if ge:
        lines.append("  Proton-GE: " + ", ".join(ge))
    lines.append("")
    lines.append("Proton oficial: o Steam baixa sozinho (Steam Play). "
                 "Para Proton-GE: cisma-games --proton-ge")
    return "\n".join(lines)
