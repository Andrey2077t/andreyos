# -*- coding: utf-8 -*-
"""Testes do andrey-keys.apply() sem GTK/MATE (Gio e Gdk trocados por dublês).   Rode: python3 -m unittest discover -s tests"""
import importlib.machinery
import importlib.util
import os
import sys
import types
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")
KEYS = os.path.join(ROOT, "usr", "bin", "andrey-keys")


class FakeSettings:
    store = {}                                   # caminho -> {chave: valor}

    def __init__(self, path):
        self.d = FakeSettings.store.setdefault(path, {})

    @classmethod
    def new_with_path(cls, _schema, path):
        return cls(path)

    @classmethod
    def sync(cls):
        pass

    def get_string(self, k):
        return self.d.get(k, "")

    def set_string(self, k, v):
        self.d[k] = v

    def reset(self, k):
        self.d.pop(k, None)


def load_keys():
    """Carrega o script (sem .py) com um 'gi' falso, só para testar a lógica."""
    gi = types.ModuleType("gi")
    gi.require_version = lambda *_a, **_k: None
    rep = types.ModuleType("gi.repository")
    schema_src = types.SimpleNamespace(lookup=lambda *_a: True)
    rep.Gio = types.SimpleNamespace(Settings=FakeSettings,
                                    SettingsSchemaSource=types.SimpleNamespace(get_default=lambda: schema_src))
    rep.Gdk = types.SimpleNamespace()
    rep.Gtk = types.SimpleNamespace()
    gi.repository = rep
    saved = {k: sys.modules.get(k) for k in ("gi", "gi.repository")}
    sys.modules["gi"], sys.modules["gi.repository"] = gi, rep
    try:
        loader = importlib.machinery.SourceFileLoader("andrey_keys_under_test", KEYS)
        spec = importlib.util.spec_from_loader(loader.name, loader)
        mod = importlib.util.module_from_spec(spec)
        loader.exec_module(mod)
    finally:
        for k, v in saved.items():
            if v is None:
                sys.modules.pop(k, None)
            else:
                sys.modules[k] = v
    return mod


class ApplyTests(unittest.TestCase):
    def setUp(self):
        FakeSettings.store = {}
        self.k = load_keys()
        self.k.slugs = lambda: [p.rstrip("/").rsplit("/", 1)[-1] for p in FakeSettings.store]

    def binding(self, slug):
        return FakeSettings.store.get(self.k.BASE + slug + "/", {}).get("binding", "")

    def test_com_super_cria_tudo(self):
        self.k.super_ok = lambda: True
        self.assertEqual(self.k.apply(), 0)
        for slug, _n, b, _a in self.k.CUSTOM:
            self.assertEqual(self.binding(slug), b, slug)

    def test_sem_super_ainda_cria_os_atalhos_sem_super(self):
        # regressão: com a tecla Super sem modificador o apply() saía sem criar nada, e como o gschema
        # desliga o tratamento de volume do MATE, as teclas de volume ficavam mortas
        self.k.super_ok = lambda: False
        self.assertEqual(self.k.apply(), 0)
        for slug, _n, b, _a in self.k.CUSTOM:
            if b.startswith("<Super>"):
                self.assertEqual(self.binding(slug), "", slug)      # estes travariam a tecla
            else:
                self.assertEqual(self.binding(slug), b, slug)       # estes não dependem do Super
        self.assertEqual(self.binding("andrey-vol-up"), "XF86AudioRaiseVolume")

    def test_sem_super_limpa_atalhos_com_super_que_ja_existiam(self):
        self.k.super_ok = lambda: True
        self.k.apply()
        self.k.super_ok = lambda: False
        self.k.apply()
        self.assertEqual(self.binding("andrey-files"), "")
        self.assertEqual(self.binding("andrey-vol-down"), "XF86AudioLowerVolume")

    def test_nao_pisa_no_atalho_personalizado(self):
        self.k.super_ok = lambda: True
        FakeSettings.store[self.k.BASE + "andrey-files/"] = {"binding": "<Control>e"}
        self.k.apply()
        self.assertEqual(self.binding("andrey-files"), "<Control>e")
        self.k.apply(force=True)
        self.assertEqual(self.binding("andrey-files"), "<Super>e")


