# -*- coding: utf-8 -*-
"""Detecção e recomendações de compatibilidade de jogos para o Andrey OS.

O módulo não altera configurações globais do Steam. As opções de lançamento são
temporárias e as mudanças do perfil de desempenho são restauradas ao fechar o jogo.
"""
from __future__ import annotations

import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
from typing import Dict, Iterable, List, Optional


GAMES = {
    "mta": {
        "name": "MTA: San Andreas", "keys": ("multi theft auto", "mta san andreas", "mtasa"),
        "status": "Suporte parcial", "proton": "Proton Experimental ou GE-Proton",
        "detail": "O projeto MTA documenta uso via Wine em caráter best-effort; há problemas variáveis por servidor e versão.",
        "fix": "PROTON_USE_WINED3D=1", "fix_label": "Usar WineD3D no lugar do DXVK (reversível).",
    },
    "fivem": {
        "name": "FiveM", "keys": ("fivem",), "status": "Problemático",
        "proton": "Nenhum Proton resolve a limitação atual do cliente/anti-cheat",
        "detail": "O cliente pode abrir via Wine, mas recursos e autenticação protegidos podem ficar indisponíveis.",
        "fix": None, "fix_label": "Sem correção automática: não desativamos nem contornamos o anti-cheat.",
    },
    "ea-fc": {
        "name": "EA SPORTS FC", "keys": ("ea sports fc", "fifa "), "status": "Problemático",
        "proton": "Nenhum recomendado enquanto o EA Javelin não oferecer suporte ao Proton",
        "detail": "O anti-cheat Javelin e o launcher da EA podem impedir a execução no Proton.",
        "fix": None, "fix_label": "Sem correção automática conhecida e segura para o anti-cheat.",
    },
    "fortnite": {
        "name": "Fortnite", "keys": ("fortnite",), "status": "Problemático",
        "proton": "Nenhum: Fortnite não tem suporte nativo ao Linux/Proton atualmente",
        "detail": "A Epic declara que Fortnite não é suportado em Linux nem em máquinas virtuais.",
        "fix": None, "fix_label": "Sem correção automática: não tentamos contornar o anti-cheat.",
    },
}


