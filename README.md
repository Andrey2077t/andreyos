# Andrey OS
Sistema operacional focado em privacidade, desempenho e velocidade.Um desktop limpo, leve e sob controle. Sem telemetria, sem serviços desnecessários e sem atrito.O que prioriza
Privacidade

Nenhum dado é coletado. O sistema não tenta “melhorar a experiência” às custas do usuário.
Desempenho

Ambiente enxuto. Recursos ficam disponíveis para o que realmente importa.
Velocidade

Interface direta, resposta rápida e atualizações que não forçam reinstalação.
FilosofiaMenos camadas entre o usuário e a máquina.

Menos ruído.

Mais controle.


## Drivers e hardware

O Andrey OS já vem com firmware (`firmware-linux`, `firmware-misc-nonfree`, SOF), microcódigo Intel/AMD,
Mesa (OpenGL/Vulkan), aceleração de vídeo (VA-API) e fwupd. O que depende do seu hardware é decidido pelo
**Drivers e hardware** (`andrey-drivers`, também no menu):

- `andrey-drivers` — janela: detecta CPU/GPU/Wi-Fi/VM e instala o que falta
- `andrey-drivers --report` — relatório em texto (não instala nada)
- `andrey-drivers --auto` — instala tudo que for recomendado, sem janela
- `andrey-drivers --newer` — kernel, Mesa, firmware e NVIDIA mais novos (trixie-backports)
- `andrey-drivers --firmware` — atualizações de BIOS/SSD/periféricos via fwupd

NVIDIA: o pacote certo (`nvidia-driver` ou um `legacy`) vem do `nvidia-detect`. AMD e Intel usam o driver do
kernel + Mesa (não existe "driver da AMD/Intel para baixar" no Linux: o do Debian já é o oficial).
Com Secure Boot ligado, o assistente cria a chave do DKMS e pede uma senha para cadastrar no MOK (tela azul no
próximo boot). Depois do login, um aviso único oferece instalar o que faltar.


## Bluetooth

Vem pronto na ISO e funciona até com adaptador USB genérico/barato: `bluez`, `blueman` (ícone na área de notificação +
gerenciador), `pulseaudio-module-bluetooth` (fone/caixa A2DP/HSP), `bluez-obexd` (enviar/receber arquivos) e firmware
Realtek, Atheros e Intel/MediaTek. O hook `config/hooks/live/0250-bluetooth.hook.chroot` **quebra o build** se algo faltar.

- Liga sozinho (`AutoEnable`) e repareia fone/mouse que "esqueceu" o pareamento (`JustWorksRepairing = always`).
- `btusb enable_autosuspend=0` + regra udev: adaptador barato não some mais por economia de energia.
- `andrey-bluetooth` abre o gerenciador · `--check` diagnóstico · `--fix` (menu: *Reparar Bluetooth*) libera o rfkill,
  recarrega o driver, religa o adaptador USB e reinicia o `bluetoothd` · `--compat` modo só BR/EDR para adaptador
  falsificado/antigo (`--compat off` volta ao normal).
- `andrey-drivers` detecta adaptador USB Bluetooth e instala o firmware que faltar.
- Quem já usa o sistema recebe tudo ao atualizar o `andrey-os-base` (publique uma versão nova).

## Jogos: Steam, Proton e Wine

Já vêm instalados na ISO (arquitetura i386 habilitada, `steam-installer`, `wine` + `wine32`, Mesa/Vulkan/OpenGL de 32 bits,
`winetricks`, `gamemode`, `steam-devices` quando disponíveis no Debian). O hook `config/hooks/live/0150-games.hook.chroot` faz
a instalação e **quebra o build** se Steam, Wine ou as libs gráficas de 32 bits faltarem.

- **Proton**: o Steam baixa o Proton oficial sozinho. Ative em *Steam › Configurações › Compatibilidade › Ativar Steam Play
  para todos os outros títulos*.