class TabShortcutsTests(unittest.TestCase):
    """Ctrl+1..9 não são mais atalhos globais (engoliam o Ctrl+1..9 do navegador): quem tinha os antigos perde."""

    def setUp(self):
        FakeSettings.store = {}
        self.k = load_keys()
        self.k.slugs = lambda: [p.rstrip("/").rsplit("/", 1)[-1] for p in FakeSettings.store]
        self.k.super_ok = lambda: True

    def old_tab(self, i, action=None):
        FakeSettings.store[self.k.BASE + "andrey-tab-%d/" % i] = {
            "name": "Ir para a aba %d" % i, "binding": "<Control>%d" % i, "action": action or "andrey-tabs %d" % i}

    def entry(self, i):
        return FakeSettings.store.get(self.k.BASE + "andrey-tab-%d/" % i, {})

    def test_apply_nao_cria_ctrl_numero(self):
        self.k.apply()
        self.assertFalse([s for s, d in FakeSettings.store.items() if "andrey-tab-" in s and d])   # nada gravado
        self.assertFalse([b for _s, _n, b, _a in self.k.CUSTOM if b.startswith("<Control>") and b[-1].isdigit()])

    def test_apply_remove_os_atalhos_globais_antigos(self):
        for i in range(1, 10):
            self.old_tab(i)
        self.k.apply()
        for i in range(1, 10):
            self.assertEqual(self.entry(i), {}, i)           # name, binding e action voltaram ao padrão (vazio)

    def test_reset_tambem_remove(self):
        self.old_tab(4)
        self.k.apply(force=True)
        self.assertEqual(self.entry(4), {})

    def test_nao_mexe_no_que_voce_trocou(self):
        self.old_tab(2, action="meu-script --aba 2")          # personalizado: não chama o andrey-tabs
        self.old_tab(3)
        self.assertEqual(self.k.drop_tab_shortcuts(), 1)
        self.assertEqual(self.entry(2)["action"], "meu-script --aba 2")
        self.assertEqual(self.entry(3), {})

    def test_sem_nada_antigo_nao_faz_nada(self):
        self.assertEqual(self.k.drop_tab_shortcuts(), 0)

    def test_ajuda_diz_que_precisa_clicar_na_barra(self):
        row = [d for k, d in self.k.BUILTIN if "Ctrl+1" in k]
        self.assertEqual(len(row), 1)
        self.assertIn("clicar na barra", row[0])


class DangerousTests(unittest.TestCase):
    """Atalhos que capturam uma tecla de digitação sem modificador de verdade 'engolem' a tecla no sistema todo."""

    def setUp(self):
        self.k = load_keys()

    def test_soltos_sao_perigosos(self):
        for b in ("e", "L", "5", "space", "<Shift>e", "<Lock>e", "<Mod2>e", "<Mod5>e", "<Shift><Mod2>l", ";"):
            self.assertTrue(self.k.is_dangerous(b, True), b)

    def test_super_sem_modificador_vira_tecla_solta(self):
        self.assertTrue(self.k.is_dangerous("<Super>e", False))
        self.assertTrue(self.k.is_dangerous("<Super><Shift>s", False))
        self.assertFalse(self.k.is_dangerous("<Super>e", True))

    def test_atalhos_normais_ficam(self):
        for b in ("<Control>e", "<Alt>F4", "<Control><Shift>Escape", "XF86AudioMute", "F1", "Print", "",
                  "disabled", "<Super>Up", "<Mod4>e", "<Control><Alt>l"):
            self.assertFalse(self.k.is_dangerous(b, True), b)

    def test_chave_do_atalho(self):
        self.assertEqual(self.k.binding_key("<Super><Shift>s"), "s")
        self.assertEqual(self.k.binding_key("<Super>Up"), "Up")
        self.assertTrue(self.k.has_super("<Primary><SUPER>d"))


