"""Andrey Music: atalho valido, launcher sem frescura de sintaxe e migracao que fixa o app no Menu Iniciar."""
import json
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")


def p(*parts):
    return os.path.join(ROOT, *parts)


class AndreyMusic(unittest.TestCase):
    def test_desktop_entry(self):
        with open(p("usr", "share", "applications", "andrey-music.desktop"), encoding="utf-8") as fh:
            kv = dict(l.rstrip("\n").split("=", 1) for l in fh if "=" in l)
        self.assertEqual(kv["Exec"], "andrey-music %U")
        self.assertEqual(kv["Icon"], "andrey-music")
        self.assertEqual(kv["StartupWMClass"], "AndreyMusic")
        self.assertNotIn("NoDisplay", kv)
        self.assertTrue(os.path.exists(p("usr", "share", "icons", "Andrey", "scalable", "apps", "andrey-music.svg")))

    def test_launcher_usa_o_endereco_e_perfil_proprios(self):
        with open(p("usr", "bin", "andrey-music"), encoding="utf-8") as fh:
            src = fh.read()
        self.assertIn("https://andreymusic.pages.dev/", src)
        self.assertIn('CLASS="AndreyMusic"', src)
        self.assertIn("/andrey-music", src)          # perfil separado do Andrey AI
        self.assertNotIn("andrey-ai", src)
        self.assertEqual(subprocess.run(["sh", "-n", p("usr", "bin", "andrey-music")]).returncode, 0)

    def _migrar(self, inicial):
        with tempfile.TemporaryDirectory() as home:
            d = os.path.join(home, "andrey-os", "start")
            os.makedirs(d)
            f = os.path.join(d, "pinned.json")
            if inicial is not None:
                with open(f, "w", encoding="utf-8") as fh:
                    json.dump(inicial, fh)
            env = dict(os.environ, XDG_CONFIG_HOME=home)
            for _ in range(2):   # idempotente: rodar duas vezes nao duplica
                subprocess.run(["bash", p("usr", "lib", "andrey-os", "migrations", "0.3.0.sh")], env=env, check=True)
            if inicial is None:
                self.assertFalse(os.path.exists(f))
                return None
            with open(f, encoding="utf-8") as fh:
                return json.load(fh)

    def test_migracao_poe_depois_do_andrey_ai(self):
        self.assertEqual(self._migrar(["x.desktop", "andrey-ai.desktop", "y.desktop"]),
                         ["x.desktop", "andrey-ai.desktop", "andrey-music.desktop", "y.desktop"])

    def test_migracao_sem_andrey_ai_poe_no_comeco(self):
        self.assertEqual(self._migrar(["x.desktop"]), ["andrey-music.desktop", "x.desktop"])

    def test_migracao_sem_lista_salva_nao_cria_nada(self):
        self.assertIsNone(self._migrar(None))


class SuporteAArquivos(unittest.TestCase):
    def test_pacotes_na_iso(self):
        with open(os.path.join(os.path.dirname(__file__), "..", "config", "package-lists", "andrey.list.chroot"),
                  encoding="utf-8") as fh:
            pkgs = {l.strip() for l in fh if l.strip() and not l.startswith("#")}
        for want in ("engrampa", "zip", "unzip", "7zip", "unrar", "xz-utils", "bzip2", "zstd"):
            self.assertIn(want, pkgs)

    def test_pacote_depende_do_engrampa(self):
        with open(os.path.join(ROOT, "..", "control"), encoding="utf-8") as fh:
            dep = next(l for l in fh if l.startswith("Depends:"))
        for want in ("engrampa", "zip", "unzip"):
            self.assertIn(want, dep)


if __name__ == "__main__":
    unittest.main()