- `andrey-games` — janela de diagnóstico/reparo e instalador do Proton-GE (também no menu: *Jogos*)
- `andrey-games --check` — relatório em texto · `--repair` — instala o que faltar · `--proton-ge` — baixa o Proton-GE
  mais novo (SHA-512 conferido, sem root)
- NVIDIA: o `andrey-drivers` agora também instala `nvidia-driver-libs-i386` (o Steam precisa)
- `vm.max_map_count` alto (`/usr/lib/sysctl.d/80-andrey-games.conf`), exigido por vários jogos
- Wine: arquivos `.exe` abrem com duplo clique; `winetricks` para runtimes (vcrun, dotnet, DXVK)

A ISO fica ~1 GB maior por causa das bibliotecas de 32 bits.

## Desinstalar Programas

Janela em Qt6 (`ui/andrey_uninstall.cpp`, binário `/usr/bin/andrey-uninstall`, no menu *Sistema*). Lista os programas
instalados que têm atalho no menu, com ícone, tamanho, versão e data de instalação reais.

- **Backend** (`/usr/lib/andrey-os/andrey_uninstall.py`): `list` (JSON), `plan PACOTE...` (o que sairia junto) e
  `check PACOTE...` (código 3 = recusado). Lê `.desktop` + `/var/lib/dpkg`, sem root.
- **Segurança:** o sistema (`andrey-os-base`, MATE, lightdm, rede, pacotes `Essential`...) não aparece na lista e a
  remoção é recusada se a simulação do apt (`apt-get -s remove`) mostrar que ela arrastaria algum deles — por exemplo,
  remover o único navegador que o `andrey-os-base` exige. A janela avisa **o que mais será removido** antes de confirmar.
- **Quem remove:** `pkexec /usr/lib/andrey-os/apt-helper remove ...`, que roda a mesma checagem como root (vale
  também para a Loja). Usa `apt-get remove` (não `purge`: configurações ficam).
- `andrey-uninstall --dry-run` só simula. Para testar a partir do repositório:
  `ANDREY_LIB_DIR=pkg/andrey-os-base/root/usr/lib/andrey-os ./andrey-uninstall --dry-run`
- **Build:** precisa de `qt6-base-dev qt6-svg-dev`. Por compilar um binário, o `andrey-os-base` agora é **amd64**
  (`andrey-os-base_VERSAO_amd64.deb`). Sem Qt, `ANDREY_SKIP_QT=1 pkg/build-deb.sh ...` gera o pacote sem a janela (só testes).


## Volume (Display na tela)

Ao usar as teclas de volume (Fn+F1/F2/F3, teclas de multimídia ou Super+F2/F3/F4) aparece o **Display de volume**: uma
barra vermelha na parte de baixo da tela com o ícone de alto-falante, o slider e a porcentagem. Código em
`ui/andrey_volume_osd.cpp` (Qt6, binário `/usr/bin/andrey-volume-osd`).

- Não rouba o foco, fica por cima de tudo e some sozinho (~2 s). Segurar a tecla só atualiza a mesma janela (instância única).
- Com o aviso na tela: arrastar o trilho ou girar a roda do mouse ajusta o volume; clicar no ícone liga/desliga o mudo.
  No mudo o ícone vira um X, a barra fica cinza e o texto mostra "Mudo".
- `andrey-volume up|down|mute|get|set N` é quem mexe no som (pactl, wpctl ou amixer). `--quiet` não mostra o aviso.
  Sem o `andrey-volume-osd` (pacote gerado sem Qt) volta a ser uma notificação comum.
- Para ver o visual sem mexer no som: `andrey-volume-osd --window 70` · para um PNG: `QT_QPA_PLATFORM=offscreen andrey-volume-osd --grab a.png 70 0`.
- **Build:** além de `qt6-base-dev qt6-svg-dev` usa o Qt6Network (já vem no `qt6-base-dev`).
