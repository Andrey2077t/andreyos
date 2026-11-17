"""Dock: fundo transparente, edicao dos itens (andrey-dock), integracao no Menu Iniciar e migracao do CSS antigo.

O teste do programa roda com o backend de gsettings "keyfile" numa pasta temporaria (nao mexe nas configuracoes de
ninguem) e so se os schemas do mate-panel estiverem instalados; sem eles, pula.
"""
import glob
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(__file__)
TOP = os.path.join(HERE, "..")
ROOT = os.path.join(TOP, "pkg", "andrey-os-base", "root")
DOCK = os.path.join(ROOT, "usr", "bin", "andrey-dock")


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


def schema_dir():
    for d in (os.environ.get("GSETTINGS_SCHEMA_DIR"), "/usr/share/glib-2.0/schemas"):
        if d and os.path.exists(os.path.join(d, "org.mate.panel.object.gschema.xml")):
            return d
    return None


class DockEstatico(unittest.TestCase):
    def test_css_nao_pinta_o_dock_de_escuro(self):
        css = read(ROOT, "etc/skel/.config/gtk-3.0/gtk.css")
        # a regra do fundo escuro nao pode pegar o painel inteiro (senao o dock nunca fica transparente)
        i = css.index("rgba(8, 2, 3, 0.88)")
        seletor = css[:i].rsplit("}", 1)[-1]
        self.assertNotIn("PanelToplevel", seletor)
        self.assertNotIn("#PanelWidget", seletor)
        self.assertIn(".mate-panel-menu-bar", seletor)

    def test_session_setup_aplica_o_estilo_uma_vez(self):
        s = read(ROOT, "usr/lib/andrey-os/session-setup")
        self.assertIn("LEVEL=10", s)
        self.assertIn("andrey-dock --apply-style", s)
        # depois do corte "ja aplicado", senao rodaria a cada login e pisaria na escolha da pessoa
        self.assertGreater(s.index("andrey-dock --apply-style"), s.index('-ge "$LEVEL"'))

    def test_atalho_e_menu_iniciar(self):
        kv = dict(l.rstrip("\n").split("=", 1) for l in read(ROOT, "usr/share/applications/andrey-dock.desktop").splitlines() if "=" in l)
        self.assertEqual(kv["Exec"], "andrey-dock")
        self.assertNotIn("NoDisplay", kv)
        self.assertIn("andrey-dock", read(ROOT, "usr/bin/andrey-start"))
        self.assertTrue(os.access(DOCK, os.X_OK) or os.name != "posix")

    def test_layout_tem_os_ids_que_o_editor_usa_no_padrao(self):
        layout = read(ROOT, "usr/share/mate-panel/layouts/andrey.layout")
        src = read(DOCK)
        for oid in ("bottom-grid", "bottom-browser", "bottom-terminal", "bottom-files", "bottom-settings", "bottom-separator",
                    "bottom-trash", "bottom-power"):
            self.assertIn("[Object %s]" % oid, layout)
            self.assertIn('"%s"' % oid, src)

    def test_migracao_do_css_antigo(self):
        old = (".mate-panel-menu-bar,\n.mate-panel-menu-bar menubar,\nPanelToplevel,\n#PanelWidget {\n"
               "    background-color: rgba(8, 2, 3, 0.88);\n}\nmeu-ajuste { color: red; }\n")
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "gtk-3.0"))
            f = os.path.join(d, "gtk-3.0", "gtk.css")
            with open(f, "w", encoding="utf-8") as fh:
                fh.write(old)
            env = dict(os.environ, XDG_CONFIG_HOME=d)
            for _ in range(2):   # idempotente
                subprocess.run(["bash", os.path.join(ROOT, "usr/lib/andrey-os/migrations/0.4.0.sh")], env=env, check=True)
            new = read(f)
            self.assertNotIn("PanelToplevel,", new)
            self.assertIn("meu-ajuste { color: red; }", new)   # o que a pessoa personalizou fica


@unittest.skipUnless(schema_dir() and shutil.which("python3") and shutil.which("glib-compile-schemas"),
                     "schemas do mate-panel nao instalados")
class DockPrograma(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.env = dict(os.environ, GSETTINGS_BACKEND="keyfile", XDG_CONFIG_HOME=self.tmp, HOME=self.tmp,
                        GSETTINGS_SCHEMA_DIR=schema_dir())
        self.env.pop("DISPLAY", None)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_dock(self, *args, ok=True):
        r = subprocess.run(["python3", DOCK, *args], env=self.env, capture_output=True, text=True, timeout=60)
        if ok:
            self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def items(self):
        return [l.split("\t") for l in self.run_dock("--list").stdout.splitlines()]

    def test_padrao_adicionar_remover(self):
        self.run_dock("--reset")
        base = self.items()
        self.assertEqual([t for t, _ in base][:5], ["launcher"] * 5)
        self.assertEqual(base[-1], ["action", "shutdown"])
        self.run_dock("--add", "andrey-code.desktop")
        it = self.items()
        self.assertIn(["launcher", "andrey-code.desktop"], it)
        # o app novo entra depois do ultimo lancador, antes do separador e da lixeira
        self.assertEqual(it.index(["launcher", "andrey-code.desktop"]), 5)
        self.assertEqual(it[6][0], "separator")
        self.assertIn("ja estava", self.run_dock("--add", "andrey-code.desktop").stdout)
        self.assertEqual(self.run_dock("--has", "andrey-code.desktop").returncode, 0)
        self.run_dock("--remove", "andrey-code.desktop")
        self.assertEqual(self.run_dock("--has", "andrey-code.desktop", ok=False).returncode, 1)
        self.assertEqual(self.items(), base)

    def test_fundo_transparente(self):
        self.run_dock("--apply-style")
        bg = os.path.join(self.tmp, "glib-2.0", "settings", "keyfile")
        txt = read(bg)
        self.assertIn("[org/mate/panel/toplevels/bottom/background]\ncolor='#000000'\nopacity=0\ntype='none'", txt)
        self.assertIn("[org/mate/panel/toplevels/top/background]\ncolor='#080203'\nopacity=57671", txt)
        self.run_dock("--transparent", "off")
        self.assertNotIn("bottom/background]\ncolor='#000000'\nopacity=0", read(bg))
        self.run_dock("--transparent", "on")
        self.assertIn("bottom/background]\ncolor='#000000'\nopacity=0\ntype='none'", read(bg))

    def test_posicoes_seguem_a_ordem_sem_sobrepor(self):
        self.run_dock("--reset")
        txt = read(os.path.join(self.tmp, "glib-2.0", "settings", "keyfile"))
        pos = [int(l.split("=")[1]) for l in txt.splitlines() if l.startswith("position=")]
        self.assertEqual(pos, sorted(set(pos)))


if __name__ == "__main__":
    unittest.main()
