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


class BarClickTests(unittest.TestCase):
    """Ctrl+1..9 só valem depois de clicar na barra superior: a lógica de "onde caiu o clique"."""

    def setUp(self):
        self.t = load(TABS, "andrey_tabs_bar_under_test")
        self.top = (0, 0, 1280, 32)             # barra superior: expand=true, size=32
        self.dock = (440, 734, 400, 56)         # dock de baixo (flutuante, centralizado)
        self.h = 800

    def click(self, x, y, rects=None):
        return self.t.click_in_top_bar(x, y, [self.top, self.dock] if rects is None else rects, self.h)

    def test_clique_na_barra_de_cima_arma(self):
        for x, y in ((0, 0), (640, 16), (1279, 31)):
            self.assertTrue(self.click(x, y), (x, y))

    def test_clique_fora_da_barra_nao_arma(self):
        for x, y in ((640, 32), (640, 400), (10, 799), (1280, 10)):
            self.assertFalse(self.click(x, y), (x, y))

    def test_dock_de_baixo_nao_e_a_barra(self):
        self.assertFalse(self.click(640, 760))               # em cima do dock
        self.assertEqual(self.t.top_bar_rects([self.top, self.dock], self.h), [self.top])

    def test_sem_painel_achado_vale_a_faixa_de_cima(self):
        self.assertTrue(self.click(500, 10, rects=[]))
        self.assertFalse(self.click(500, 40, rects=[]))

    def test_estado_arma_e_desarma_conforme_os_cliques(self):
        st = self.t.BarState()
        self.assertFalse(st.armed)
        self.assertTrue(st.click(True))                      # clicou na barra: arma (mudou)
        self.assertTrue(st.armed)
        self.assertFalse(st.click(True))                     # clicou de novo na barra: continua armada
        self.assertTrue(st.armed)
        self.assertTrue(st.click(False))                     # clicou em outro lugar: solta as teclas
        self.assertFalse(st.armed)
        self.assertFalse(st.click(False))                    # clique fora de novo: nada muda
        self.assertFalse(st.armed)


class NotGlobalShortcutTests(unittest.TestCase):
    def test_ctrl_1_a_9_nao_sao_atalhos_globais(self):
        sys.path.insert(0, os.path.dirname(__file__))
        import test_keys
        k = test_keys.load_keys()
        self.assertEqual([s for s, _n, _b, _a in k.CUSTOM if s.startswith("andrey-tab-")], [])
        self.assertEqual(len(k.TAB_SLUGS), 9)                # só existem para limpar o que versões antigas criaram


class DaemonWiringTests(unittest.TestCase):
    def test_autostart_sobe_o_servico(self):
        f = os.path.join(ROOT, "etc", "xdg", "autostart", "andrey-tabs.desktop")
        txt = open(f, encoding="utf-8").read()
        self.assertIn("Exec=andrey-tabs --daemon", txt)
        self.assertIn("X-MATE-Autostart-enabled=true", txt)

    def test_session_setup_roda_a_limpeza_para_quem_ja_usava(self):
        txt = open(os.path.join(ROOT, "usr", "lib", "andrey-os", "session-setup"), encoding="utf-8").read()
        level = [l for l in txt.splitlines() if l.startswith("LEVEL=")][0]
        self.assertGreaterEqual(int(level.split("=")[1]), 7)  # quem estava no nível 6 tinha os atalhos globais


if __name__ == "__main__":
    unittest.main()
