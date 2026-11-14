import hashlib
import importlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")
sys.path.insert(0, os.path.join(ROOT, "usr", "lib", "andrey-os"))
g = importlib.import_module("andrey_games")

TAG = "GE-Proton10-1"
URL = g.PROTON_GE_PREFIX + TAG + "/"


def release(**over):
    r = {"tag_name": TAG, "assets": [
        {"name": TAG + ".tar.gz", "browser_download_url": URL + TAG + ".tar.gz"},
        {"name": TAG + ".sha512sum", "browser_download_url": URL + TAG + ".sha512sum"}]}
    r.update(over)
    return r


def make_tar(path, names):
    with tarfile.open(path, "w:gz") as tf:
        for n in names:
            data = b"x"
            ti = tarfile.TarInfo(n)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))


class ReleaseTests(unittest.TestCase):
    def test_release_valida(self):
        self.assertEqual(g.parse_release(release())[0:2], (TAG, TAG + ".tar.gz"))

    def test_recusa_tag_estranha(self):
        for t in ("../x", "GE-Proton1; rm -rf ~", "Proton-9", "", None, 5):
            self.assertIsNone(g.parse_release(release(tag_name=t)), repr(t))

    def test_recusa_url_de_fora_do_repositorio_oficial(self):
        r = release()
        r["assets"][0]["browser_download_url"] = "https://evil.example/" + TAG + ".tar.gz"
        self.assertIsNone(g.parse_release(r))

    def test_recusa_sem_checksum_ou_json_torto(self):
        r = release()
        r["assets"].pop()
        self.assertIsNone(g.parse_release(r))
        self.assertIsNone(g.parse_release({}))
        self.assertIsNone(g.parse_release(None))

    def test_checksum(self):
        h = "a" * 128
        self.assertEqual(g.parse_checksum(f"{h}  {TAG}.tar.gz\n", TAG + ".tar.gz"), h)
        self.assertEqual(g.parse_checksum(f"{h} *{TAG}.tar.gz", TAG + ".tar.gz"), h)
        self.assertIsNone(g.parse_checksum(f"{h}  outro.tar.gz", TAG + ".tar.gz"))
        self.assertIsNone(g.parse_checksum("zzz  " + TAG + ".tar.gz", TAG + ".tar.gz"))
        self.assertIsNone(g.parse_checksum("", "x"))


class ExtractTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))

    def test_extrai_pasta_esperada(self):
        t = os.path.join(self.tmp, "a.tar.gz")
        make_tar(t, [TAG + "/compatibilitytool.vdf", TAG + "/files/x"])
        out = os.path.join(self.tmp, "out")
        os.makedirs(out)
        g.safe_extract(t, out, TAG)
        self.assertTrue(os.path.isfile(os.path.join(out, TAG, "compatibilitytool.vdf")))

    def test_recusa_fuga_de_caminho_e_pasta_errada(self):
        for names in (["../fora"], [TAG + "/../../fora"], ["/etc/passwd"], ["outra/x"], [TAG + "/ok", "intruso"]):
            t = os.path.join(self.tmp, "b.tar.gz")
            make_tar(t, names)
            out = os.path.join(self.tmp, "o2")
            os.makedirs(out, exist_ok=True)
            with self.assertRaises(ValueError, msg=names):
                g.safe_extract(t, out, TAG)

    def test_installed_proton(self):
        home = self.tmp
        d = os.path.join(g.compat_dir(home), TAG)
        os.makedirs(d)
        self.assertEqual(g.installed_proton(home), [])
        open(os.path.join(d, "compatibilitytool.vdf"), "w").close()
        self.assertEqual(g.installed_proton(home), [TAG])


class DiagnoseTests(unittest.TestCase):
    def test_diagnose_devolve_tuplas_e_relatorio_funciona(self):
        res = g.diagnose(tempfile.mkdtemp())
        self.assertTrue(all(len(r) == 3 for r in res))
        rep = g.render_report(res)
        self.assertIn("Jogos no Andrey OS", rep)
        self.assertIn("Proton", rep)

    def test_pacotes_de_reparo_passam_no_apt_helper(self):
        helper = os.path.join(ROOT, "usr", "lib", "andrey-os", "apt-helper")
        func = [ln for ln in open(helper, encoding="utf-8") if ln.startswith("valid()")][0]
        for p in g.REPAIR_PACKAGES:
            r = subprocess.run(["bash", "-c", func + '\nvalid "$1"', "x", p], capture_output=True, timeout=10)
            self.assertEqual(r.returncode, 0, p)
        for bad in ("wine:arm64", "a;b", "-x", "Wine", "wine32:i386;id"):
            r = subprocess.run(["bash", "-c", func + '\nvalid "$1"', "x", bad], capture_output=True, timeout=10)
            self.assertNotEqual(r.returncode, 0, bad)


class SysctlTests(unittest.TestCase):
    def test_max_map_count_alto(self):
        f = os.path.join(ROOT, "usr", "lib", "sysctl.d", "80-andrey-games.conf")
        vals = [int(ln.split("=")[1]) for ln in open(f, encoding="utf-8") if ln.startswith("vm.max_map_count")]
        self.assertEqual(len(vals), 1)
        self.assertGreaterEqual(vals[0], g.MIN_MAP_COUNT)


if __name__ == "__main__":
    unittest.main()
