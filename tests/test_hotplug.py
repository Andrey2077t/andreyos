# -*- coding: utf-8 -*-
"""Coisas novas (app, microfone, fone) tem que aparecer na hora, sem reiniciar o PC."""
import os
import re
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PKG = os.path.join(ROOT, "pkg", "cisma-os-base")


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


class MenuAppsTest(unittest.TestCase):
    def setUp(self):
        self.src = read(PKG, "root", "usr", "bin", "cisma-start")

    def test_monitor_fica_guardado(self):
        # Gio.AppInfoMonitor.get().connect(...) sem guardar o objeto: o GLib destroi o monitor e o sinal
        # "changed" nunca chega (app novo so aparecia depois de reiniciar).
        self.assertNotRegex(self.src, r"Gio\.AppInfoMonitor\.get\(\)\.connect")
        self.assertRegex(self.src, r"self\._app_monitor\s*=\s*Gio\.AppInfoMonitor\.get\(\)")

    def test_menu_confere_as_pastas_de_desktop(self):
        self.assertIn("_apps_signature", self.src)
        self.assertIn("get_system_data_dirs", self.src)


class AudioHotplugTest(unittest.TestCase):
    def test_dropin_do_pulseaudio(self):
        s = read(PKG, "root", "etc", "pulse", "default.pa.d", "50-cisma-hotplug.pa")
        self.assertRegex(s, r"(?m)^load-module module-switch-on-connect\s*$")

    def test_dependencias(self):
        control = read(PKG, "control")
        lista = read(ROOT, "config", "package-lists", "cisma.list.chroot")
        for pkg in ("desktop-file-utils", "shared-mime-info", "libgtk-3-bin", "udisks2", "gvfs"):
            self.assertRegex(control, r"[ ,]%s[ ,]" % re.escape(pkg), pkg)
            self.assertRegex(lista, r"(?m)^%s\s*$" % re.escape(pkg), pkg)


if __name__ == "__main__":
    unittest.main()


class AudioHealTest(unittest.TestCase):
    """cisma-audio-heal: placa so em saida (jack do microfone nao detectado) ganha o perfil duplex."""

    @classmethod
    def setUpClass(cls):
        import importlib.machinery
        import importlib.util
        path = os.path.join(PKG, "root", "usr", "bin", "cisma-audio-heal")
        loader = importlib.machinery.SourceFileLoader("cisma_audio_heal_under_test", path)
        spec = importlib.util.spec_from_loader(loader.name, loader)
        cls.m = importlib.util.module_from_spec(spec)
        loader.exec_module(cls.m)

    @staticmethod
    def pch(active="output:analog-stereo"):
        # a placa real do Cisma (Realtek ALC662): entrada "indisponivel" por falta de deteccao do jack
        return {"name": "alsa_card.pci-0000_00_1b.0", "active_profile": active, "profiles": {
            "input:analog-stereo": {"sinks": 0, "sources": 1, "priority": 65, "available": False},
            "output:analog-stereo": {"sinks": 1, "sources": 0, "priority": 39268, "available": True},
            "output:analog-stereo+input:analog-stereo": {"sinks": 1, "sources": 1, "priority": 6565, "available": True},
            "off": {"sinks": 0, "sources": 0, "priority": 0, "available": True}}}

    @staticmethod
    def hdmi():
        return {"name": "alsa_card.pci-0000_01_00.1", "active_profile": "output:hdmi-stereo-extra3", "profiles": {
            "output:hdmi-stereo-extra3": {"sinks": 1, "sources": 0, "priority": 5, "available": True}}}

    def test_placa_so_de_saida_vai_para_duplex(self):
        self.assertEqual(self.m.plan([self.hdmi(), self.pch()], set()),
                         [("alsa_card.pci-0000_00_1b.0", "output:analog-stereo+input:analog-stereo")])

    def test_ja_duplex_ou_ja_tratada_ou_hdmi_nao_mexe(self):
        self.assertEqual(self.m.plan([self.pch("output:analog-stereo+input:analog-stereo")], set()), [])
        self.assertEqual(self.m.plan([self.pch()], {"alsa_card.pci-0000_00_1b.0"}), [])
        self.assertEqual(self.m.plan([self.hdmi()], set()), [])

    def test_json_estranho_nao_quebra(self):
        self.assertEqual(self.m.plan([{"name": "alsa_card.x", "active_profile": "output:a", "profiles": []}, {}], set()), [])
