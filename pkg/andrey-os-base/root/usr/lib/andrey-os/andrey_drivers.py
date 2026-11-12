# -*- coding: utf-8 -*-
"""Detecção de hardware e plano de drivers do Andrey OS (lógica pura, sem root e sem GTK).

Usado pelo /usr/bin/andrey-drivers. Os testes (tests/test_drivers.py) cobrem tudo daqui.

Como funciona:
  1. read_pci()      lê /sys/bus/pci/devices (não depende do pciutils)
  2. build_plan()    decide quais pacotes cada peça de hardware precisa
  3. o que já está instalado ou não existe no Debian não entra em "missing"
"""
import glob
import hashlib
import os
import re
import shlex
import subprocess

V_NVIDIA, V_AMD, V_INTEL, V_BROADCOM = "10de", "1002", "8086", "14e4"
GPU_CLASSES = {"0300", "0302", "0380"}          # VGA, 3D, display
# placas de vídeo virtuais (QEMU, VMware, virtio, VirtualBox, QXL, Hyper-V): não têm driver para instalar
VIRTUAL_GPU_VENDORS = {"1234", "15ad", "1af4", "80ee", "1b36", "1414"}
NVIDIA_FALLBACK = "nvidia-driver"
HEADERS = "linux-headers-amd64"

# pacotes de "drivers mais novos" (backports do Debian); o que não existir lá fica na versão estável
NEWER_BASE = ["linux-image-amd64", HEADERS, "firmware-linux", "firmware-misc-nonfree",
              "libgl1-mesa-dri", "libglx-mesa0", "libegl-mesa0", "mesa-vulkan-drivers", "mesa-va-drivers"]

PKG_RE = re.compile(r"^[a-z0-9][a-z0-9+.-]*$")


# --------------------------------------------------------------------------- leitura do sistema
def _rd(path):
    try:
        with open(path, encoding="ascii", errors="ignore") as f:
            return f.read().strip()
    except OSError:
        return ""


def _hex(s, n=4):
    s = s.lower()
    return (s[2:] if s.startswith("0x") else s)[:n]


def read_pci(root="/sys/bus/pci/devices"):
    """Lista de dispositivos PCI: slot, vendor, device, cls (4 hex), driver (None se ninguém assumiu)."""
    devs = []
    try:
        slots = sorted(os.listdir(root))
    except OSError:
        return devs
    for slot in slots:
        base = os.path.join(root, slot)
        driver = None
        try:
            driver = os.path.basename(os.readlink(os.path.join(base, "driver")))
        except OSError:
            pass
        devs.append({"slot": slot, "vendor": _hex(_rd(base + "/vendor")), "device": _hex(_rd(base + "/device")),
                     "cls": _hex(_rd(base + "/class")), "driver": driver})
    return devs


