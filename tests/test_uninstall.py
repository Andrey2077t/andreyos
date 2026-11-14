import gzip
import importlib
import os
import sys
import tempfile
import time
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")
sys.path.insert(0, os.path.join(ROOT, "usr", "lib", "andrey-os"))
u = importlib.import_module("andrey_uninstall")

STATUS = """Package: vlc
Status: install ok installed
Priority: optional
Installed-Size: 73728
Version: 3.0.21-1
Description: multimedia player and streamer
 VLC is the VideoLAN project's media player. It plays MPEG, DVD and many
 other formats.
 .
 Second paragraph.

Package: andrey-os-base
Status: install ok installed
Installed-Size: 900
Version: 0.1.8
Description: base

Package: firefox-esr
Status: install ok installed
Installed-Size: 1153433
Version: 140.0
Description: Mozilla Firefox web browser - Extended Support Release

Package: bash
Status: install ok installed
Essential: yes
Priority: required
Installed-Size: 1500
Version: 5.2
Description: GNU Bourne Again SHell

Package: removido
Status: deinstall ok config-files
Installed-Size: 10
Version: 1
Description: x

Package: wine
Status: install ok installed
Installed-Size: 720896
Version: 10.0
Description: Windows API implementation
"""


def w(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


class Env(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.t, ignore_errors=True))
        self.apps = os.path.join(self.t, "apps")
        self.info = os.path.join(self.t, "info")
        self.status = os.path.join(self.t, "status")
        os.makedirs(self.info)
        os.makedirs(self.apps)
        w(self.status, STATUS)

    def desktop(self, name, body, pkg):
        p = os.path.join(self.apps, name)
        w(p, body)
        with open(os.path.join(self.info, pkg + ".list"), "a", encoding="utf-8") as fh:
            fh.write(p + "\n")
        return p


class StatusTests(Env):
    def test_so_instalados_e_descricao(self):
        s = u.parse_status(self.status)
        self.assertIn("vlc", s)
        self.assertNotIn("removido", s)
        sm, lg = u.split_desc(s["vlc"]["Description"])
        self.assertEqual(sm, "multimedia player and streamer")
        self.assertTrue(lg.startswith("VLC is the VideoLAN") and "Second" not in lg)

    def test_status_ausente(self):
        self.assertEqual(u.parse_status("/nao/existe"), {})

    def test_tamanho_formatado(self):
        self.assertEqual(u.fmt_size(73728), "72 MB")
        self.assertEqual(u.fmt_size(1153433), "1,1 GB")
        self.assertEqual(u.fmt_size(300), "300 KB")
        self.assertEqual(u.fmt_size("x"), "—")

    def test_shorten(self):
        self.assertEqual(u.shorten("curto"), "curto")
        r = u.shorten("palavra " * 30, 40)
        self.assertLessEqual(len(r), 41)
        self.assertTrue(r.endswith("…"))

    def test_protegidos(self):
        s = u.parse_status(self.status)
        self.assertTrue(u.is_protected("andrey-os-base"))
        self.assertTrue(u.is_protected("mate-desktop-environment-core"))
        self.assertTrue(u.is_protected("bash", s["bash"]))
        self.assertTrue(u.is_protected("qualquer", {"Essential": "yes"}))
        self.assertFalse(u.is_protected("vlc", s["vlc"]))
        self.assertFalse(u.is_protected("firefox-esr", s["firefox-esr"]))


class DesktopTests(Env):
    def test_parse_basico_e_pt_br(self):
        p = self.desktop("a.desktop", "[Desktop Entry]\nType=Application\nName=Player\nName[pt_BR]=Reprodutor\n"
                         "Comment=Toca\nIcon=vlc\nCategories=AudioVideo;Player;\n", "vlc")
        e = u.parse_desktop(p)
        self.assertEqual((e["name"], e["comment"], e["icon"]), ("Reprodutor", "Toca", "vlc"))
        self.assertIn("AudioVideo", e["categories"])

    def test_ignora_escondidos_e_outros_desktops(self):
        for i, body in enumerate(("NoDisplay=true", "Hidden=true", "Type=Link", "OnlyShowIn=KDE;", "NotShowIn=MATE;")):
            base = "[Desktop Entry]\nType=Application\nName=X\n"
            if body.startswith("Type="):
                base = "[Desktop Entry]\nName=X\n"
            p = self.desktop("x%d.desktop" % i, base + body + "\n", "vlc")
            self.assertIsNone(u.parse_desktop(p), body)
        p = self.desktop("m.desktop", "[Desktop Entry]\nType=Application\nName=X\nOnlyShowIn=MATE;GNOME;\n", "vlc")
        self.assertIsNotNone(u.parse_desktop(p))

    def test_ignora_acoes(self):
        p = self.desktop("ac.desktop", "[Desktop Entry]\nType=Application\nName=Real\n"
                         "[Desktop Action nova]\nName=Nova janela\n", "vlc")
        self.assertEqual(u.parse_desktop(p)["name"], "Real")

    def test_categorias(self):
        self.assertEqual(u.category_of(["Game", "Utility"]), "Jogos")
        self.assertEqual(u.category_of(["Network", "WebBrowser"]), "Internet")
        self.assertEqual(u.category_of(["AudioVideo"]), "Multimídia")
        self.assertEqual(u.category_of(["Office"]), "Escritório")
        self.assertEqual(u.category_of(["Utility"]), "Utilitários")
        self.assertEqual(u.category_of(["System"]), "Outros")
        self.assertEqual(u.category_of(["Game"], "wine"), "Outros")


