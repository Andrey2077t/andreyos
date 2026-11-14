"""andrey-volume: up/down/set/mute/get e a chamada do Display de volume (andrey-volume-osd).

Usa um `pactl` falso (estado num arquivo) e um `andrey-volume-osd` falso (so registra os argumentos),
entao roda sem som, sem tela e sem rede.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile
import time
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "pkg/andrey-os-base/root/usr/bin/andrey-volume"

FAKE_PACTL = r"""#!/bin/sh
D="$FAKE_DIR"
[ -f "$D/vol" ] || echo 50 > "$D/vol"
[ -f "$D/mute" ] || echo no > "$D/mute"
case "$1" in
    get-sink-volume) v=$(cat "$D/vol"); echo "Volume: front-left: 1 / ${v}% / -3.00 dB,   front-right: 1 / ${v}% / -3.00 dB" ;;
    get-sink-mute)   echo "Mute: $(cat "$D/mute")" ;;
    set-sink-volume) echo "${3%\%}" > "$D/vol" ;;
    set-sink-mute)   case "$3" in 1) echo yes > "$D/mute";; 0) echo no > "$D/mute";;
                     *) [ "$(cat "$D/mute")" = yes ] && echo no > "$D/mute" || echo yes > "$D/mute";; esac ;;
esac
"""

FAKE_OSD = """#!/bin/sh
echo "$*" >> "$FAKE_DIR/osd.log"
"""


class VolumeScript(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.install("pactl", FAKE_PACTL)
        (self.tmp / "vol").write_text("50\n")
        (self.tmp / "mute").write_text("no\n")

    def install(self, name, text):
        p = self.bin / name
        p.write_text(text)
        p.chmod(0o755)

    def run_vol(self, *args):
        env = {"PATH": f"{self.bin}:/usr/bin:/bin", "FAKE_DIR": str(self.tmp), "HOME": str(self.tmp)}
        return subprocess.run(["sh", str(SCRIPT), *args], env=env, capture_output=True, text=True, timeout=10)

    def vol(self):
        return int((self.tmp / "vol").read_text())

    def muted(self):
        return (self.tmp / "mute").read_text().strip() == "yes"

    def osd_log(self):
        log = self.tmp / "osd.log"
        for _ in range(40):            # o aviso e aberto em segundo plano
            if log.exists() and log.read_text():
                break
            time.sleep(0.05)
        return log.read_text().splitlines() if log.exists() else []

    def test_up_down_clamp(self):
        self.run_vol("up", "--quiet")
        self.assertEqual(self.vol(), 55)
        self.run_vol("down", "--quiet")
        self.run_vol("down", "--quiet")
        self.assertEqual(self.vol(), 45)
        (self.tmp / "vol").write_text("98\n")
        self.run_vol("up", "--quiet")
        self.assertEqual(self.vol(), 100)
        (self.tmp / "vol").write_text("2\n")
        self.run_vol("down", "--quiet")
        self.assertEqual(self.vol(), 0)

    def test_set_valida_e_limita(self):
        self.assertEqual(self.run_vol("set", "73", "--quiet").returncode, 0)
        self.assertEqual(self.vol(), 73)
        self.run_vol("set", "250", "--quiet")
        self.assertEqual(self.vol(), 100)
        for bad in ("abc", "-5", ""):
            r = self.run_vol("set", bad, "--quiet")
            self.assertEqual(r.returncode, 2, bad)
        self.assertEqual(self.vol(), 100)
        self.assertEqual(self.run_vol("set", "--quiet").returncode, 2)

    def test_set_desliga_o_mudo(self):
        (self.tmp / "mute").write_text("yes\n")
        self.run_vol("set", "30", "--quiet")
        self.assertFalse(self.muted())

    def test_mute_alterna_e_get(self):
        self.run_vol("mute", "--quiet")
        self.assertTrue(self.muted())
        self.assertEqual(self.run_vol("get").stdout.strip(), "mudo")
        self.run_vol("mute", "--quiet")
        self.assertFalse(self.muted())
        self.assertEqual(self.run_vol("get").stdout.strip(), "50%")

    def test_comando_desconhecido(self):
        self.assertEqual(self.run_vol("xyz").returncode, 2)

    def test_chama_o_display_com_volume_e_mudo(self):
        self.install("andrey-volume-osd", FAKE_OSD)
        self.run_vol("up")
        self.assertEqual(self.osd_log(), ["55 0"])
        (self.tmp / "osd.log").unlink()
        self.run_vol("mute")
        self.assertEqual(self.osd_log(), ["55 1"])

    def test_quiet_nao_chama_o_display(self):
        self.install("andrey-volume-osd", FAKE_OSD)
        self.run_vol("set", "40", "--quiet")
        time.sleep(0.3)
        self.assertFalse((self.tmp / "osd.log").exists())

    def test_sem_display_nao_quebra(self):
        # sem andrey-volume-osd (pacote sem Qt) cai na notificacao; sem gdbus ela e ignorada
        r = self.run_vol("up")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.vol(), 55)


if __name__ == "__main__":
    unittest.main()