def pci_names():
    """{slot: 'Fabricante Modelo'} via lspci (se não houver lspci, volta vazio e o código usa os IDs)."""
    try:
        out = subprocess.run(["lspci", "-D", "-mm"], capture_output=True, text=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    names = {}
    for ln in out.splitlines():
        try:
            f = shlex.split(ln)
        except ValueError:
            continue
        if len(f) >= 4:
            names[f[0]] = f"{f[2]} {f[3]}"
    return names


def cpu_vendor(path="/proc/cpuinfo"):
    for ln in _rd(path).splitlines() if os.path.exists(path) else []:
        if ln.startswith("vendor_id"):
            return ln.split(":", 1)[1].strip()
    return ""


def detect_virt():
    """'none' em máquina real; senão kvm, qemu, vmware, oracle, microsoft..."""
    try:
        out = subprocess.run(["systemd-detect-virt"], capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "none"
    return out or "none"


def has_wifi_iface(net="/sys/class/net"):
    try:
        return any(os.path.isdir(os.path.join(net, n, "wireless")) for n in os.listdir(net))
    except OSError:
        return False


def secure_boot_enabled():
    """True se o Secure Boot está ligado (módulos DKMS, como o da NVIDIA, precisam ser assinados)."""
    try:
        out = subprocess.run(["mokutil", "--sb-state"], capture_output=True, text=True, timeout=10).stdout
        if out:
            return "secureboot enabled" in out.lower()
    except (OSError, subprocess.SubprocessError):
        pass
    for f in glob.glob("/sys/firmware/efi/efivars/SecureBoot-*"):
        try:
            with open(f, "rb") as fh:
                return fh.read()[-1:] == b"\x01"
        except OSError:
            pass
    return False


def is_live(cmdline="/proc/cmdline"):
    return "boot=live" in _rd(cmdline).split()


def have_debian_lists(lists="/var/lib/apt/lists"):
    """False numa ISO limpa (sem índices do Debian): aí não dá para saber o que existe e o helper faz o apt update."""
    return bool(glob.glob(os.path.join(lists, "*_dists_*Packages*")))


def installed_set(pkgs):
    pkgs = sorted(set(pkgs))
    if not pkgs:
        return set()
    try:
        out = subprocess.run(["dpkg-query", "-W", "-f=${Package} ${db:Status-Abbrev}\n", *pkgs],
                             capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return set()
    return {p[0] for p in (ln.split() for ln in out.splitlines()) if len(p) >= 2 and p[1].startswith("ii")}


def available_func(pkgs):
    """Devolve available(pkg) -> bool: o Debian tem esse pacote? Sem índices, assume que sim (o helper confere)."""
    if not have_debian_lists():
        return lambda p: True
    pkgs = sorted(set(pkgs))
    found = set()
    try:
        out = subprocess.run(["apt-cache", "policy", *pkgs], capture_output=True, text=True, timeout=60).stdout
        cur = None
        for ln in out.splitlines():
            m = re.match(r"^(\S+):$", ln)
            if m:
                cur = m.group(1)
            elif cur and ln.strip().startswith("Candidate:") and "(none)" not in ln:
                found.add(cur)
    except (OSError, subprocess.SubprocessError):
        return lambda p: True
    return lambda p: p in found


def run_nvidia_detect():
    try:
        r = subprocess.run(["nvidia-detect"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    return r.stdout + r.stderr


def parse_nvidia_detect(text):
    """Pacote que o nvidia-detect recomenda (nvidia-driver, nvidia-legacy-470xx-driver...) ou None.

    A saída é assim: 'It is recommended to install the\\n    nvidia-driver\\npackage.'"""
    for m in re.finditer(r"install the\s+(\S+)\s+package", text or ""):
        if PKG_RE.match(m.group(1)):
            return m.group(1)
    return None


# --------------------------------------------------------------------------- o plano
def _item(iid, title, why, pkgs, installed, available, reboot=False, dkms=False, note=""):
    seen, uniq = set(), []
    for p in pkgs:
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    missing = [p for p in uniq if p not in installed and available(p)]
    return {"id": iid, "title": title, "why": why, "packages": uniq, "missing": missing,
            "reboot": reboot, "dkms": dkms, "note": note}


def plan_packages(devs, cpu, virt, nvidia_pkg=None, wifi_iface=True):
    """Todos os pacotes que o plano pode mencionar (para consultar dpkg/apt de uma vez só)."""
    return sorted({p for it in build_plan(devs, cpu, virt, set(), lambda p: True, nvidia_pkg, wifi_iface, {})
                   for p in it["packages"]})


def build_plan(devs, cpu, virt, installed, available, nvidia_pkg=None, wifi_iface=True, names=None):
    names = names or {}
    items = []
    bare = virt in ("none", "")

    def nm(d):
        return names.get(d["slot"]) or f"{d['vendor']}:{d['device']}"

    gpus = [d for d in devs if d["cls"] in GPU_CLASSES and d["vendor"] not in VIRTUAL_GPU_VENDORS]
    vendors = {d["vendor"] for d in gpus}
    for d in gpus:
        if d["vendor"] == V_NVIDIA:
            pkg = nvidia_pkg or NVIDIA_FALLBACK
            note = ""
            if vendors - {V_NVIDIA}:
                note = ("Notebook com duas placas de vídeo: a tela continua na integrada. Para rodar um programa "
                        "na NVIDIA: __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia programa")
            items.append(_item("gpu-nvidia", f"Placa de vídeo NVIDIA ({nm(d)})",
                               f"Driver proprietário {pkg} (3D, jogos e vídeo acelerado)",
                               [pkg, HEADERS, "firmware-misc-nonfree"], installed, available,
                               reboot=True, dkms=True, note=note))
        elif d["vendor"] == V_AMD:
            items.append(_item("gpu-amd", f"Placa de vídeo AMD ({nm(d)})",
                               "Driver amdgpu já vem no kernel; faltam firmware, Mesa (OpenGL/Vulkan) e aceleração de vídeo",
                               ["firmware-amd-graphics", "libgl1-mesa-dri", "mesa-vulkan-drivers",
                                "mesa-va-drivers", "vainfo"], installed, available, reboot=True))
        elif d["vendor"] == V_INTEL:
            items.append(_item("gpu-intel", f"Vídeo Intel ({nm(d)})",
                               "Driver i915/xe já vem no kernel; faltam firmware, Mesa e aceleração de vídeo",
                               ["firmware-intel-graphics", "libgl1-mesa-dri", "mesa-vulkan-drivers",
                                "intel-media-va-driver-non-free", "i965-va-driver", "vainfo"],
                               installed, available))

    brcm_wifi = [d for d in devs if d["vendor"] == V_BROADCOM and d["cls"] == "0280"]
    if brcm_wifi and not wifi_iface:
        items.append(_item("wifi-broadcom", f"Wi-Fi Broadcom sem driver ({nm(brcm_wifi[0])})",
                           "Nenhuma interface Wi-Fi ativa; instala firmware e o driver wl (precisa de internet por cabo ou USB)",
                           ["firmware-brcm80211", "broadcom-sta-dkms", HEADERS], installed, available,
                           reboot=True, dkms=True))

    if bare:
        if cpu == "GenuineIntel":
            items.append(_item("microcode", "Microcódigo do processador Intel",
                               "Correções de segurança e estabilidade da CPU", ["intel-microcode"], installed, available, reboot=True))
        elif cpu == "AuthenticAMD":
            items.append(_item("microcode", "Microcódigo do processador AMD",
                               "Correções de segurança e estabilidade da CPU", ["amd64-microcode"], installed, available, reboot=True))
        items.append(_item("firmware", "Firmware de Wi-Fi, rede, som e vídeo",
                           "Arquivos que placas e chips precisam para funcionar (non-free-firmware do Debian)",
                           ["firmware-linux", "firmware-misc-nonfree", "firmware-sof-signed"], installed, available, reboot=True))
        items.append(_item("fwupd", "Atualização de firmware (BIOS, SSD, periféricos)",
                           "Serviço fwupd, a mesma fonte (LVFS) que fabricantes como Dell, Lenovo e Logitech usam",
                           ["fwupd"], installed, available))
    else:
        vm = {"kvm": ["spice-vdagent", "qemu-guest-agent"], "qemu": ["spice-vdagent", "qemu-guest-agent"],
              "vmware": ["open-vm-tools-desktop"], "oracle": ["virtualbox-guest-x11"],
              "microsoft": ["hyperv-daemons"]}.get(virt)
        if vm:
            items.append(_item("vm-guest", f"Ferramentas de máquina virtual ({virt})",
                               "Tela com resolução automática, copiar/colar e mouse integrado", vm, installed, available))
    return [i for i in items]


def newer_packages(plan, nvidia_pkg=None):
    """Pacotes da opção 'drivers mais novos' (backports): base + o driver NVIDIA se houver placa NVIDIA."""
    pkgs = list(NEWER_BASE)
    if any(i["id"] == "gpu-nvidia" for i in plan):
        pkgs.append(nvidia_pkg or NVIDIA_FALLBACK)
    return pkgs


def unclaimed(devs):
    """Rede, vídeo e som (classes 02/03/04) que nenhum driver assumiu."""
    return [d for d in devs if d["driver"] is None and d["cls"][:2] in ("02", "03", "04")]


def fingerprint(plan):
    """Muda quando o que falta muda; o aviso automático só reaparece se isso mudar."""
    missing = sorted(p for i in plan for p in i["missing"])
    return hashlib.sha1("\n".join(missing).encode()).hexdigest()[:16] if missing else ""


def render_report(devs, names, plan, cpu, virt, sb, nvidia_pkg=None):
    out = ["== Hardware",
           f"CPU: {cpu or 'desconhecida'}   Virtualização: {virt}   Secure Boot: {'ligado' if sb else 'desligado'}"]
    for d in devs:
        if d["cls"][:2] in ("02", "03", "04", "0c") or d["cls"] in GPU_CLASSES:
            nm = names.get(d["slot"]) or f"{d['vendor']}:{d['device']}"
            out.append(f"  {d['slot'][5:] if d['slot'].startswith('0000:') else d['slot']}  {nm}  "
                       f"[driver: {d['driver'] or 'nenhum'}]")
    out.append("")
    out.append("== Drivers")
    for it in plan:
        state = "faltando: " + ", ".join(it["missing"]) if it["missing"] else "ok"
        out.append(f"  {it['title']}: {state}")
        if it["note"]:
            out.append(f"      dica: {it['note']}")
    un = unclaimed(devs)
    if un:
        out.append("")
        out.append("== Sem driver ativo")
        for d in un:
            out.append(f"  {names.get(d['slot']) or d['vendor'] + ':' + d['device']}")
    return "\n".join(out)
