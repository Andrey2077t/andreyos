# -*- coding: utf-8 -*-
"""Testes da lógica pura da busca do Menu Iniciar (sem GTK).   Rode: python3 -m unittest discover -s tests"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root", "usr", "lib", "andrey-os"))
import andrey_search as s  # noqa: E402


class CalcTests(unittest.TestCase):
    def test_contas_simples(self):
        self.assertEqual(s.calc("2+2"), "4")
        self.assertEqual(s.calc("3 x 4"), "12")
        self.assertEqual(s.calc("2^10"), "1024")
        self.assertEqual(s.calc("10÷4"), "2,5")
        self.assertEqual(s.calc("1,5*2"), "3")
        self.assertEqual(s.calc("(1+2)*3"), "9")
        self.assertEqual(s.calc("-3+5"), "2")

    def test_nao_e_conta(self):
        for t in ("7", "-5", "firefox", "", "   ", "(11) 99999-9999", "2+", "()", "1e5+1"):
            self.assertIsNone(s.calc(t), t)

    def test_erros_viram_none(self):
        self.assertIsNone(s.calc("10/0"))
        self.assertIsNone(s.calc("9**9**9"))
        self.assertIsNone(s.calc("(" * 500 + "1+1" + ")" * 500))

    def test_resultado_gigante_nao_estoura(self):
        # regressão: str() de inteiro com >4300 dígitos levantava ValueError e quebrava a busca
        self.assertIsNone(s.calc("999999**1000"))
        self.assertIsNone(s.calc("10**500"))
        self.assertEqual(s.calc("10**20"), str(10 ** 20))

    def test_nao_executa_codigo(self):
        for t in ("__import__('os')", "2+__import__('os').system('true')", "[1]+[2]"):
            self.assertIsNone(s.calc(t))


class ScoreTests(unittest.TestCase):
    @staticmethod
    def entry(name, extra=""):
        return {"lname": s.norm(name), "lextra": s.norm(extra)}

    def test_ordem_das_pontuacoes(self):
        e = self.entry("Mozilla Firefox", "navegador web")
        self.assertEqual(s.score_app(self.entry("Firefox"), "firefox"), 100)
        self.assertEqual(s.score_app(e, "mozilla"), 90)
        self.assertEqual(s.score_app(e, "fire"), 75)
        self.assertEqual(s.score_app(e, "refo"), 60)
        self.assertEqual(s.score_app(e, "navegador"), 30)
        self.assertEqual(s.score_app(e, "zzz"), 0)


class FilesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        h = self.tmp.name
        os.makedirs(os.path.join(h, "Documentos", "node_modules", "x"))
        os.makedirs(os.path.join(h, ".oculta"))
        for f in ("Documentos/relatorio.txt", "Documentos/node_modules/x/lixo.js", ".oculta/segredo.txt", "notas.md"):
            open(os.path.join(h, f), "w").close()

    def tearDown(self):
        self.tmp.cleanup()

    def test_indexa_sem_ocultos_nem_pastas_ignoradas(self):
        names = {n for n, _p, _d in s.index_home(home=self.tmp.name)}
        self.assertIn("relatorio.txt", names)
        self.assertIn("notas.md", names)
        self.assertNotIn("segredo.txt", names)
        self.assertNotIn("lixo.js", names)          # dentro de node_modules (não desce)

    def test_limite(self):
        self.assertLessEqual(len(s.index_home(limit=2, home=self.tmp.name)), 5)

    def test_match_files_ordena_por_prefixo(self):
        idx = s.index_home(home=self.tmp.name)
        hits = s.match_files(idx, "no")
        self.assertEqual(hits[0][0], "notas.md")     # começa com "no" vem antes de "node_modules"... e do resto
        self.assertEqual(s.match_files(idx, "n"), [])  # menos de 2 letras: não busca
        self.assertEqual(s.match_files([], "no"), [])


if __name__ == "__main__":
    unittest.main()