class IconTests(Env):
    def test_resolve(self):
        ic = os.path.join(self.t, "icons")
        for rel in ("hicolor/48x48/apps/foo.png", "hicolor/256x256/apps/foo.png", "hicolor/16x16/apps/foo.png",
                    "hicolor/scalable/apps/bar.svg", "Adwaita/64x64/apps/baz.png"):
            w(os.path.join(ic, rel), "x")
        pm = os.path.join(self.t, "pix")
        w(os.path.join(pm, "qux.xpm"), "x")
        self.assertTrue(u.resolve_icon("foo", ic, pm).endswith("256x256/apps/foo.png"))
        self.assertTrue(u.resolve_icon("bar", ic, pm).endswith("bar.svg"))
        self.assertTrue(u.resolve_icon("baz", ic, pm).endswith("baz.png"))
        self.assertTrue(u.resolve_icon("qux", ic, pm).endswith("qux.xpm"))
        self.assertEqual(u.resolve_icon("nada", ic, pm), "")
        self.assertEqual(u.resolve_icon("../../etc/passwd", ic, pm), "")
        self.assertEqual(u.resolve_icon("", ic, pm), "")
        self.assertEqual(u.resolve_icon(os.path.join(ic, "hicolor/scalable/apps/bar.svg")),
                         os.path.join(ic, "hicolor/scalable/apps/bar.svg"))

    def test_svg_preferido_sobre_png(self):
        ic = os.path.join(self.t, "icons")
        w(os.path.join(ic, "hicolor/256x256/apps/z.png"), "x")
        w(os.path.join(ic, "hicolor/scalable/apps/z.svg"), "x")
        self.assertTrue(u.resolve_icon("z", ic, self.t).endswith("z.svg"))


class DateTests(Env):
    def test_primeira_instalacao_no_log_inclusive_gz(self):
        log = os.path.join(self.t, "dpkg.log")
        w(log, "2026-11-12 16:00:07 install steam-installer:amd64 <none> 1.0\n"
               "2026-11-13 01:00:00 install steam-installer:amd64 <none> 1.1\n"
               "2026-11-12 16:00:08 upgrade vlc:amd64 1 2\n")
        with gzip.open(log + ".2.gz", "wt") as fh:
            fh.write("2026-10-01 10:00:00 install vlc:amd64 <none> 3.0\n")
        d = u.install_dates(log)
        self.assertEqual(time.strftime("%Y-%m-%d", time.localtime(d["steam-installer"])), "2026-11-12")
        self.assertEqual(time.strftime("%Y-%m-%d", time.localtime(d["vlc"])), "2026-10-01")
        self.assertEqual(u.fmt_date(None), "—")


