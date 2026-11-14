# Andrey OS

O Andrey OS não possui malwares, nós não nos responsabilizamos se uma pessoa fizer um fork fornecendo malwares na versão dela, não temos afiliação com nenhum fork.

Sistema operacional focado em privacidade, desempenho e velocidade. Um desktop limpo, leve e sob controle. Sem telemetria, sem serviços desnecessários e sem atrito.

## O que prioriza

**Privacidade**
Nenhum dado é coletado. O sistema não tenta "melhorar a experiência" às custas do usuário.

**Desempenho**
Ambiente enxuto. Recursos ficam disponíveis para o que realmente importa.

**Velocidade**
Interface direta, resposta rápida e atualizações que não forçam reinstalação.

## Filosofia

Menos camadas entre o usuário e a máquina.

Menos ruído.

Mais controle.

## Identidade: tudo vira aba

O diferencial visual do Andrey OS é que **cada janela aberta aparece como uma aba**, igual num navegador. Quem vem do Windows ou do Chrome já tem o reflexo de "aba = janela", então não precisa aprender nada.

- **Barra superior em abas:** apps abertos (Discord, Chrome, Andrey Music, o que for) ficam lado a lado no topo, como abas de navegador.
- **Barra de título vermelha:** com os botões de minimizar, maximizar e fechar à direita, combinando com o tema vermelho/preto do sistema e dos apps próprios.
- **Ctrl+1 ... Ctrl+9:** vai direto para a aba 1 a 9 da barra superior (se estiver minimizada, restaura). `andrey-tabs --list` mostra a ordem.
- **Agrupar abas:** as janelas do mesmo programa podem virar uma aba só. Liga/desliga em *Agrupar abas* no menu, com `andrey-tabs --group toggle` ou em *Preferências* do botão direito na barra (`on`, `off`, `auto`). Agrupado, `Ctrl+N` vai para o N-ésimo programa e apertar de novo passa para a próxima janela dele.
- **Dock embaixo:** lançador, navegador, terminal, arquivos e lixeira, com a assinatura *"Potencial máximo em cada detalhe"*.
- **Apps exclusivos:** o **Andrey AI** e o **Andrey Music** são apps feitos para o Andrey OS e já vêm instalados. Abrem em janela própria, com o mesmo visual do sistema, e entram na barra superior como qualquer outro programa (veja a seção abaixo).

Por baixo é o MATE: a lista de janelas do painel, estilizada para parecer abas. O diferencial não é uma tecnologia nova, e sim a decisão de design aplicada de forma consistente no sistema todo (painel, barra de título, dock e os apps próprios como Andrey Music, display de volume e desinstalador), para tudo ter a mesma cara.

> Janelas em abas existem em outros projetos, por exemplo no IceWM 3.0, mas como recurso opcional do gerenciador de janelas. Até onde pesquisamos, não encontramos outra distro que use isso como identidade visual padrão do sistema.

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

## Compactar e extrair arquivos

Suporte nativo, sem instalar nada: o **Engrampa** (gerenciador de arquivos compactados do MATE) vem na ISO e se
integra ao **Caja** (Arquivos). Clique com o botão direito num arquivo ou pasta:

- **Extrair aqui** / **Extrair para…** — duplo clique num arquivo compactado também abre no Engrampa
- **Comprimir…** — escolhe o formato (`.zip`, `.7z`, `.tar.gz`, `.tar.xz`, `.tar.zst`…), nome e senha (zip/7z)

Formatos: `zip`, `7z`, `rar` (só extrair), `tar`, `gz`, `bz2`, `xz`, `zst`, `lzo`, `cpio`, `arj` e combinações (`.tar.gz` etc.).
No terminal ficam `zip`, `unzip`, `7z`, `unrar`, `tar`, `xz`, `zstd`. O hook `config/hooks/live/0260-archive.hook.chroot`
**quebra o build** se algum backend faltar. Quem já usa o sistema recebe tudo ao atualizar o `andrey-os-base`.

## Apps exclusivos: Andrey AI e Andrey Music

Dois apps próprios do Andrey OS, já instalados e integrados ao sistema (menu, atalhos, ícone e perfil separados). Abrem em janela própria, sem barra de endereço, e se já estiverem abertos só trazem a janela para a frente.

- **Andrey AI** (`andrey-ai`): a IA do sistema, a um atalho de distância com `Super+Espaço`.
- **Andrey Music** (`andrey-music`): player de música no menu *Som e Vídeo* e fixado no Menu Iniciar. A música continua tocando com a janela em segundo plano e as teclas de mídia funcionam.

Detalhes técnicos: cada app roda em Chromium em modo `--app` e carrega https://andreyai.pages.dev/ e https://andreymusic.pages.dev/. `ANDREY_MUSIC_URL` troca o endereço do Andrey Music.

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
