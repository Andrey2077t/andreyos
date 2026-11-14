# -*- coding: utf-8 -*-
"""Testes do andrey-tabs (ordem das abas, agrupamento, escolha da janela) sem X/libwnck.
Rode: python3 -m unittest discover -s tests"""
import importlib.machinery
import importlib.util
import os
import sys
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")
TABS = os.path.join(ROOT, "usr", "bin", "andrey-tabs")
KEYS = os.path.join(ROOT, "usr", "bin", "andrey-keys")


def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


class TabsTests(unittest.TestCase):
    def setUp(self):
        self.t = load(TABS, "andrey_tabs_under_test")
        # ordem de abertura: chrome(1), terminal(2), chrome(3), caja(4), terminal(5)
        self.wins = [(1, "chrome"), (2, "term"), (3, "chrome"), (4, "caja"), (5, "term")]

    def test_sem_agrupar_cada_janela_e_uma_aba_e_o_mesmo_programa_fica_junto(self):
        self.assertEqual(self.t.build_tabs(self.wins, False), [[1], [3], [2], [5], [4]])

    def test_agrupado_cada_programa_e_uma_aba(self):
        self.assertEqual(self.t.build_tabs(self.wins, True), [[1, 3], [2, 5], [4]])

    def test_sem_janelas(self):
        self.assertEqual(self.t.build_tabs([], True), [])
        self.assertEqual(self.t.build_tabs([], False), [])

    def test_pick_janela_ativa_do_grupo_passa_para_a_proxima(self):
        self.assertEqual(self.t.pick_window([1, 3], 1, [1, 3]), 3)
        self.assertEqual(self.t.pick_window([1, 3], 3, [1, 3]), 1)          # volta ao comeco

    def test_pick_de_fora_do_grupo_abre_a_mais_a_frente(self):
        self.assertEqual(self.t.pick_window([1, 3], 4, [1, 4, 3]), 3)       # empilhamento: de tras para a frente
        self.assertEqual(self.t.pick_window([1, 3], None, [3, 1]), 1)

    def test_pick_aba_com_uma_janela(self):
        self.assertEqual(self.t.pick_window([4], 1, [4, 1]), 4)
        self.assertEqual(self.t.pick_window([4], 4, [4]), 4)                # ja ativa: continua nela

    def test_numero_da_aba(self):
        self.assertEqual([self.t.parse_n(x) for x in ("1", "9", "0", "10", "x", None)], [1, 9, None, None, None, None])

    def test_modos_de_agrupamento_batem_com_o_painel(self):
        self.assertEqual(self.t.MODES, {"on": "always", "off": "never", "auto": "auto"})


class KeysIntegrationTests(unittest.TestCase):
    def test_ctrl_1_a_9_estao_nos_atalhos_e_nao_sao_perigosos(self):
        sys.path.insert(0, os.path.dirname(__file__))
        import test_keys
        k = test_keys.load_keys()
        tabs = {slug: (b, a) for slug, _n, b, a in k.CUSTOM if slug.startswith("andrey-tab-")}
        self.assertEqual(len(tabs), 9)
        for i in range(1, 10):
            b, a = tabs["andrey-tab-%d" % i]
            self.assertEqual((b, a), ("<Control>%d" % i, "andrey-tabs %d" % i))
            self.assertFalse(k.is_dangerous(b, True))
            self.assertFalse(k.is_dangerous(b, False))          # nao dependem da Super


if __name__ == "__main__":
    unittest.main()
