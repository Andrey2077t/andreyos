import importlib
import importlib.machinery
import importlib.util
import os
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..", "pkg", "andrey-os-base", "root")
sys.path.insert(0, os.path.join(ROOT, "usr", "lib", "andrey-os"))
e = importlib.import_module("andrey_exe")


def load_cli():
    path = os.path.join(ROOT, "usr", "bin", "andrey-exe")
    loader = importlib.machinery.SourceFileLoader("andrey_exe_cli", path)
    spec = importlib.util.spec_from_loader("andrey_exe_cli", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def make_pe(path, imports=(), bits=64, gui=True, dll=False, extra=b""):
    """PE mínimo e válido: cabeçalho + uma seção .idata com a tabela de importação."""
    pe_off = 0x80
    opt_size = 240 if bits == 64 else 224
    sect_va, sect_raw = 0x1000, 0x400
    # tabela de importação: descritores (20 bytes) + nomes
    names = [n.encode() + b"\0" for n in imports]
    desc_len = 20 * (len(imports) + 1)
    name_va = sect_va + desc_len
    desc, offs = b"", 0
    for n in names:
        desc += struct.pack("<IIIII", 0, 0, 0, name_va + offs, 0)
        offs += len(n)
    desc += b"\0" * 20
    idata = desc + b"".join(names)
    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664 if bits == 64 else 0x14C, 1, 0, 0, 0, opt_size,
                                   (0x2000 if dll else 0) | 0x0002)
    magic = 0x20B if bits == 64 else 0x10B
    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, magic)
    struct.pack_into("<H", opt, 68, 2 if gui else 3)
    dd = 112 if bits == 64 else 96
    struct.pack_into("<I", opt, dd - 4, 16)                       # NumberOfRvaAndSizes
    struct.pack_into("<II", opt, dd + 8, sect_va if imports else 0, len(idata))   # diretório 1 = importações
    sect = b".idata\0\0" + struct.pack("<IIIIIIHHI", len(idata), sect_va, len(idata), sect_raw, 0, 0, 0, 0, 0x40000040)
    blob = bytearray(sect_raw + len(idata))
    blob[0:2] = b"MZ"
    struct.pack_into("<I", blob, 0x3C, pe_off)
    blob[pe_off:pe_off + len(coff)] = coff
    blob[pe_off + 24:pe_off + 24 + opt_size] = opt
    blob[pe_off + 24 + opt_size:pe_off + 24 + opt_size + len(sect)] = sect
    blob[sect_raw:sect_raw + len(idata)] = idata
    with open(path, "wb") as fh:
        fh.write(bytes(blob) + extra)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))

    def put(self, *parts, data=b""):
        p = os.path.join(self.tmp, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(data)
        return p


class PeTests(Base):
    def test_le_importacoes_e_arquitetura(self):
        p = os.path.join(self.tmp, "a.exe")
        make_pe(p, imports=("KERNEL32.dll", "D3D11.dll"), bits=64)
        pe = e.read_pe(p)
        self.assertEqual(pe["bits"], 64)
        self.assertTrue(pe["gui"])
        self.assertFalse(pe["dll"])
        self.assertEqual(pe["imports"], {"kernel32.dll", "d3d11.dll"})
        make_pe(p, imports=("x.dll",), bits=32, gui=False, dll=True)
        pe = e.read_pe(p)
        self.assertEqual((pe["bits"], pe["console"], pe["dll"]), (32, True, True))

    def test_lixo_nao_quebra(self):
        for data in (b"", b"MZ", b"MZ" + b"\0" * 100, b"hello world" * 50, b"MZ" + b"\xff" * 4000):
            p = self.put("lixo.exe", data=data)
            self.assertIsNone(e.read_pe(p))
        self.assertIsNone(e.read_pe(os.path.join(self.tmp, "nao-existe.exe")))

    def test_pe_truncado_nao_levanta_excecao(self):
        p = os.path.join(self.tmp, "t.exe")
        make_pe(p, imports=("a.dll", "b.dll"))
        with open(p, "rb") as fh:
            data = fh.read()
        for cut in (0x90, 0x100, 0x200, 0x405, len(data) - 3):
            with open(p, "wb") as fh:
                fh.write(data[:cut])
            e.read_pe(p)   # só não pode levantar exceção


class ClassifyTests(Base):
    def test_unity_com_steam_e_game(self):
        p = os.path.join(self.tmp, "Meu Jogo", "game.exe")
        os.makedirs(os.path.dirname(p))
        make_pe(p, imports=("kernel32.dll", "d3d11.dll"))
        open(os.path.join(self.tmp, "Meu Jogo", "UnityPlayer.dll"), "wb").close()
        self.put("Meu Jogo", "steam_appid.txt", data=b"12345\n")
        c = e.classify(p)
        self.assertEqual(c["kind"], "game")
        self.assertEqual(c["appid"], "12345")
        self.assertEqual(c["bits"], 64)
        self.assertEqual(e.display_name(p, "game"), "Meu Jogo")

    def test_unreal_shipping(self):
        p = os.path.join(self.tmp, "Jogo", "Game", "Binaries", "Win64", "Jogo-Win64-Shipping.exe")
        os.makedirs(os.path.dirname(p))
        make_pe(p, imports=("kernel32.dll",))
        self.assertEqual(e.classify(p)["kind"], "game")
        self.assertEqual(e.display_name(p, "game"), "Jogo")   # sobe até a primeira pasta que não é genérica (Win64, Binaries, Game)

    def test_dados_de_jogo_mais_directx(self):
        p = os.path.join(self.tmp, "x", "app.exe")
        os.makedirs(os.path.dirname(p))
        make_pe(p, imports=("d3d9.dll", "xinput1_3.dll"))
        self.put("x", "data.pak")
        self.assertEqual(e.classify(p)["kind"], "game")

    def test_programa_comum_so_com_kernel32(self):
        p = os.path.join(self.tmp, "Downloads", "calc.exe")
        os.makedirs(os.path.dirname(p))
        make_pe(p, imports=("kernel32.dll", "user32.dll", "comctl32.dll"))
        c = e.classify(p)
        self.assertEqual(c["kind"], "app")
        self.assertEqual(e.prefix_for("app", p, home=self.tmp), e.prefix_for("app", "/x/y/outro.exe", home=self.tmp))

    def test_electron_com_pak_nao_vira_jogo(self):
        p = os.path.join(self.tmp, "Chat", "chat.exe")
        os.makedirs(os.path.dirname(p))
        make_pe(p, imports=("d3d11.dll",))
        self.put("Chat", "resources.pak")
        self.put("Chat", "icudtl.dat")
        self.assertEqual(e.classify(p)["kind"], "app")

    def test_instaladores(self):
        d = os.path.join(self.tmp, "Downloads")
        os.makedirs(d)
        by_name = os.path.join(d, "Meu_Setup_x64.exe")
        make_pe(by_name, imports=("kernel32.dll",))
        self.assertEqual(e.classify(by_name)["kind"], "installer")
        inno = os.path.join(d, "app123.exe")
        make_pe(inno, imports=("kernel32.dll",), extra=b"\0" * 100 + b"Inno Setup Setup Data (6.2.2)")
        c = e.classify(inno)
        self.assertEqual(c["kind"], "installer")
        self.assertIn("Inno Setup", c["reasons"][0])
        msi = self.put("Downloads", "x.msi", data=b"\xd0\xcf\x11\xe0")
        self.assertEqual(e.classify(msi)["kind"], "installer")

    def test_redistribuivel_usa_o_prefixo_comum(self):
        p = os.path.join(self.tmp, "vc_redist.x64.exe")
        make_pe(p)
        c = e.classify(p)
        self.assertEqual((c["kind"], c["redist"]), ("installer", True))
        self.assertTrue(e.prefix_for("installer", p, home=self.tmp, redist=True).endswith("programas"))

    def test_instalador_de_jogo_ganha_prefixo_proprio_estavel(self):
        a = e.prefix_for("installer", "/d/setup_cool_game_1.0.exe", home=self.tmp)
        self.assertEqual(a, e.prefix_for("installer", "/outro/setup_cool_game_1.0.exe", home=self.tmp))
        self.assertIn("cool-game", a)
        self.assertNotEqual(a, e.prefix_for("installer", "/d/setup_outro.exe", home=self.tmp))
        # só "setup.exe": nome genérico não pode juntar instaladores diferentes no mesmo prefixo
        self.assertNotEqual(e.prefix_for("installer", "/a/setup.exe", home=self.tmp),
                            e.prefix_for("installer", "/b/setup.exe", home=self.tmp))

    def test_prefixo_de_jogo_por_pasta(self):
        a = e.prefix_for("game", "/jogos/Alpha/game.exe", home=self.tmp)
        self.assertNotEqual(a, e.prefix_for("game", "/jogos/Beta/game.exe", home=self.tmp))
        self.assertEqual(a, e.prefix_for("game", "/jogos/Alpha/game.exe", home=self.tmp))
        self.assertTrue(a.startswith(e.prefixes_dir(self.tmp)))


class ProtonTests(Base):
    def test_escolhe_o_mais_novo_entre_usuario_e_sistema(self):
        sysdir = os.path.join(self.tmp, "sys")
        old = e.SYSTEM_PROTON_DIR
        e.SYSTEM_PROTON_DIR = sysdir
        self.addCleanup(lambda: setattr(e, "SYSTEM_PROTON_DIR", old))
        for base, tag in ((sysdir, "GE-Proton9-27"), (e.G.compat_dir(self.tmp), "GE-Proton10-4"),
                          (e.G.compat_dir(self.tmp), "GE-Proton10-12")):
            os.makedirs(os.path.join(base, tag))
            open(os.path.join(base, tag, "proton"), "w").close()
        os.makedirs(os.path.join(sysdir, "GE-Proton99-9"))      # pasta sem o arquivo 'proton' não conta
        self.assertEqual(os.path.basename(e.find_proton({}, self.tmp)), "GE-Proton10-12")
        self.assertEqual(e.find_proton({"proton": "GE-Proton9-27"}, self.tmp), "GE-Proton9-27")

    def test_sem_nenhum_deixa_o_umu_baixar(self):
        old = e.SYSTEM_PROTON_DIR
        e.SYSTEM_PROTON_DIR = os.path.join(self.tmp, "vazio")
        self.addCleanup(lambda: setattr(e, "SYSTEM_PROTON_DIR", old))
        self.assertEqual(e.find_proton({}, self.tmp), "GE-Proton")


class CommandTests(Base):
    def test_exe_com_umu(self):
        cmd, env, cwd = e.build_run("/j/Alpha/g.exe", "game", "/pfx", "/ge", ["-x"], umu="/usr/bin/umu-run", appid="570",
                                    environ={"A": "1"})
        self.assertEqual(cmd, ["/usr/bin/umu-run", "/j/Alpha/g.exe", "-x"])
        self.assertEqual((env["WINEPREFIX"], env["PROTONPATH"], env["GAMEID"], env["A"]), ("/pfx", "/ge", "umu-570", "1"))
        self.assertEqual(cwd, "/j/Alpha")

    def test_appid_so_vale_para_jogo_e_msi_bat(self):
        _, env, _ = e.build_run("/a/b.exe", "app", "/p", "GE-Proton", umu="umu-run", appid="570", environ={})
        self.assertNotIn("GAMEID", env)
        self.assertEqual(e.build_run("/a/x.msi", "installer", "/p", "g", umu="umu-run", environ={})[0],
                         ["umu-run", "msiexec", "/i", "/a/x.msi"])
        self.assertEqual(e.build_run("/a/x.bat", "app", "/p", "g", umu="umu-run", environ={})[0],
                         ["umu-run", "cmd", "/c", "/a/x.bat"])

    def test_sem_umu_cai_no_wine(self):
        cmd, env, _ = e.build_run("/a/b.exe", "app", "/p", "g", umu=None, environ={})
        self.assertEqual(cmd, ["wine", "/a/b.exe"])
        self.assertEqual(env["WINEPREFIX"], "/p")
        self.assertNotIn("PROTONPATH", env)


class ShortcutTests(Base):
    TRICKY = ['Jogo "Legal" $HOME `id` \\ 100% ç', "a b c", "simples", "'aspas'", "x;y&z"]

    def test_exec_do_desktop_vai_e_volta(self):
        for nm in self.TRICKY:
            args = e.shortcut_exec("game", "/p/" + nm, "/j/" + nm + "/g.exe")
            line = " ".join(e.desktop_quote(a) for a in args)
            self.assertEqual(e.desktop_unquote(line), args, nm)

    def test_entrada_nao_aceita_quebra_de_linha_no_nome(self):
        txt = e.desktop_entry("Nome\nExec=rm -rf ~", "app", "/p", "/a/b.exe", "icone")
        self.assertEqual(sum(1 for ln in txt.splitlines() if ln.startswith("Exec=")), 1)
        self.assertEqual(sum(1 for ln in txt.splitlines() if ln.startswith("Name=")), 1)
        self.assertIn("Categories=Utility;", txt)
        self.assertIn("Categories=Game;", e.desktop_entry("J", "game", "/p", "/a/b.exe", "i"))

    def test_cria_atalhos_no_menu_e_na_area_de_trabalho(self):
        exe = self.put("Jogos", "Alpha", "a.exe", data=b"x")
        made = e.write_shortcuts("Alpha", "game", "/pfx", exe, ("menu", "desktop"), home=self.tmp)
        self.assertEqual(len(made), 2)
        self.assertTrue(any(m.startswith(os.path.join(self.tmp, ".local", "share", "applications")) for m in made))
        self.assertTrue(any(m.startswith(os.path.join(self.tmp, "Desktop")) for m in made))
        for m in made:
            self.assertEqual(os.stat(m).st_mode & 0o777, 0o755)
            with open(m, encoding="utf-8") as fh:
                body = fh.read()
            self.assertIn("Name=Alpha", body)
            self.assertIn("Path=" + os.path.dirname(exe), body)
        only_menu = e.write_shortcuts("Alpha", "game", "/pfx", exe, ("menu",), home=self.tmp)
        self.assertEqual(only_menu, [m for m in made if "applications" in m])

    def test_nomes_iguais_em_pastas_diferentes_nao_se_sobrescrevem(self):
        a = e.write_shortcuts("App", "app", "/p", self.put("a", "x.exe"), ("menu",), home=self.tmp)
        b = e.write_shortcuts("App", "app", "/p", self.put("b", "x.exe"), ("menu",), home=self.tmp)
        self.assertNotEqual(a, b)


class StateTests(Base):
    def test_arquivos_conhecidos(self):
        self.assertIsNone(e.known_entry("/a/b.exe", self.tmp))
        e.mark_known("/a/b.exe", "game", "/pfx", self.tmp)
        self.assertEqual(e.known_entry("/a/b.exe", self.tmp), {"kind": "game", "prefix": "/pfx"})
        e.mark_known("/a/c.exe", "app", "/pfx2", self.tmp)
        self.assertEqual(e.known_entry("/a/b.exe", self.tmp)["prefix"], "/pfx")

    def test_known_json_corrompido_e_ignorado(self):
        os.makedirs(e.state_dir(self.tmp))
        with open(os.path.join(e.state_dir(self.tmp), "known.json"), "w") as fh:
            fh.write("{ nao é json")
        self.assertIsNone(e.known_entry("/a/b.exe", self.tmp))
        e.mark_known("/a/b.exe", "app", "/p", self.tmp)    # e a próxima gravação conserta
        self.assertIsNotNone(e.known_entry("/a/b.exe", self.tmp))

    def test_configuracao(self):
        self.assertEqual(e.load_conf(self.tmp), e.DEFAULT_CONF)
        os.makedirs(os.path.dirname(e.conf_path(self.tmp)))
        with open(e.conf_path(self.tmp), "w", encoding="utf-8") as fh:
            fh.write("# comentário\nask = never\ngamemode=on\nproton=GE-Proton9-1\nlixo\nask2=x\n")
        c = e.load_conf(self.tmp)
        self.assertEqual((c["ask"], c["gamemode"], c["proton"]), ("never", "on", "GE-Proton9-1"))
        with open(e.conf_path(self.tmp), "w", encoding="utf-8") as fh:
            fh.write("ask=talvez\ngamemode=sim\n")
        c = e.load_conf(self.tmp)
        self.assertEqual((c["ask"], c["gamemode"]), ("first", "off"))


class InstalledProgramsTests(Base):
    def test_acha_o_programa_novo_e_ignora_desinstalador(self):
        pfx = os.path.join(self.tmp, "pfx")
        for rel in ("Program Files/Foo/foo.exe", "Program Files/Foo/unins000.exe", "Program Files (x86)/Bar/Bar.exe",
                    "windows/system32/notepad.exe", "users/steamuser/x.exe", "GOG Games/Baz/baz.exe"):
            self.put("pfx", "drive_c", *rel.split("/"))
        found = {os.path.relpath(p, os.path.join(pfx, "drive_c")) for p in e.prefix_exes(pfx)}
        self.assertEqual(found, {"Program Files/Foo/foo.exe", "Program Files (x86)/Bar/Bar.exe", "GOG Games/Baz/baz.exe"})
        before = e.prefix_exes(pfx)
        self.put("pfx", "drive_c", "Program Files", "Novo", "novo.exe")
        self.assertEqual(e.new_programs(before, e.prefix_exes(pfx)), [os.path.join(pfx, "drive_c", "Program Files", "Novo", "novo.exe")])

    def test_prefixo_inexistente(self):
        self.assertEqual(e.prefix_exes(os.path.join(self.tmp, "nada")), set())


class CliTests(Base):
    def setUp(self):
        super().setUp()
        self.cli = load_cli()
        self.cli.HAVE_GUI = False

    def test_parse(self):
        p = self.cli.parse(["--kind", "game", "--prefix", "/p", "--no-ask", "--", "/a/b.exe", "-x", "--y"])
        self.assertEqual((p["kind"], p["prefix"], p["no_ask"], p["file"], p["args"]), ("game", "/p", True, "/a/b.exe", ["-x", "--y"]))
        p = self.cli.parse(["/a/b.exe", "--kind", "app"])
        self.assertEqual((p["file"], p["args"], p["kind"]), ("/a/b.exe", ["--kind", "app"], None))
        self.assertIsNone(self.cli.parse(["--kind", "banana", "x.exe"]))
        self.assertIsNone(self.cli.parse(["--nao-existe"]))
        self.assertIsNone(self.cli.parse(["-h"]))

    def test_uri_do_caja(self):
        self.assertEqual(self.cli.normalize("file:///a/b%20c/d.exe"), "/a/b c/d.exe")

    def test_recusa_dll_e_arquivo_inexistente(self):
        dll = os.path.join(self.tmp, "x.dll")
        make_pe(dll, dll=True)
        self.assertEqual(self.cli.main([dll]), 1)
        self.assertEqual(self.cli.main([os.path.join(self.tmp, "nao.exe")]), 1)

    def test_info_nao_executa(self):
        exe = os.path.join(self.tmp, "G", "g.exe")
        os.makedirs(os.path.dirname(exe))
        make_pe(exe, imports=("d3d11.dll", "xinput1_3.dll"))
        self.put("G", "steam_api64.dll")
        self.assertEqual(self.cli.main(["--info", exe]), 0)


class IntegrationFilesTests(unittest.TestCase):
    TYPES = ["application/vnd.microsoft.portable-executable", "application/x-msdownload", "application/x-dosexec",
             "application/x-ms-dos-executable", "application/x-ms-ne-executable", "application/x-msi", "application/x-bat"]

    def read(self, *rel):
        with open(os.path.join(ROOT, *rel), encoding="utf-8") as fh:
            return fh.read()

    def test_desktop_do_handler_declara_os_tipos_e_chama_o_script(self):
        txt = self.read("usr", "share", "applications", "andrey-exe.desktop")
        self.assertIn("Exec=andrey-exe %f", txt)
        mt = [ln for ln in txt.splitlines() if ln.startswith("MimeType=")][0]
        for t in self.TYPES:
            self.assertIn(t + ";", mt)
        self.assertIn("Exec=andrey-exe --shortcut %f", self.read("usr", "share", "applications", "andrey-exe-shortcut.desktop"))

    def test_mimeapps_define_o_padrao_de_todos_os_tipos(self):
        lines = self.read("etc", "xdg", "mimeapps.list").splitlines()
        sect = lines[lines.index("[Default Applications]") + 1:lines.index("[Added Associations]")]
        defaults = dict(ln.split("=", 1) for ln in sect if "=" in ln)
        for t in self.TYPES:
            self.assertEqual(defaults.get(t), "andrey-exe.desktop;", t)

    def test_session_setup_cobre_os_mesmos_tipos(self):
        txt = self.read("usr", "lib", "andrey-os", "session-setup")
        for t in self.TYPES:
            self.assertIn(t, txt)
        self.assertIn("LEVEL=9", txt)


if __name__ == "__main__":
    unittest.main()
