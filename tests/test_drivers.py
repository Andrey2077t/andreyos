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

    def test_notebook_hibrido_tem_dica(self):
        p = plan([dev("0000:00:02.0", "8086", "9a49", "0300"), dev("0000:01:00.0", "10de", "2560", "0302")])
        self.assertIn("gpu-intel", ids(p))
        self.assertIn("PRIME_RENDER_OFFLOAD", next(i for i in p if i["id"] == "gpu-nvidia")["note"])

    def test_ja_instalado_nao_falta_nada(self):
        devs = [dev("0000:01:00.0", "10de", "1c82", "0300")]
        p = plan(devs, installed=["nvidia-driver", "linux-headers-amd64", "firmware-misc-nonfree", "intel-microcode",
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
                  "mesa-vulkan-drivers", "linux-headers-amd64", "broadcom-sta-dkms", "spice-vdagent", "fwupd"):
            self.assertTrue(HelperAllow.allows(p), p)

    def test_recusa(self):
        for p in ("bash", "openssh-server", "netcat-traditional", "andrey-os-base", "-oAPT::Foo=1", "nvidia-x;rm",
                  "Firmware-linux", "", "firmware-linux nvidia-driver", "../etc/passwd"):
            self.assertFalse(HelperAllow.allows(p), repr(p))


if __name__ == "__main__":
    unittest.main()