def _steam_roots(home: Optional[str] = None) -> List[str]:
    home = home or os.path.expanduser("~")
    candidates = [os.path.join(home, ".local/share/Steam"), os.path.join(home, ".steam/steam"),
                  os.path.join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam")]
    roots = []
    for root in candidates:
        if os.path.isdir(root) and root not in roots:
            roots.append(root)
        library_file = os.path.join(root, "steamapps/libraryfolders.vdf")
        try:
            text = open(library_file, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for path in re.findall(r'"path"\s*"((?:[^"\\]|\\.)+)"', text):
            path = path.replace("\\\\", "\\").replace("\\\"", '"')
            if os.path.isdir(os.path.join(path, "steamapps")) and path not in roots:
                roots.append(path)
    return roots


def _manifest_name(path: str) -> str:
    try:
        text = open(path, encoding="utf-8", errors="replace").read(65536)
    except OSError:
        return ""
    match = re.search(r'"name"\s*"((?:[^"\\]|\\.)*)"', text)
    return match.group(1) if match else ""


def detect_games(home: Optional[str] = None, roots: Optional[Iterable[str]] = None) -> List[Dict[str, str]]:
    """Encontra os jogos da lista em manifests Steam locais, sem executar os jogos."""
    home_dir = home or os.path.expanduser("~")
    found = {}
    for root in roots if roots is not None else _steam_roots(home):
        for manifest in glob.glob(os.path.join(root, "steamapps", "appmanifest_*.acf")):
            title = _manifest_name(manifest)
            folded = title.casefold()
            for slug, game in GAMES.items():
                if slug not in found and any(key in folded for key in game["keys"]):
                    found[slug] = {"id": slug, "name": game["name"], "steam_name": title,
                                   "manifest": manifest, "status": game["status"]}
    # MTA/FiveM e launchers da EA/Epic costumam ficar fora dos manifests do Steam.
    # Procura somente nos caminhos usuais de prefixo e nos executáveis conhecidos.
    if roots is None or home is not None:
        prefix_roots = [os.path.join(home_dir, ".wine")]
        prefix_roots += glob.glob(os.path.join(home_dir, ".local/share/wineprefixes", "*"))
        prefix_roots += glob.glob(os.path.join(home_dir, ".local/share/andrey-os/exe/prefixes", "*"))
        prefix_roots += glob.glob(os.path.join(home_dir, "Games", "*"))
        for root in _steam_roots(home_dir):
            prefix_roots += glob.glob(os.path.join(root, "steamapps/compatdata", "*", "pfx"))
        patterns = {
            "mta": ("drive_c/Program Files/MTA San Andreas*/Multi Theft Auto.exe",
                    "drive_c/Program Files (x86)/MTA San Andreas*/Multi Theft Auto.exe"),
            "fivem": ("drive_c/users/*/AppData/Local/FiveM/FiveM.exe",),
            "ea-fc": ("drive_c/Program Files/EA Games/EA SPORTS FC */*.exe",),
            "fortnite": ("drive_c/Program Files/Epic Games/Fortnite/FortniteGame/Binaries/Win64/"
                         "FortniteClient-Win64-Shipping.exe",),
        }
        for slug, relative_patterns in patterns.items():
            if slug in found:
                continue
            match = next((p for prefix in prefix_roots for relative in relative_patterns
                          for p in glob.glob(os.path.join(prefix, relative))), None)
            if match:
                found[slug] = {"id": slug, "name": GAMES[slug]["name"], "steam_name": "",
                               "manifest": match, "status": GAMES[slug]["status"]}
    return list(found.values())


def installed_proton_tools(home: Optional[str] = None) -> List[str]:
    """Lista versões selecionáveis que já existem nas bibliotecas Steam locais."""
    tools = set()
    for root in _steam_roots(home):
        compat = os.path.join(root, "steamapps/compatibilitytools.d")
        compat2 = os.path.join(root, "compatibilitytools.d")
        for folder in (compat, compat2):
            for marker in glob.glob(os.path.join(folder, "*", "compatibilitytool.vdf")):
                tools.add(os.path.basename(os.path.dirname(marker)))
        common = os.path.join(root, "steamapps/common")
        for name in os.listdir(common) if os.path.isdir(common) else []:
            if name.casefold().startswith("proton"):
                tools.add(name)
    return sorted(tools, key=str.casefold)


def launch_options(slug: str, profile: str = "balanced") -> str:
    """Gera a opção Steam reversível; nunca adiciona comandos para burlar anti-cheat."""
    if slug not in GAMES:
        raise ValueError("jogo não reconhecido: " + slug)
    if profile not in ("balanced", "performance"):
        raise ValueError("perfil deve ser balanced ou performance")
    parts = ["andrey-game-compat", "--launch", "--game", slug, "--profile", profile, "--", "%command%"]
    return " ".join(parts)


def _gsettings_get(key: str) -> Optional[str]:
    try:
        p = subprocess.run(["gsettings", "get", "org.mate.Marco.general", key], capture_output=True,
                           text=True, timeout=3, check=True)
        return p.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None


def _gsettings_set(key: str, value: str) -> None:
    subprocess.run(["gsettings", "set", "org.mate.Marco.general", key, value],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3, check=False)


def launch(slug: str, profile: str, command: List[str]) -> int:
    """Executa o comando Steam e restaura o compositor/perfil de energia no encerramento."""
    if not command:
        raise ValueError("faltou o comando do jogo (use %command% no Steam)")
    if slug not in GAMES or profile not in ("balanced", "performance"):
        raise ValueError("jogo ou perfil inválido")
    env = os.environ.copy()
    game = GAMES[slug]
    if game["fix"]:
        env[game["fix"].split("=", 1)[0]] = game["fix"].split("=", 1)[1]

    old_composite = None
    old_power = None
    power_tool = shutil.which("powerprofilesctl")
    if profile == "performance":
        old_composite = _gsettings_get("compositing-manager")
        if old_composite == "true":
            _gsettings_set("compositing-manager", "false")
        if power_tool:
            try:
                current = subprocess.run([power_tool, "get"], capture_output=True, text=True, timeout=3)
                if current.returncode == 0:
                    old_power = current.stdout.strip()
                    if old_power != "performance":
                        subprocess.run([power_tool, "set", "performance"], capture_output=True, timeout=5)
            except (OSError, subprocess.SubprocessError):
                old_power = None

    proc = None
    old_handlers = {}
    try:
        proc = subprocess.Popen(command, env=env)
        def stop(signum, _frame):
            if proc and proc.poll() is None:
                proc.send_signal(signum)
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            old_handlers[sig] = signal.signal(sig, stop)
        return proc.wait()
    finally:
        for sig, handler in old_handlers.items():
            signal.signal(sig, handler)
        if old_composite == "true":
            _gsettings_set("compositing-manager", "true")
        if old_power and power_tool:
            try:
                subprocess.run([power_tool, "set", old_power], capture_output=True, timeout=5)
            except (OSError, subprocess.SubprocessError):
                pass


def describe(slug: str, tools: Optional[List[str]] = None) -> str:
    game = GAMES[slug]
    tools = tools if tools is not None else installed_proton_tools()
    if "MTA" in game["name"]:
        found = next((x for x in tools if "ge-proton" in x.casefold()), None)
        proton = found or "Proton Experimental (ou instale Proton-GE pelo Andrey Jogos)"
    else:
        found = next((x for x in tools if "experimental" in x.casefold()), None)
        proton = found or "Proton Experimental (se disponível)"
    return (f"{game['name']}\nStatus: {game['status']}\nProton sugerido: {game['proton']}\n"
            f"Versão instalada localmente (não garante compatibilidade): {proton}\n\n{game['detail']}\n{game['fix_label']}\n\n"
            f"Opção Steam ({game['name']}, perfil equilibrado):\n{launch_options(slug)}")


def main(argv: List[str]) -> int:
    if argv and argv[0] == "--launch":
        try:
            marker = argv.index("--")
            opts, command = argv[1:marker], argv[marker + 1:]
            game = opts[opts.index("--game") + 1]
            profile = opts[opts.index("--profile") + 1]
            return launch(game, profile, command)
        except (ValueError, IndexError) as exc:
            print("andrey-game-compat: %s" % exc, file=sys.stderr)
            return 2
    if "--json" in argv:
        print(json.dumps(detect_games(), ensure_ascii=False, indent=2))
    elif "--scan" in argv:
        games = detect_games()
        if not games:
            print("Nenhum jogo MTA, FiveM, EA SPORTS FC ou Fortnite foi detectado nas bibliotecas Steam.")
        for game in games:
            print(describe(game["id"]))
            print("\n" + "-" * 60)
    elif len(argv) == 2 and argv[0] == "--recommend" and argv[1] in GAMES:
        print(describe(argv[1]))
    else:
        print("Uso: andrey-game-compat --scan | --json | --recommend ID | --launch --game ID --profile balanced|performance -- %command%")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