class ListTests(Env):
    def build(self):
        self.desktop("vlc.desktop", "[Desktop Entry]\nType=Application\nName=VLC media player\nComment=Reprodutor\n"
                     "Icon=vlc\nCategories=AudioVideo;Player;\n", "vlc")
        self.desktop("vlc-extra.desktop", "[Desktop Entry]\nType=Application\nName=VLC media player extra longo\n", "vlc")
        self.desktop("ff.desktop", "[Desktop Entry]\nType=Application\nName=Firefox\nCategories=Network;WebBrowser;\n",
                     "firefox-esr")
        self.desktop("andrey-store.desktop", "[Desktop Entry]\nType=Application\nName=Loja\n", "andrey-os-base")
        self.desktop("orf.desktop", "[Desktop Entry]\nType=Application\nName=Orfao\n", "pacote-nao-instalado")
        return u.list_apps(status=u.parse_status(self.status), owners=u.desktop_owners(self.info, [self.apps]),
                           dates={}, apps_dirs=[self.apps], info_dir=self.info,
                           icon_root=os.path.join(self.t, "ic"), pixmaps=os.path.join(self.t, "pm"))

    def test_lista_sem_protegidos_nem_orfaos_e_sem_duplicar(self):
        apps = self.build()
        self.assertEqual([a["pkg"] for a in apps], ["firefox-esr", "vlc"])
        vlc = apps[1]
        self.assertEqual(vlc["name"], "VLC media player")
        self.assertEqual((vlc["size"], vlc["version"], vlc["category"]), ("72 MB", "3.0.21-1", "Multimídia"))
        self.assertEqual(vlc["desc"], "Reprodutor")
        self.assertTrue(vlc["long"].startswith("VLC is the VideoLAN"))
        self.assertRegex(vlc["date"], r"^\d\d/\d\d/\d{4}$")
        self.assertEqual(apps[0]["size"], "1,1 GB")

    def test_dono_do_desktop_ignora_sufixo_de_arquitetura(self):
        w(os.path.join(self.info, "vlc:amd64.list"), os.path.join(self.apps, "z.desktop") + "\n")
        w(os.path.join(self.apps, "z.desktop"), "x")
        self.assertEqual(u.desktop_owners(self.info, [self.apps]).get(os.path.join(self.apps, "z.desktop")), "vlc")


class PlanTests(Env):
    def st(self):
        return u.parse_status(self.status)

    def test_remocao_simples(self):
        r = u.plan(["vlc"], self.st(), simulate=lambda p: ["vlc"])
        self.assertTrue(r["ok"])
        self.assertEqual((r["remove"], r["extra"], r["blocked"]), (["vlc"], [], []))

    def test_arrasta_outros_mas_permitido(self):
        r = u.plan(["wine"], self.st(), simulate=lambda p: ["wine", "firefox-esr"])
        self.assertTrue(r["ok"])
        self.assertEqual(r["extra"], ["firefox-esr"])

    def test_bloqueia_se_arrastar_o_sistema(self):
        r = u.plan(["vlc"], self.st(), simulate=lambda p: ["vlc", "andrey-os-base"])
        self.assertFalse(r["ok"])
        self.assertEqual(r["blocked"], ["andrey-os-base"])
        self.assertIn("sistema", r["error"])

    def test_bloqueia_essential_mesmo_fora_da_lista_fixa(self):
        r = u.plan(["vlc"], self.st(), simulate=lambda p: ["vlc", "bash"])
        self.assertFalse(r["ok"])

    def test_entrada_invalida_e_falha_do_apt(self):
        for bad in ([], ["-oAPT::x=1"], ["Vlc"], ["a b"], ["../x"]):
            self.assertFalse(u.plan(bad, self.st(), simulate=lambda p: p)["ok"], bad)
        r = u.plan(["vlc"], self.st(), simulate=lambda p: None)
        self.assertFalse(r["ok"])
        self.assertIn("simular", r["error"])

    def test_pacote_nao_instalado_e_ignorado(self):
        called = []
        r = u.plan(["libreoffice-l10n-pt-br"], self.st(), simulate=lambda p: called.append(p) or p)
        self.assertTrue(r["ok"])
        self.assertEqual(called, [])
        r = u.plan(["vlc", "nao-instalado"], self.st(), simulate=lambda p: called.append(p) or p)
        self.assertEqual(called, [["vlc"]])

    def test_simulate_parseia_remv(self):
        import subprocess
        orig = subprocess.run

        class R:
            returncode = 0
            stdout = "Reading...\nRemv vlc [3.0] (x)\nRemv libvlc5:amd64 [3.0]\nInst foo\n"
        subprocess.run = lambda *a, **k: R()
        try:
            self.assertEqual(u.simulate_remove(["vlc"]), ["vlc", "libvlc5"])
        finally:
            subprocess.run = orig


class CliTests(Env):
    def test_check_recusa_com_codigo_3_e_aceita_com_0(self):
        import contextlib
        import io
        orig = (u.simulate_remove, u.parse_status)
        u.parse_status = lambda path=u.DPKG_STATUS: {"vlc": {}}
        try:
            u.simulate_remove = lambda p: p + ["mate-panel"]
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(u.main(["check", "vlc"]), 3)
            u.simulate_remove = lambda p: p
            self.assertEqual(u.main(["check", "vlc"]), 0)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(u.main([]), 2)
        finally:
            u.simulate_remove, u.parse_status = orig


if __name__ == "__main__":
    unittest.main()
