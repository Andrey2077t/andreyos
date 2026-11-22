# -*- coding: utf-8 -*-
"""Testa o need_lists() do apt-helper: só os índices do Debian contam como "lista de pacotes".
Rode: python3 -m unittest discover -s tests"""
import os
import re
import subprocess
import tempfile
import unittest

HELPER = os.path.join(os.path.dirname(__file__), "..", "pkg", "cisma-os-base", "root",
                      "usr", "lib", "cisma-os", "apt-helper")


def need_lists_runs_update(lists_dir, files):
    """True se need_lists() chamou 'apt-get update' com esses arquivos em lists_dir."""
    for f in files:
        open(os.path.join(lists_dir, f), "w").close()
    src = open(HELPER, encoding="utf-8").read()
    m = re.search(r"^need_lists\(\) \{.*?^\}", src, re.S | re.M)
    assert m, "need_lists() não encontrada no apt-helper"
    func = m.group(0).replace("/var/lib/apt/lists", lists_dir)
    out = subprocess.run(["bash", "-c", "apt-get() { echo APT_GET \"$@\"; }\n" + func + "\nneed_lists"],
                         capture_output=True, text=True, timeout=20)
    return "APT_GET update" in out.stdout


class NeedListsTests(unittest.TestCase):
    def run_case(self, files):
        with tempfile.TemporaryDirectory() as d:
            return need_lists_runs_update(d, files)

    def test_sem_nenhuma_lista_atualiza(self):
        self.assertTrue(self.run_case([]))

    def test_so_o_repositorio_do_cisma_ainda_precisa_atualizar(self):
        # regressão: depois de "Atualizar Cisma OS" só existia a lista do repo flat do Cisma,
        # o teste antigo achava que havia índices do Debian e a Loja dizia "pacote não encontrado"
        self.assertTrue(self.run_case(["Andrey2077t.github.io_cismaos_apt_._Packages",
                                       "Andrey2077t.github.io_cismaos_apt_._Release"]))

    def test_com_indice_do_debian_nao_atualiza(self):
        self.assertFalse(self.run_case(["deb.debian.org_debian_dists_trixie_main_binary-amd64_Packages.lz4"]))


if __name__ == "__main__":
    unittest.main()
