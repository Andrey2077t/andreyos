# -*- coding: utf-8 -*-
"""Testes da detecção de hardware/drivers (sem root, sem rede).   Rode: python3 -m unittest discover -s tests"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

LIB = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root", "usr", "lib", "andrey-os")
sys.path.insert(0, LIB)
import andrey_drivers as d  # noqa: E402

NV_DETECT = """Detected NVIDIA GPUs:
01:00.0 VGA compatible controller [0300]: NVIDIA Corporation GP107 [GeForce GTX 1050 Ti] [10de:1c82] (rev a1)

Checking card:  NVIDIA Corporation GP107 [GeForce GTX 1050 Ti] (rev a1)
Your card is supported by all driver versions.
It is recommended to install the
    nvidia-driver
package.
"""
NV_LEGACY = "Your card is only supported by a legacy driver series.\nIt is recommended to install the\n    nvidia-legacy-390xx-driver\npackage.\n"


def dev(slot, vendor, device, cls, driver="x"):
    return {"slot": slot, "vendor": vendor, "device": device, "cls": cls, "driver": driver}


def plan(devs, cpu="GenuineIntel", virt="none", installed=(), avail=lambda p: True, nv=None, wifi=True):
    return d.build_plan(devs, cpu, virt, set(installed), avail, nv, wifi, {})


def ids(p):
    return [i["id"] for i in p]


class NvidiaDetectTests(unittest.TestCase):
    def test_driver_atual(self):
        self.assertEqual(d.parse_nvidia_detect(NV_DETECT), "nvidia-driver")

    def test_driver_legado(self):
        self.assertEqual(d.parse_nvidia_detect(NV_LEGACY), "nvidia-legacy-390xx-driver")

    def test_sem_recomendacao(self):
        self.assertIsNone(d.parse_nvidia_detect("No NVIDIA GPU detected."))
        self.assertIsNone(d.parse_nvidia_detect(""))

    def test_nome_estranho_e_ignorado(self):
        self.assertIsNone(d.parse_nvidia_detect("install the\n  $(rm -rf)\npackage"))


class PlanTests(unittest.TestCase):
    def test_intel_igpu_na_maquina_do_andrey(self):
        p = plan([dev("0000:00:02.0", "8086", "0152", "0300")])
        self.assertIn("gpu-intel", ids(p))
        gpu = next(i for i in p if i["id"] == "gpu-intel")
        self.assertIn("i965-va-driver", gpu["missing"])
        self.assertIn("intel-media-va-driver-non-free", gpu["missing"])
        self.assertEqual(next(i for i in p if i["id"] == "microcode")["packages"], ["intel-microcode"])

    def test_amd(self):
        p = plan([dev("0000:06:00.0", "1002", "73bf", "0300")], cpu="AuthenticAMD")
        self.assertIn("firmware-amd-graphics", next(i for i in p if i["id"] == "gpu-amd")["missing"])
        self.assertEqual(next(i for i in p if i["id"] == "microcode")["packages"], ["amd64-microcode"])

    def test_nvidia_usa_o_pacote_do_detect_e_dkms(self):
        p = plan([dev("0000:01:00.0", "10de", "1c82", "0300")], nv="nvidia-legacy-390xx-driver")
        it = next(i for i in p if i["id"] == "gpu-nvidia")
        self.assertIn("nvidia-legacy-390xx-driver", it["missing"])
        self.assertIn("linux-headers-amd64", it["missing"])
        self.assertTrue(it["dkms"] and it["reboot"])

    def test_nvidia_sem_detect_cai_no_padrao(self):
        it = next(i for i in plan([dev("0000:01:00.0", "10de", "1c82", "0302")]) if i["id"] == "gpu-nvidia")
        self.assertEqual(it["packages"][0], "nvidia-driver")

    def test_nvidia_traz_bibliotecas_de_32_bits_para_steam(self):
        it = next(i for i in plan([dev("0000:01:00.0", "10de", "1c82", "0300")]) if i["id"] == "gpu-nvidia")
        self.assertIn("nvidia-driver-libs-i386", it["missing"])
        self.assertEqual(d.nvidia_libs_i386("nvidia-driver"), "nvidia-driver-libs-i386")
        self.assertEqual(d.nvidia_libs_i386("nvidia-legacy-470xx-driver"), "nvidia-legacy-470xx-driver-libs-i386")

    def test_amd_e_intel_trazem_mesa_de_32_bits(self):
        for dv, gid in ((dev("0000:06:00.0", "1002", "73bf", "0300"), "gpu-amd"), (dev("0000:00:02.0", "8086", "0152", "0300"), "gpu-intel")):
            it = next(i for i in plan([dv]) if i["id"] == gid)
            for pk in ("libgl1-mesa-dri:i386", "mesa-vulkan-drivers:i386", "libvulkan1:i386"):
                self.assertIn(pk, it["missing"], (gid, pk))

    def test_i386_desabilitada_nao_pede_pacote_32_bits(self):
        it = next(i for i in plan([dev("0000:06:00.0", "1002", "73bf", "0300")], avail=lambda p: not p.endswith(":i386"))
                  if i["id"] == "gpu-amd")
        self.assertFalse([p for p in it["missing"] if p.endswith(":i386")])

    def test_notebook_hibrido_tem_dica(self):
        p = plan([dev("0000:00:02.0", "8086", "9a49", "0300"), dev("0000:01:00.0", "10de", "2560", "0302")])
        self.assertIn("gpu-intel", ids(p))
        self.assertIn("PRIME_RENDER_OFFLOAD", next(i for i in p if i["id"] == "gpu-nvidia")["note"])

    def test_ja_instalado_nao_falta_nada(self):
        devs = [dev("0000:01:00.0", "10de", "1c82", "0300")]
        p = plan(devs, installed=["nvidia-driver", "nvidia-driver-libs-i386", "linux-headers-amd64", "firmware-misc-nonfree", "intel-microcode",
                                  "firmware-linux", "firmware-sof-signed", "fwupd"])
        self.assertEqual([i["id"] for i in p if i["missing"]], [])
        self.assertEqual(d.fingerprint(p), "")

    def test_pacote_que_nao_existe_no_debian_e_ignorado(self):
        p = plan([dev("0000:00:02.0", "8086", "0152", "0300")], avail=lambda x: x != "firmware-intel-graphics")
        gpu = next(i for i in p if i["id"] == "gpu-intel")
        self.assertNotIn("firmware-intel-graphics", gpu["missing"])
        self.assertIn("firmware-intel-graphics", gpu["packages"])

    def test_broadcom_so_se_nao_ha_wifi_ativo(self):
        bc = [dev("0000:03:00.0", "14e4", "4353", "0280", driver=None)]
        self.assertNotIn("wifi-broadcom", ids(plan(bc, wifi=True)))
        self.assertIn("wifi-broadcom", ids(plan(bc, wifi=False)))

    def test_vm_nao_recebe_microcodigo_nem_driver_de_video(self):
        p = plan([dev("0000:00:02.0", "1234", "1111", "0300")], virt="kvm")
        self.assertEqual(ids(p), ["vm-guest"])
        self.assertEqual(p[0]["packages"], ["spice-vdagent", "qemu-guest-agent"])

    def test_vmware(self):
        self.assertEqual(plan([], virt="vmware")[0]["packages"], ["open-vm-tools-desktop"])

    def test_fingerprint_muda_quando_o_que_falta_muda(self):
        a = plan([dev("0000:00:02.0", "8086", "0152", "0300")])
        b = plan([dev("0000:00:02.0", "8086", "0152", "0300")], installed=["intel-microcode"])
        self.assertNotEqual(d.fingerprint(a), d.fingerprint(b))
        self.assertEqual(d.fingerprint(a), d.fingerprint(a))

    def test_newer_inclui_nvidia_so_com_placa_nvidia(self):
        self.assertNotIn("nvidia-driver", d.newer_packages(plan([dev("0000:00:02.0", "8086", "0152", "0300")])))
        n = plan([dev("0000:01:00.0", "10de", "1c82", "0300")])
        self.assertIn("nvidia-driver", d.newer_packages(n))

    def test_todos_os_pacotes_do_plano_passam_na_lista_do_helper(self):
        devs = [dev("0000:00:02.0", "8086", "0152", "0300"), dev("0000:01:00.0", "10de", "1c82", "0302"),
                dev("0000:06:00.0", "1002", "73bf", "0300"), dev("0000:03:00.0", "14e4", "4353", "0280")]
        pk = set(d.plan_packages(devs, "GenuineIntel", "none", "nvidia-legacy-470xx-driver", False))
        pk |= set(d.NEWER_BASE)
        for v in ("kvm", "vmware", "oracle", "microsoft"):
            pk |= set(d.plan_packages([], "", v))
        bad = [p for p in sorted(pk) if not HelperAllow.allows(p)]
        self.assertEqual(bad, [])


BT = [{"dev": "1-1", "vendor": "0a12", "product": "0001", "driver": "btusb"}]


class BluetoothTests(unittest.TestCase):
    def test_sem_adaptador_usb_nao_ha_item(self):
        self.assertNotIn("bluetooth", ids(d.build_plan([], "GenuineIntel", "none", set(), lambda p: True, bt_usb=[])))

    def test_adaptador_usb_gera_item_com_firmware(self):
        p = d.build_plan([], "GenuineIntel", "none", set(), lambda x: True, bt_usb=BT)
        it = next(i for i in p if i["id"] == "bluetooth")
        for pkg in ("bluez", "firmware-realtek", "firmware-atheros"):
            self.assertIn(pkg, it["missing"])
        self.assertEqual(it["note"], "")

    def test_sem_controlador_traz_dica_de_reparo(self):
        p = d.build_plan([], "GenuineIntel", "none", set(), lambda x: True, bt_usb=BT, bt_ctrl=False)
        self.assertIn("andrey-bluetooth --fix", next(i for i in p if i["id"] == "bluetooth")["note"])

    def test_pacotes_do_bluetooth_passam_na_lista_do_helper(self):
        bad = [p for p in d.BT_PACKAGES if not HelperAllow.allows(p)]
        self.assertEqual(bad, [])

    def test_ja_instalado_nao_falta_nada(self):
        p = d.build_plan([], "GenuineIntel", "none", set(d.BT_PACKAGES), lambda x: True, bt_usb=BT)
        self.assertEqual(next(i for i in p if i["id"] == "bluetooth")["missing"], [])

    def test_read_usb_bt_sysfs_falso(self):
        with tempfile.TemporaryDirectory() as root:
            def w(path, val):
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w") as f:
                    f.write(val + "\n")
            w(os.path.join(root, "1-1", "idVendor"), "0a12")
            w(os.path.join(root, "1-1", "idProduct"), "0001")
            for n in ("1-1:1.0", "1-1:1.1"):                      # adaptador: duas interfaces e0/01/01
                w(os.path.join(root, n, "bInterfaceClass"), "e0")
                w(os.path.join(root, n, "bInterfaceSubClass"), "01")
                w(os.path.join(root, n, "bInterfaceProtocol"), "01")
            os.symlink("/sys/bus/usb/drivers/btusb", os.path.join(root, "1-1:1.0", "driver"))
            w(os.path.join(root, "1-2", "idVendor"), "046d")      # mouse: nao e Bluetooth
            w(os.path.join(root, "1-2:1.0", "bInterfaceClass"), "03")
            got = d.read_usb_bt(root)
        self.assertEqual(got, [{"dev": "1-1", "vendor": "0a12", "product": "0001", "driver": "btusb"}])
        self.assertEqual(d.read_usb_bt("/nao/existe"), [])

    def test_has_bt_controller(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertFalse(d.has_bt_controller(root))
            os.makedirs(os.path.join(root, "hci0:256"))           # conexao, nao controlador
            self.assertFalse(d.has_bt_controller(root))
            os.makedirs(os.path.join(root, "hci0"))
            self.assertTrue(d.has_bt_controller(root))


class BluetoothHelperTests(unittest.TestCase):
    with open(os.path.join(LIB, "bluetooth-helper"), encoding="utf-8") as _f:
        func = re.search(r"^conf_set\(\) \{.*?^\}", _f.read(), re.S | re.M).group(0)

    def run_set(self, text, *args):
        with tempfile.NamedTemporaryFile("w+", suffix=".conf") as f:
            f.write(text)
            f.flush()
            subprocess.run(["bash", "-c", self.func + '\nconf_set "$1" "$2" "$3" "$4"', "x", *args, f.name], check=True, timeout=10)
            with open(f.name, encoding="utf-8") as g:    # sed -i troca o arquivo: le pelo caminho, nao pelo handle
                return g.read()

    def test_descomenta_e_troca(self):
        out = self.run_set("[General]\n#JustWorksRepairing = never\n", "General", "JustWorksRepairing", "always")
        self.assertIn("JustWorksRepairing = always", out)
        self.assertNotIn("#JustWorks", out)

    def test_cria_na_secao_se_nao_existir(self):
        out = self.run_set("[General]\nName = x\n[Policy]\n", "General", "ControllerMode", "bredr")
        self.assertEqual(out.splitlines()[:2], ["[General]", "ControllerMode = bredr"])

    def test_cria_a_secao_se_faltar(self):
        out = self.run_set("[General]\n", "Policy", "AutoEnable", "true")
        self.assertIn("[Policy]\nAutoEnable = true", out)

    def test_idempotente(self):
        a = self.run_set("[Policy]\n#AutoEnable=true\n", "Policy", "AutoEnable", "true")
        self.assertEqual(self.run_set(a, "Policy", "AutoEnable", "true"), a)


class ReadPciTests(unittest.TestCase):
    def test_sysfs_falso(self):
        with tempfile.TemporaryDirectory() as root:
            s = os.path.join(root, "0000:00:02.0")
            os.makedirs(s)
            for name, val in (("vendor", "0x8086"), ("device", "0x0152"), ("class", "0x030000")):
                with open(os.path.join(s, name), "w") as f:
                    f.write(val + "\n")
            os.symlink("/sys/bus/pci/drivers/i915", os.path.join(s, "driver"))
            s2 = os.path.join(root, "0000:03:00.0")
            os.makedirs(s2)
            for name, val in (("vendor", "0x14e4"), ("device", "0x4353"), ("class", "0x028000")):
                with open(os.path.join(s2, name), "w") as f:
                    f.write(val + "\n")
            devs = d.read_pci(root)
        self.assertEqual(devs[0], {"slot": "0000:00:02.0", "vendor": "8086", "device": "0152", "cls": "0300", "driver": "i915"})
        self.assertEqual((devs[1]["cls"], devs[1]["driver"]), ("0280", None))
        self.assertEqual([x["slot"] for x in d.unclaimed(devs)], ["0000:03:00.0"])

    def test_pasta_inexistente(self):
        self.assertEqual(d.read_pci("/nao/existe"), [])


class HelperAllow:
    """Roda o ok_pkg() de verdade (bash) extraído do drivers-helper."""
    with open(os.path.join(LIB, "drivers-helper"), encoding="utf-8") as _f:
        src = _f.read()
    func = re.search(r"^ok_pkg\(\) \{.*?^\}", src, re.S | re.M).group(0)

    @classmethod
    def allows(cls, pkg):
        r = subprocess.run(["bash", "-c", cls.func + '\nok_pkg "$1"', "x", pkg], capture_output=True, timeout=10)
        return r.returncode == 0


class HelperAllowlistTests(unittest.TestCase):
    def test_aceita(self):
        for p in ("nvidia-driver", "nvidia-legacy-390xx-driver", "firmware-amd-graphics", "intel-microcode",
                  "mesa-vulkan-drivers", "mesa-vulkan-drivers:i386", "libgl1-mesa-dri:i386", "linux-headers-amd64", "broadcom-sta-dkms", "spice-vdagent", "fwupd"):
            self.assertTrue(HelperAllow.allows(p), p)

    def test_recusa(self):
        for p in ("bash", "openssh-server", "netcat-traditional", "andrey-os-base", "-oAPT::Foo=1", "nvidia-x;rm",
                  "Firmware-linux", "", "bash:i386", "nvidia-driver:i386", "libgl1-mesa-dri:amd64", "firmware-linux nvidia-driver", "../etc/passwd"):
            self.assertFalse(HelperAllow.allows(p), repr(p))


if __name__ == "__main__":
    unittest.main()