class SanitizeTests(unittest.TestCase):
    def setUp(self):
        FakeSettings.store = {}
        self.k = load_keys()
        self.k.slugs = lambda: [p.rstrip("/").rsplit("/", 1)[-1] for p in FakeSettings.store]

    def binding(self, slug):
        return FakeSettings.store.get(self.k.BASE + slug + "/", {}).get("binding", "")

    def test_limpa_so_os_soltos(self):
        for slug, b in (("a", "e"), ("b", "<Shift>l"), ("c", "<Mod2>r"), ("d", "<Control>e"), ("f", "<Super>e")):
            FakeSettings.store[self.k.BASE + slug + "/"] = {"binding": b}
        self.assertEqual(self.k.sanitize(True), 3)
        self.assertEqual([self.binding(x) for x in "abcdf"], ["", "", "", "<Control>e", "<Super>e"])

    def test_super_sem_modificador_tambem_e_limpo(self):
        FakeSettings.store[self.k.BASE + "x/"] = {"binding": "<Super>e"}
        self.assertEqual(self.k.sanitize(False), 1)
        self.assertEqual(self.binding("x"), "")

    def test_apply_tenta_consertar_a_super_antes_de_desistir(self):
        calls = []
        state = {"ok": False}
        self.k.super_ok = lambda: state["ok"]
        self.k.fix_super = lambda: calls.append("fix") or state.update(ok=True) or True
        self.assertEqual(self.k.apply(), 0)
        self.assertEqual(calls, ["fix"])
        self.assertEqual(self.binding("andrey-files"), "<Super>e")        # com a Super consertada, os atalhos nascem

    def test_apply_sem_conserto_nao_cria_atalho_com_super(self):
        self.k.super_ok = lambda: False
        self.k.fix_super = lambda: False
        self.k.apply()
        self.assertEqual(self.binding("andrey-files"), "")
        self.assertEqual(self.binding("andrey-vol-up"), "XF86AudioRaiseVolume")


class HealTests(unittest.TestCase):
    def setUp(self):
        FakeSettings.store = {}
        self.k = load_keys()
        self.k.slugs = lambda: [p.rstrip("/").rsplit("/", 1)[-1] for p in FakeSettings.store]
        self.k.super_ok = lambda: True
        self.k.log = lambda _m: None
        self.k.apply()                                      # estado normal: todos os atalhos criados

    def binding(self, slug):
        return FakeSettings.store.get(self.k.BASE + slug + "/", {}).get("binding", "")

    def test_sem_teclas_capturadas_nao_mexe_nos_atalhos(self):
        self.k.probe = lambda: []
        self.assertEqual(self.k.heal(settle=0), 0)
        self.assertEqual(self.binding("andrey-files"), "<Super>e")

    def test_nao_recria_atalho_que_a_pessoa_apagou(self):
        self.k.probe = lambda: []
        FakeSettings.store[self.k.BASE + "andrey-run/"]["binding"] = ""
        self.k.heal(settle=0)
        self.assertEqual(self.binding("andrey-run"), "")

    def test_super_consertada_recria_os_atalhos(self):
        state = {"ok": False}
        self.k.super_ok = lambda: state["ok"]
        self.k.fix_super = lambda: state.update(ok=True) or True
        self.k.probe = lambda: []
        FakeSettings.store[self.k.BASE + "andrey-run/"]["binding"] = ""
        self.k.heal(settle=0)
        self.assertEqual(self.binding("andrey-run"), "<Super>r")

    def test_sem_x_nao_faz_nada(self):
        self.k.probe = lambda: None
        self.assertEqual(self.k.heal(settle=0), 0)
        self.assertEqual(self.binding("andrey-files"), "<Super>e")

    def test_tecla_engolida_desliga_so_os_atalhos_super_daquela_tecla(self):
        answers = [[(26, "e")], []]                          # 1o teste: E engolido; 2o: liberado
        self.k.probe = lambda: answers.pop(0)
        self.assertEqual(self.k.heal(settle=0), 0)
        self.assertEqual(self.binding("andrey-files"), "")          # <Super>e foi desligado
        self.assertEqual(self.binding("andrey-run"), "<Super>r")    # os outros ficam

    def test_tecla_que_continua_engolida_devolve_erro(self):
        self.k.probe = lambda: [(26, "x")]
        self.assertEqual(self.k.heal(settle=0), 1)
        self.assertEqual(self.binding("andrey-files"), "<Super>e")  # nenhum atalho com X: nada a desligar


if __name__ == "__main__":
    unittest.main()
