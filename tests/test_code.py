"""Andrey Code: atalho valido, icone, associacoes de arquivos, integracao no pacote/ISO e no Menu Iniciar.

O editor em si (ui/andrey_code.cpp) e compilado e testado pelo tests/run.sh (`andrey-code --selftest`).
"""
import os
import re
import unittest

HERE = os.path.dirname(__file__)
TOP = os.path.join(HERE, "..")
ROOT = os.path.join(TOP, "pkg", "andrey-os-base", "root")


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


class AndreyCode(unittest.TestCase):
    def test_desktop_entry(self):
        kv = dict(l.rstrip("\n").split("=", 1) for l in read(ROOT, "usr/share/applications/andrey-code.desktop").splitlines(True) if "=" in l)
        self.assertEqual(kv["Exec"], "andrey-code %F")
        self.assertEqual(kv["Icon"], "andrey-code")
        self.assertEqual(kv["StartupWMClass"], "andrey-code")   # o mesmo nome que o app usa (setApplicationName)
        self.assertIn("TextEditor", kv["Categories"])
        self.assertIn("text/x-python", kv["MimeType"])
        self.assertNotIn("NoDisplay", kv)
        self.assertTrue(os.path.exists(os.path.join(ROOT, "usr/share/icons/Andrey/scalable/apps/andrey-code.svg")))

    def test_nome_do_app_no_codigo_bate_com_o_atalho(self):
        src = read(TOP, "ui", "andrey_code.cpp")
        self.assertIn('setApplicationName(QStringLiteral("andrey-code"))', src)
        self.assertIn('setDesktopFileName(QStringLiteral("andrey-code"))', src)
        self.assertIsNone(re.search(r"^\s*Q_OBJECT", src, re.M))   # compila com um g++ so, sem moc (igual aos outros apps Qt do projeto)

    def test_arquivos_de_codigo_abrem_no_andrey_code(self):
        m = read(ROOT, "etc/xdg/mimeapps.list")
        defaults = m.split("[Added Associations]")[0]
        for t in ("text/x-python", "text/x-csrc", "text/x-c++src", "application/json", "text/x-lua", "text/markdown"):
            self.assertIn(f"{t}=andrey-code.desktop;", defaults)
        # texto puro nao muda de programa padrao: so aparece nas associacoes extras
        self.assertNotIn("text/plain=andrey-code.desktop", defaults)
        self.assertIn("text/plain=andrey-code.desktop;", m)

    def test_build_do_pacote_compila_e_a_iso_confere(self):
        b = read(TOP, "pkg", "build-deb.sh")
        self.assertIn("ui/andrey_code.cpp", b)
        self.assertIn('"$STAGE/usr/bin/andrey-code"', b)
        self.assertIn("andrey-code.desktop", b)   # sem Qt (ANDREY_SKIP_QT) o atalho e removido junto
        hook = read(TOP, "config", "hooks", "live", "0050-andrey-base.hook.chroot")
        for f in ("/usr/bin/andrey-code", "/usr/share/applications/andrey-code.desktop", "andrey-code.svg"):
            self.assertIn(f, hook)

    def test_pacotes_da_iso(self):
        pkgs = {l.strip() for l in read(TOP, "config", "package-lists", "andrey.list.chroot").splitlines() if l.strip() and not l.startswith("#")}
        for want in ("fonts-dejavu-core", "python3-pyflakes", "git"):
            self.assertIn(want, pkgs)

    def test_aparece_fixado_no_menu_iniciar(self):
        src = read(ROOT, "usr/bin/andrey-start")
        m = re.search(r"DEFAULT_PINNED = \[(.*?)\]", src, re.S)
        self.assertIn('"andrey-code.desktop"', m.group(1))


if __name__ == "__main__":
    unittest.main()
