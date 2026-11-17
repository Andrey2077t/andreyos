# -*- coding: utf-8 -*-
"""Diagnóstico e plano seguro de VFIO/Looking Glass; não altera a configuração de boot."""
from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import shlex
import tempfile
import xml.etree.ElementTree as ET
from typing import Dict, List, Optional


def inspect_system(root: str = "/", cpuinfo: Optional[str] = None, cmdline: Optional[str] = None) -> Dict[str, object]:
    """Inspeciona virtualização, IOMMU, GPUs e ferramentas sem carregar módulos ou mudar o sistema."""
    cpuinfo = cpuinfo if cpuinfo is not None else os.path.join(root, "proc/cpuinfo")
    cmdline = cmdline if cmdline is not None else os.path.join(root, "proc/cmdline")
    try:
        cpu = open(cpuinfo, encoding="utf-8", errors="replace").read().lower()
    except OSError:
        cpu = ""
    flags = set()
    for line in cpu.splitlines():
        if line.startswith(("flags", "features")) and ":" in line:
            flags.update(line.split(":", 1)[1].split())
    try:
        boot = open(cmdline, encoding="utf-8", errors="replace").read().strip().split()
    except OSError:
        boot = []

    groups_path = os.path.join(root, "sys/kernel/iommu_groups")
    groups = sorted(glob.glob(os.path.join(groups_path, "[0-9]*")))
    group_details = []
    devices = []
    pci_details = {}
    pci = os.path.join(root, "sys/bus/pci/devices")
    for path in sorted(glob.glob(os.path.join(pci, "*"))):
        try:
            cls = open(os.path.join(path, "class"), encoding="ascii").read().strip().lower()
            vendor = open(os.path.join(path, "vendor"), encoding="ascii").read().strip()
            device = open(os.path.join(path, "device"), encoding="ascii").read().strip()
            driver = os.path.basename(os.path.realpath(os.path.join(path, "driver")))
            try:
                boot_vga = open(os.path.join(path, "boot_vga"), encoding="ascii").read().strip() == "1"
            except OSError:
                boot_vga = False
        except OSError:
            continue
        bdf = os.path.basename(path)
        pci_details[bdf] = {"pci": bdf, "vendor": vendor, "device": device, "class": cls,
                            "driver": driver if driver != "driver" else "nenhum", "boot_vga": boot_vga}
        if cls.startswith(("0x0300", "0x0302")):
            devices.append(pci_details[bdf])

    # Tela conectada ou descritor DRM aberto por qualquer processo marca a GPU como em uso.
    active_bdfs = set()
    open_dri = _open_dri_nodes(root)
    drm_nodes = glob.glob(os.path.join(root, "sys/class/drm/card[0-9]*")) + \
        glob.glob(os.path.join(root, "sys/class/drm/renderD[0-9]*"))
    for drm_node in drm_nodes:
        name = os.path.basename(drm_node)
        if not re.fullmatch(r"(?:card|renderD)[0-9]+", name):
            continue
        bdf = os.path.basename(os.path.realpath(os.path.join(drm_node, "device")))
        if bdf not in pci_details:
            continue
        connectors = glob.glob(os.path.join(root, "sys/class/drm", name + "-*", "status")) if name.startswith("card") else []
        if any(_read(path).strip() == "connected" for path in connectors) or name in open_dri:
            active_bdfs.add(bdf)
        node = os.path.join(root, "dev/dri", name)
        if root == "/" and os.path.exists(node) and _device_is_open(node):
            active_bdfs.add(bdf)

    for gpu in devices:
        gpu["in_use"] = gpu["boot_vga"] or gpu["pci"] in active_bdfs

    group_by_bdf = {}
    for group in groups:
        members = sorted(os.path.basename(x) for x in glob.glob(os.path.join(group, "devices", "*")))
        group_details.append({"id": os.path.basename(group), "devices": [pci_details.get(x, {"pci": x}) for x in members]})
        for bdf in members:
            group_by_bdf[bdf] = os.path.basename(group)
    for gpu in devices:
        gpu["iommu_group"] = group_by_bdf.get(gpu["pci"])

    video_alternatives = [gpu["pci"] for gpu in devices if gpu["driver"] != "vfio-pci"]
    ssh_available = False
    if root == "/":
        for service in ("ssh.service", "sshd.service"):
            try:
                active = subprocess.run(["systemctl", "is-active", "--quiet", service], timeout=3).returncode == 0
            except (OSError, subprocess.SubprocessError):
                active = False
            if active:
                try:
                    addresses = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=3)
                    ssh_available = bool(addresses.stdout.strip())
                except (OSError, subprocess.SubprocessError):
                    pass
            if ssh_available:
                break

    tools = {"virsh": _which("virsh") is not None,
             "virt_manager": shutil.which("virt-manager") is not None,
             "looking_glass": shutil.which("looking-glass-client") is not None,
             "update_grub": _which("update-grub") is not None,
             "update_initramfs": _which("update-initramfs") is not None}
    kernel = os.uname().release if root == "/" else "teste"
    cpu_vendor_match = re.search(r"^vendor_id\s*:\s*(\S+)", cpu, re.MULTILINE)
    cpu_vendor = cpu_vendor_match.group(1) if cpu_vendor_match else ""
    iommu_flag = "intel_iommu=on" if cpu_vendor == "genuineintel" else \
        "amd_iommu=on" if cpu_vendor == "authenticamd" else "IOMMU=<fabricante não identificado>"
    return {"cpu_virtualization": "vmx" in flags or "svm" in flags,
            "cpu_vendor": cpu_vendor,
            "iommu_kernel_params": [iommu_flag, "iommu=pt"],
            "iommu_enabled": bool(groups) or any(x.startswith(("intel_iommu=on", "amd_iommu=on", "iommu=pt")) for x in boot),
            "iommu_groups": len(groups), "iommu_group_details": group_details,
            "vfio_pci_loaded": os.path.exists(os.path.join(root, "sys/module/vfio_pci")),
            "gpus": devices, "pci_devices": pci_details, "video_alternatives": video_alternatives,
            "ssh_recovery": ssh_available, "kernel": kernel,
            "unstable_kernel": any(x in kernel.casefold() for x in ("-rc", "-git", "-zen", "-tkg")),
            "tools": tools}


