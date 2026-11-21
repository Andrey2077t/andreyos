"""Cisma Code: atalho valido, icone, associacoes de arquivos, integracao no pacote/ISO e no Menu Iniciar.

O editor em si (ui/cisma_code.cpp) e compilado e testado pelo tests/run.sh (`cisma-code --selftest`).
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(__file__)
TOP = os.path.join(HERE, "..")
ROOT = os.path.join(TOP, "pkg", "cisma-os-base", "root")


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as fh:
        return fh.read()


class CismaCode(unittest.TestCase):
    def test_desktop_entry(self):
        kv = dict(l.rstrip("\n").split("=", 1) for l in read(ROOT, "usr/share/applications/cisma-code.desktop").splitlines(True) if "=" in l)
        self.assertEqual(kv["Exec"], "cisma-code %F")
        self.assertEqual(kv["Icon"], "cisma-code")
        self.assertEqual(kv["StartupWMClass"], "cisma-code")   # o mesmo nome que o app usa (setApplicationName)
        self.assertIn("TextEditor", kv["Categories"])
        self.assertIn("text/x-python", kv["MimeType"])
        self.assertNotIn("NoDisplay", kv)
        self.assertTrue(os.path.exists(os.path.join(ROOT, "usr/share/icons/Cisma/scalable/apps/cisma-code.svg")))

    def test_nome_do_app_no_codigo_bate_com_o_atalho(self):
        src = read(TOP, "ui", "cisma_code.cpp")
        self.assertIn('setApplicationName(QStringLiteral("cisma-code"))', src)
        self.assertIn('setDesktopFileName(QStringLiteral("cisma-code"))', src)
        self.assertIsNone(re.search(r"^\s*Q_OBJECT", src, re.M))   # compila com um g++ so, sem moc (igual aos outros apps Qt do projeto)

    def test_arquivos_de_codigo_abrem_no_cisma_code(self):
        m = read(ROOT, "etc/xdg/mimeapps.list")
        defaults = m.split("[Added Associations]")[0]
        for t in ("text/x-python", "text/x-csrc", "text/x-c++src", "application/json", "text/x-lua", "text/markdown"):
            self.assertIn(f"{t}=cisma-code.desktop;", defaults)
        # texto puro nao muda de programa padrao: so aparece nas associacoes extras
        self.assertNotIn("text/plain=cisma-code.desktop", defaults)
        self.assertIn("text/plain=cisma-code.desktop;", m)

    def test_build_do_pacote_compila_e_a_iso_confere(self):
        b = read(TOP, "pkg", "build-deb.sh")
        self.assertIn("ui/cisma_code.cpp", b)
        self.assertIn('"$STAGE/usr/bin/cisma-code"', b)
        self.assertIn("cisma-code.desktop", b)   # sem Qt (CISMA_SKIP_QT) o atalho e removido junto
        hook = read(TOP, "config", "hooks", "live", "0050-cisma-base.hook.chroot")
        for f in ("/usr/bin/cisma-code", "/usr/share/applications/cisma-code.desktop", "cisma-code.svg"):
            self.assertIn(f, hook)

    def test_pacotes_da_iso(self):
        pkgs = {l.strip() for l in read(TOP, "config", "package-lists", "cisma.list.chroot").splitlines() if l.strip() and not l.startswith("#")}
        for want in ("fonts-dejavu-core", "python3-pyflakes", "git"):
            self.assertIn(want, pkgs)

    def test_aparece_fixado_no_menu_iniciar(self):
        src = read(ROOT, "usr/bin/cisma-start")
        m = re.search(r"DEFAULT_PINNED = \[(.*?)\]", src, re.S)
        self.assertIn('"cisma-code.desktop"', m.group(1))


if __name__ == "__main__":
    unittest.main()

class DepuradorPython(unittest.TestCase):
    """O adaptador de depuracao vai embutido no cisma_code.cpp; aqui ele e extraido e conduzido como o editor faz."""

    def adapter(self):
        m = re.search(r'kDbgAdapter = R"PYDBG\((.*?)\)PYDBG";', read(TOP, "ui", "cisma_code.cpp"), re.S)
        self.assertIsNotNone(m, "adaptador do depurador nao encontrado no cisma_code.cpp")
        return m.group(1)

    def test_pontos_de_parada_passos_variaveis_e_excecao(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "cisma_dbg.py"), "w", encoding="utf-8") as fh:
                fh.write(self.adapter())
            prog = os.path.realpath(os.path.join(d, "prog.py"))
            with open(prog, "w", encoding="utf-8") as fh:
                fh.write("def soma(a, b):\n    r = a + b\n    return r\n\ntotal = 0\nfor i in range(3):\n"
                         "    total = soma(total, i)\nprint('total', total)\nx = {'k': [1, 2, 3]}\nraise ValueError('boom')\n")
            sockp = os.path.join(d, "s")
            srv = socket.socket(socket.AF_UNIX)
            srv.bind(sockp)
            srv.listen(1)
            srv.settimeout(20)
            boot = "import sys;sys.path.insert(0,%r);import cisma_dbg;sys.path.pop(0);cisma_dbg.main()" % d
            p = subprocess.Popen([sys.executable, "-u", "-c", boot, sockp, prog], cwd=d, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
            try:
                c, _ = srv.accept()
                c.settimeout(20)
                rf = c.makefile("r")
                rd = lambda: json.loads(rf.readline())
                wr = lambda **o: c.sendall((json.dumps(o) + "\n").encode())
                self.assertEqual(rd()["ev"], "hello")
                wr(cmd="go", bps={prog: [2]}, entry=False)
                m = rd()
                self.assertEqual((m["reason"], m["frames"][0][1], m["frames"][0][2]), ("breakpoint", 2, "soma"))
                self.assertEqual([v[2] for v in m["locals"] if v[0] == "a"], ["0"])
                self.assertEqual(m["frames"][1][2], "<module>")
                wr(cmd="eval", expr="a + 100", frame=0)
                self.assertEqual(rd()["out"], "100")
                wr(cmd="vars", frame=1)
                self.assertTrue(any(v[0] == "total" for v in rd()["locals"]))
                wr(cmd="next")
                self.assertEqual(rd()["frames"][0][1], 3)
                wr(cmd="out")
                self.assertEqual(rd()["frames"][0][2], "<module>")
                wr(cmd="setbp", bps={})
                wr(cmd="continue")
                m = rd()
                self.assertTrue(m["reason"].startswith("exce"), m["reason"])   # parada na excecao nao tratada
                wr(cmd="eval", expr="x['k']", frame=0)
                self.assertEqual(rd()["out"], "[1, 2, 3]")
                wr(cmd="continue")
                self.assertEqual(rd()["ev"], "exit")
                out = p.communicate(timeout=20)[0]
                self.assertIn("total 3", out)
                self.assertIn("ValueError: boom", out)
                self.assertNotIn("bdb.py", out)   # o traceback nao mostra o miolo do depurador
                self.assertEqual(p.returncode, 1)
            finally:
                if p.poll() is None:
                    p.kill()
                srv.close()

    def test_sem_ponto_de_parada_para_na_primeira_linha(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "cisma_dbg.py"), "w", encoding="utf-8") as fh:
                fh.write(self.adapter())
            prog = os.path.realpath(os.path.join(d, "p.py"))
            with open(prog, "w", encoding="utf-8") as fh:
                fh.write("a = 1\nb = 2\n")
            sockp = os.path.join(d, "s")
            srv = socket.socket(socket.AF_UNIX)
            srv.bind(sockp)
            srv.listen(1)
            srv.settimeout(20)
            boot = "import sys;sys.path.insert(0,%r);import cisma_dbg;sys.path.pop(0);cisma_dbg.main()" % d
            p = subprocess.Popen([sys.executable, "-u", "-c", boot, sockp, prog], cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                c, _ = srv.accept()
                c.settimeout(20)
                rf = c.makefile("r")
                rf.readline()
                c.sendall((json.dumps({"cmd": "go", "bps": {}, "entry": True}) + "\n").encode())
                m = json.loads(rf.readline())
                self.assertEqual((m["reason"], m["frames"][0][1]), ("entry", 1))
                c.sendall((json.dumps({"cmd": "continue"}) + "\n").encode())
                self.assertEqual(json.loads(rf.readline())["ev"], "exit")
                p.wait(timeout=20)
                self.assertEqual(p.returncode, 0)
            finally:
                if p.poll() is None:
                    p.kill()
                srv.close()


if __name__ == "__main__":
    unittest.main()
