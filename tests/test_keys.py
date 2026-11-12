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


if __name__ == "__main__":
    unittest.main()