def _read(path: str) -> str:
    try:
        with open(path, encoding="utf-8", errors="replace") as stream:
            return stream.read()
    except OSError:
        return ""


def _open_dri_nodes(root: str) -> set:
    """DRM card/render nodes abertos, inclusive por processos que não usam o conector da tela."""
    opened = set()
    if root != "/":
        return opened
    for descriptor in glob.glob("/proc/[0-9]*/fd/*"):
        try:
            path = os.readlink(descriptor).removesuffix(" (deleted)")
        except OSError:
            continue
        if path.startswith("/dev/dri/"):
            node = os.path.basename(path)
            if re.fullmatch(r"(?:card|renderD)[0-9]+", node):
                opened.add(node)
    return opened


def _device_is_open(path: str) -> bool:
    fuser = _which("fuser")
    if not fuser:
        return False
    try:
        return subprocess.run([fuser, path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=3).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def _which(command: str) -> Optional[str]:
    found = shutil.which(command)
    if found:
        return found
    for directory in ("/usr/sbin", "/sbin", "/usr/local/sbin"):
        path = os.path.join(directory, command)
        if os.access(path, os.X_OK):
            return path
    return None


def preflight(state: Dict[str, object], target: str, recovery_confirmed: bool = False) -> Dict[str, object]:
    """Retorna bloqueios/avisos. Não muda arquivos nem drivers."""
    blockers, warnings = [], []
    gpu = next((item for item in state["gpus"] if item["pci"] == target), None)
    if not gpu:
        blockers.append("A GPU escolhida não foi detectada neste sistema.")
        return {"allowed": False, "blockers": blockers, "warnings": warnings, "group": None}
    if gpu["in_use"]:
        blockers.append("Esta GPU está em uso pelo boot/tela atual. A opção fica bloqueada.")
    if not state["cpu_virtualization"]:
        blockers.append("VT-x/AMD-V não foi detectado; habilite a virtualização na UEFI/BIOS.")
    group = next((g for g in state["iommu_group_details"]
                  if any(d.get("pci") == target for d in g["devices"])), None)
    if not group:
        blockers.append("A GPU ainda não aparece em um grupo IOMMU; habilite IOMMU na UEFI e inicialize novamente.")
    else:
        critical = []
        unexpected = []
        for device in group["devices"]:
            if device.get("pci") == target:
                continue
            cls = device.get("class", "").removeprefix("0x")
            if cls.startswith(("0108", "0c03", "02")):
                critical.append("{0} ({1}:{2})".format(device.get("pci"), cls, device.get("driver")))
            elif cls.startswith(("0300", "0302")):
                blockers.append("O grupo IOMMU inclui outra GPU ({0}); não é possível isolar apenas a placa escolhida.".format(
                    device.get("pci", "desconhecida")))
            elif not cls.startswith(("0300", "0302", "0403")):
                unexpected.append("{0} (classe {1})".format(device.get("pci"), cls or "desconhecida"))
        if critical:
            blockers.append("O grupo IOMMU contém dispositivos críticos (NVMe/USB/rede): " + ", ".join(critical))
        if unexpected:
            blockers.append("O grupo IOMMU contém dispositivos além da GPU/áudio associado: " + ", ".join(unexpected))
        if not blockers and len(group["devices"]) > 1:
            warnings.append("A GPU compartilha grupo com: " + ", ".join(d.get("pci", "?") for d in group["devices"]
                                                                             if d.get("pci") != target))
    if not recovery_confirmed:
        options = []
        if set(state.get("video_alternatives", [])) - {target}:
            options.append("iGPU/segunda GPU (conecte um monitor nela)")
        if state.get("ssh_recovery"):
            options.append("SSH ativo com IP acessível")
        options.append("Live USB pronto")
        blockers.append("Confirme uma rota de recuperação testada: " + ", ".join(options) + ".")
    if not state["iommu_enabled"]:
        blockers.append("IOMMU não está ativo; ative-o na UEFI/BIOS e reinicie antes de isolar a GPU.")
    if not state["tools"].get("update_grub") or not state["tools"].get("update_initramfs"):
        blockers.append("Ferramentas update-grub/update-initramfs ausentes; não é seguro aplicar a configuração automaticamente.")
    if state.get("unstable_kernel"):
        warnings.append("O kernel parece customizado/pré-lançamento ({0}); prefira um kernel estável antes de VFIO.".format(
            state.get("kernel", "desconhecido")))
    return {"allowed": not blockers, "blockers": blockers, "warnings": warnings, "group": group,
            "recovery": recovery_confirmed, "target": target}


def render_report(state: Dict[str, object]) -> str:
    lines = ["Diagnóstico VFIO + Looking Glass", ""]
    checks = [("Virtualização de CPU (VT-x/AMD-V)", state["cpu_virtualization"]),
              ("IOMMU ativo", state["iommu_enabled"]),
              ("Grupos IOMMU encontrados", state["iommu_groups"]),
              ("Driver vfio-pci carregado", state["vfio_pci_loaded"])]
    for label, value in checks:
        lines.append(("OK" if value else "PENDENTE") + "  " + label + ": " + str(value))
    lines += ["", "GPUs detectadas:"]
    gpus = state["gpus"]
    if not gpus:
        lines.append("  Nenhuma GPU PCIe detectada (ou acesso de leitura indisponível).")
    for gpu in gpus:
        lines.append("  {pci}  {vendor}:{device}  driver={driver}{primary}".format(
            **gpu, primary="  [EM USO: bloqueada para passthrough]" if gpu["in_use"] else ""))
    if state.get("iommu_group_details"):
        lines += ["", "Dispositivos por grupo IOMMU:"]
        for group in state["iommu_group_details"]:
            lines.append("  grupo {0}:".format(group["id"]))
            for device in group["devices"]:
                lines.append("    {pci}  {vendor}:{device} classe={class} driver={driver}".format(
                    pci=device.get("pci", "?"), vendor=device.get("vendor", "?"),
                    device=device.get("device", "?"), **{"class": device.get("class", "?")},
                    driver=device.get("driver", "desconhecido")))
    lines += ["", "Ferramentas:"]
    for tool, present in state["tools"].items():
        lines.append("  {0}: {1}".format(tool, "instalado" if present else "não instalado"))
    lines += ["", "Rotas alternativas de vídeo: " + (", ".join(state.get("video_alternatives", [])) or "nenhuma detectada"),
              "SSH com IP detectado: " + ("sim" if state.get("ssh_recovery") else "não"),
              "Kernel atual: {0}{1}".format(state.get("kernel", "desconhecido"),
                                            " (customizado/pré-lançamento)" if state.get("unstable_kernel") else "")]
    lines += ["", "Importante: a GPU de boot/tela ativa fica bloqueada para passthrough.",
              "Antes de qualquer alteração, o assistente valida a rota alternativa de recuperação e o grupo IOMMU.",
              "ACS Override não será aplicado automaticamente; grupos inseguros bloqueiam a configuração.",
              "Não tente ocultar a máquina virtual ou desativar o anti-cheat.", ""]
    return "\n".join(lines)


def setup_plan(state: Dict[str, object]) -> str:
    """Guia de configuração sem comandos de alteração automáticos."""
    lines = [render_report(state), "Roteiro para uma VM Windows com GPU dedicada", "",
             "1. Confirme VT-x/AMD-V e IOMMU na UEFI/BIOS da placa-mãe.",
             "2. Instale QEMU/KVM, libvirt, virt-manager e OVMF pelos pacotes da distribuição.",
             "3. Identifique a GPU dedicada e as funções PCI relacionadas (GPU e áudio HDMI).",
             "4. Confira o grupo IOMMU e seus dispositivos: NVMe, USB, rede ou componentes não relacionados bloqueiam a configuração.",
             "5. Configure a VM no virt-manager com firmware UEFI e adicione a GPU e o áudio como dispositivos PCI.",
             "6. Instale Windows e drivers oficiais dentro da VM; use Looking Glass para a janela de baixa latência.",
             "7. Teste teclado, mouse, áudio, suspensão e o retorno da GPU ao desligar a VM.", "",
             "O assistente bloqueia a GPU ativa, cria backup de cada arquivo alterado e instala um caminho de reversão.",
             "Nenhuma alteração é aplicada sem confirmação explícita; uma saída alternativa ou Live USB pronto é obrigatório.", "",
              "Se o sistema perder vídeo: no menu GRUB pressione 'e', remova iommu=pt e intel_iommu=on/amd_iommu=on da linha do kernel, inicialize com Ctrl+X/F10 e execute andrey-vfio-revert.", "",
             "Limites publicados pelos desenvolvedores (17/11/2026):",
             "- VALORANT/Vanguard não suporta máquina virtual (Riot; erros VAN 9100/138).",
             "- Fortnite/Easy Anti-Cheat não suporta VM nem sistema operacional não nativo (Epic).",
             "O modo VFIO não remove esses bloqueios e o Andrey OS não vai ocultar a VM."]
    return "\n".join(lines)


STATE_DIR = "/var/lib/andrey-os/vfio"
ACTIVE_STATE = os.path.join(STATE_DIR, "active.json")
MODPROBE_FILE = "/etc/modprobe.d/andrey-vfio.conf"
GRUB_FILE = "/etc/default/grub"
INITRAMFS_MODULES = "/etc/initramfs-tools/modules"
RECOVERY_SCRIPT = "/usr/local/sbin/andrey-vfio-revert"
BIND_SCRIPT = "/usr/local/sbin/andrey-vfio-bind"
UNBIND_SCRIPT = "/usr/local/sbin/andrey-vfio-unbind"


def _atomic_write(path: str, content: str, mode: int = 0o644) -> None:
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".andrey-vfio-", dir=directory, text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _run_system_update() -> None:
    commands = ((_which("update-initramfs"), ["-u", "-k", "all"]), (_which("update-grub"), []))
    for executable, arguments in commands:
        if not executable:
            raise RuntimeError("update-initramfs/update-grub não encontrado")
        command = [executable, *arguments]
        result = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if result.returncode:
            raise RuntimeError("{} falhou: {}".format(command[0], result.stderr.strip() or result.stdout.strip()))


def _backup_files(folder: str, paths: List[str]) -> Dict[str, object]:
    records = {}
    for index, path in enumerate(paths):
        if os.path.islink(path):
            raise RuntimeError("Recusando arquivo de configuração simbólico: " + path)
        exists = os.path.isfile(path)
        record = {"exists": exists, "backup": None, "mode": None}
        if exists:
            backup = os.path.join(folder, "original-{}".format(index))
            shutil.copy2(path, backup)
            record.update({"backup": backup, "mode": os.stat(path).st_mode & 0o777})
        records[path] = record
    return records


def _restore_files(records: Dict[str, object]) -> None:
    for path, record in records.items():
        if path not in {GRUB_FILE, MODPROBE_FILE, INITRAMFS_MODULES, RECOVERY_SCRIPT, BIND_SCRIPT, UNBIND_SCRIPT}:
            raise RuntimeError("O registro de backup contém um caminho inesperado: " + path)
        if os.path.islink(path):
            raise RuntimeError("Recusando restaurar sobre um link simbólico: " + path)
        if record["exists"]:
            backup = record["backup"]
            if not backup or not os.path.isfile(backup):
                raise RuntimeError("Backup ausente; não restaurei arquivos parcialmente: " + path)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            shutil.copy2(backup, path)
            os.chmod(path, record["mode"])
        elif os.path.exists(path):
            os.unlink(path)


def _cpu_vendor(cpuinfo: str = "/proc/cpuinfo") -> str:
    match = re.search(r"^vendor_id\s*:\s*(\S+)", _read(cpuinfo), re.MULTILINE)
    return {"GenuineIntel": "intel", "AuthenticAMD": "amd"}.get(match.group(1) if match else "", "")


def _active_vms_using(members: List[str]) -> List[str]:
    try:
        result = subprocess.run(["virsh", "list", "--state-running", "--name"], capture_output=True,
                                text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError("Não consegui consultar as VMs em execução; operação VFIO bloqueada.") from exc
    if result.returncode:
        raise RuntimeError("Não consegui consultar as VMs em execução: " + (result.stderr.strip() or "virsh falhou."))
    active = []
    for domain in (line.strip() for line in result.stdout.splitlines() if line.strip()):
        try:
            xml = subprocess.run(["virsh", "dumpxml", domain], capture_output=True, text=True,
                                 timeout=15, check=True).stdout
            tree = ET.fromstring(xml)
        except (OSError, subprocess.SubprocessError, ET.ParseError) as exc:
            raise RuntimeError("Não consegui inspecionar a VM em execução {0}; operação VFIO bloqueada.".format(
                domain)) from exc
        for address in tree.findall("./devices/hostdev/source/address"):
            bdf = "{domain}:{bus}:{slot}.{function}".format(
                domain=address.get("domain", "0x0000").removeprefix("0x").zfill(4),
                bus=address.get("bus", "0x00").removeprefix("0x").zfill(2),
                slot=address.get("slot", "0x00").removeprefix("0x").zfill(2),
                function=address.get("function", "0x0").removeprefix("0x").zfill(1))
            if bdf in members:
                active.append(domain)
                break
    return active


def _active_manifest() -> Dict[str, object]:
    try:
        with open(ACTIVE_STATE, encoding="utf-8") as stream:
            manifest = json.load(stream)
    except (OSError, ValueError) as exc:
        raise RuntimeError("Não há configuração VFIO ativa.") from exc
    if not isinstance(manifest.get("members"), list) or not manifest["members"]:
        raise RuntimeError("O registro VFIO não contém dispositivos PCI válidos.")
    if any(not isinstance(bdf, str) or not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", bdf)
           for bdf in manifest["members"]):
        raise RuntimeError("O registro VFIO contém um endereço PCI inválido.")
    return manifest


def _virsh_device(bdf: str) -> str:
    return "pci_" + bdf.replace(":", "_").replace(".", "_")


def _current_driver(bdf: str) -> str:
    path = "/sys/bus/pci/devices/{}/driver".format(bdf)
    driver = os.path.basename(os.path.realpath(path))
    return "" if driver == "driver" or not os.path.exists(path) else driver


def bind_vfio_devices() -> str:
    """Desanexa os dispositivos reservados para a VM; libvirt reanexa no desligamento."""
    if os.geteuid() != 0:
        raise PermissionError("O bind VFIO precisa de privilégios administrativos.")
    members = _active_manifest()["members"]
    running = _active_vms_using(members)
    if running:
        raise RuntimeError("A VM já está usando esses dispositivos: " + ", ".join(running))
    changed = []
    try:
        for bdf in members:
            if _current_driver(bdf) == "vfio-pci":
                continue
            node = _virsh_device(bdf)
            result = subprocess.run(["virsh", "nodedev-detach", node, "--driver", "vfio-pci"],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode and "already detached" not in result.stderr.casefold():
                raise RuntimeError(result.stderr.strip() or "Não consegui associar " + bdf + " ao vfio-pci.")
            if result.returncode == 0:
                changed.append(node)
    except Exception:
        for node in reversed(changed):
            subprocess.run(["virsh", "nodedev-reattach", node], capture_output=True, timeout=30)
        raise
    return "Dispositivos preparados para a VM."


def unbind_vfio_devices() -> str:
    """Reanexa ao driver original e bloqueia a ação quando uma VM usa a GPU."""
    if os.geteuid() != 0:
        raise PermissionError("O unbind VFIO precisa de privilégios administrativos.")
    members = _active_manifest()["members"]
    running = _active_vms_using(members)
    if running:
        raise RuntimeError("Desligue a VM antes de devolver a GPU ao host: " + ", ".join(running))
    changed = []
    try:
        for bdf in reversed(members):
            if _current_driver(bdf) != "vfio-pci":
                continue
            node = _virsh_device(bdf)
            result = subprocess.run(["virsh", "nodedev-reattach", node], capture_output=True,
                                    text=True, timeout=30)
            if result.returncode and "not detached" not in result.stderr.casefold():
                raise RuntimeError(result.stderr.strip() or "Não consegui devolver " + bdf + " ao driver do host.")
            if result.returncode == 0:
                changed.append(node)
    except Exception:
        for node in reversed(changed):
            subprocess.run(["virsh", "nodedev-detach", node, "--driver", "vfio-pci"],
                           capture_output=True, timeout=30)
        raise
    return "GPU devolvida ao driver do host."


def apply_vfio(target: str, recovery_confirmed: bool = False) -> str:
    """Aplica parâmetros de IOMMU com backup e reversão; só é chamado pelo helper root."""
    if os.geteuid() != 0:
        raise PermissionError("A aplicação VFIO precisa ser executada pelo pkexec.")
    if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", target):
        raise ValueError("endereço PCI inválido")
    if os.path.exists(ACTIVE_STATE):
        raise RuntimeError("Já há uma configuração VFIO ativa. Reverta-a antes de aplicar outra.")
    if any(os.path.exists(path) for path in (MODPROBE_FILE, RECOVERY_SCRIPT, BIND_SCRIPT, UNBIND_SCRIPT)):
        raise RuntimeError("Já existe arquivo do Andrey VFIO; não vou sobrescrever uma configuração anterior.")

    state = inspect_system()
    check = preflight(state, target, recovery_confirmed)
    if not check["allowed"]:
        raise RuntimeError("Pré-verificação bloqueou a alteração:\n- " + "\n- ".join(check["blockers"]))
    vendor = _cpu_vendor()
    if not vendor:
        raise RuntimeError("Não identifiquei CPU Intel/AMD; nenhuma alteração foi feita.")
    if not os.path.isfile(GRUB_FILE) or not os.path.isfile(INITRAMFS_MODULES):
        raise RuntimeError("GRUB ou initramfs-tools não estão configurados; nenhuma alteração foi feita.")

    group = check["group"]
    params = ["intel_iommu=on" if vendor == "intel" else "amd_iommu=on", "iommu=pt"]
    cmdline = _read(GRUB_FILE)
    lines = cmdline.splitlines()
    matches = [index for index, line in enumerate(lines) if re.match(r"\s*GRUB_CMDLINE_LINUX_DEFAULT=", line)]
    if len(matches) > 1:
        raise RuntimeError("GRUB_CMDLINE_LINUX_DEFAULT duplicado; edite manualmente antes de prosseguir.")
    if matches:
        index = matches[0]
        key, raw = lines[index].split("=", 1)
        try:
            values = shlex.split(raw, comments=True)
        except ValueError as exc:
            raise RuntimeError("Linha GRUB_CMDLINE_LINUX_DEFAULT não pôde ser lida com segurança.") from exc
        if len(values) != 1:
            raise RuntimeError("Linha GRUB contém sintaxe não suportada; nenhuma alteração foi feita.")
        existing = values[0].split()
        if any(arg.startswith("vfio-pci.ids=") for arg in existing):
            raise RuntimeError("Já existe vfio-pci.ids no GRUB; não vou substituir essa configuração.")
        for parameter in params:
            key = parameter.split("=", 1)[0]
            conflicting = [arg for arg in existing if arg.split("=", 1)[0] == key and arg != parameter]
            if conflicting:
                raise RuntimeError("GRUB já define um parâmetro incompatível ({0}); ajuste-o manualmente antes de prosseguir."
                                   .format(", ".join(conflicting)))
        new_value = " ".join(existing + [arg for arg in params if arg not in existing])
        lines[index] = key + "=" + shlex.quote(new_value)
    else:
        lines.append("GRUB_CMDLINE_LINUX_DEFAULT=" + shlex.quote(" ".join(params)))

    grub_new = "\n".join(lines) + "\n"
    modules_old = _read(INITRAMFS_MODULES)
    modules_new = modules_old.rstrip() + "\n\n# Gerenciado pelo Andrey OS para VFIO\nvfio\nvfio_iommu_type1\nvfio_pci\n"
    os.makedirs(STATE_DIR, mode=0o700, exist_ok=True)
    folder = tempfile.mkdtemp(prefix="backup-", dir=STATE_DIR)
    os.chmod(folder, 0o700)
    paths = [GRUB_FILE, MODPROBE_FILE, INITRAMFS_MODULES, RECOVERY_SCRIPT, BIND_SCRIPT, UNBIND_SCRIPT]
    records = _backup_files(folder, paths)
    members = [d["pci"] for d in group["devices"]]
    manifest = {"backup_dir": folder, "files": records, "target": target, "members": members,
                "kernel_params": params, "created_by": "Andrey OS"}
    _atomic_write(ACTIVE_STATE, json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", 0o600)
    try:
        _atomic_write(GRUB_FILE, grub_new, os.stat(GRUB_FILE).st_mode & 0o777)
        _atomic_write(INITRAMFS_MODULES, modules_new, os.stat(INITRAMFS_MODULES).st_mode & 0o777)
        recovery_script = ("#!/bin/sh\nset -eu\n"
                           "if pkexec /usr/lib/andrey-os/vfio-helper revert; then\n"
                           "  if command -v zenity >/dev/null 2>&1 && zenity --question --title='Andrey OS VFIO' "
                           "--text='Backups restaurados. Reiniciar agora?'; then\n"
                           "    systemctl reboot\n"
                           "  else\n"
                           "    echo 'Backups restaurados. Execute reboot para carregar os drivers originais.'\n"
                           "  fi\nfi\n")
        _atomic_write(RECOVERY_SCRIPT, recovery_script, 0o755)
        _atomic_write(BIND_SCRIPT, "#!/bin/sh\nexec pkexec /usr/lib/andrey-os/vfio-helper bind\n", 0o755)
        _atomic_write(UNBIND_SCRIPT, "#!/bin/sh\nexec pkexec /usr/lib/andrey-os/vfio-helper unbind\n", 0o755)
        _run_system_update()
    except Exception as apply_error:
        try:
            _restore_files(records)
            _run_system_update()
        except Exception as rollback_error:
            # Keep the manifest and private backups so the recovery path remains
            # available if restoring boot files also fails.
            raise RuntimeError(
                "A aplicação falhou ({0}) e a reversão automática também falhou ({1}). "
                "Os backups foram preservados em {2}. Use andrey-vfio-revert após corrigir o erro."
                .format(apply_error, rollback_error, folder)
            ) from rollback_error
        os.unlink(ACTIVE_STATE)
        raise
    return "VFIO preparado. Reinicie para ativar. Backup: " + folder


def revert_vfio() -> str:
    """Restaura exatamente os arquivos anteriores e regenera GRUB/initramfs."""
    if os.geteuid() != 0:
        raise PermissionError("A reversão VFIO precisa ser executada pelo pkexec.")
    try:
        with open(ACTIVE_STATE, encoding="utf-8") as stream:
            manifest = json.load(stream)
    except (OSError, ValueError) as exc:
        raise RuntimeError("Não há estado VFIO ativo para reverter.") from exc
    records = manifest.get("files")
    if not isinstance(records, dict) or not records:
        raise RuntimeError("O registro de backup está incompleto; não alterei arquivos.")
    expected = {GRUB_FILE, MODPROBE_FILE, INITRAMFS_MODULES, RECOVERY_SCRIPT, BIND_SCRIPT, UNBIND_SCRIPT}
    if set(records) != expected:
        raise RuntimeError("O registro de backup não corresponde aos arquivos VFIO esperados.")
    _restore_files(records)
    _run_system_update()
    os.unlink(ACTIVE_STATE)
    return "Configuração VFIO removida e backups restaurados. Reinicie para os drivers originais voltarem."


def save_plan(text: str, env: Optional[dict] = None) -> str:
    env = env or os.environ
    base = env.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    path = os.path.join(base, "andrey-os", "anticheat", "vfio-plan.txt")
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    os.chmod(path, 0o600)
    return path


def list_vms() -> List[str]:
    try:
        result = subprocess.run(["virsh", "list", "--all", "--name"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()] if result.returncode == 0 else []


def vm_action(name: str, action: str) -> subprocess.CompletedProcess:
    if action not in ("start", "shutdown") or not name or name.startswith("-"):
        raise ValueError("ação ou nome de VM inválido")
    return subprocess.run(["virsh", action, name], capture_output=True, text=True, timeout=60)
