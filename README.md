# Andrey OS

Distro personalizada baseada em **Debian 13 (trixie)** + **MATE**, construída com `live-build`.

## Estrutura

```
andrey-os/
├── build.sh                      # gera a ISO (rodar como root)
├── auto/                         # config / build / clean do live-build
├── ui/andrey_os.cpp              # sua UI em C++/Cairo (preview + wallpaper)
└── config/
    ├── package-lists/            # pacotes instalados
    ├── hooks/live/               # branding, tema, painel, login, boot
    └── includes.chroot/          # arquivos copiados para dentro do sistema
        ├── etc/skel/.conkyrc     # painel SISTEMA/CPU/MEMÓRIA/DISCO/REDE
        └── usr/share/backgrounds/andrey/andrey-os.png
```

## Como a sua UI vira sistema real

| Elemento no seu desenho | No sistema |
|---|---|
| Barra superior (Aplicativos / Locais / Sistema) | MATE panel (menu-bar) |
| Relógio, volume, rede, bateria | applets do painel MATE |
| Dock inferior | segundo painel centralizado com lançadores |
| Painel da direita (CPU, RAM...) | **conky** com dados reais |
| Fundo + logo + slogans | `andrey_os --bg` → wallpaper, tela de login e GRUB |

## Gerar a ISO

```bash
sudo ./build.sh
```

Testar: `qemu-system-x86_64 -m 4G -enable-kvm -cdrom andrey-os-amd64.hybrid.iso`

## Contas (como funciona)

**Não existe conta pronta.** Nem na sessão live, nem no sistema instalado.

- **Sessão live**: o `live-config` está com `user-setup`, `sudo` e `lightdm` desligados (`nocomponents=` em `auto/config`). Antes do login gráfico, o serviço `andrey-firstboot` abre um assistente (tela de texto, `tty8`) onde a pessoa escolhe **o próprio usuário e a própria senha, sem regras**. Ele cria o usuário, entra no grupo `sudo` e faz autologin **só nessa sessão**. Se sair da sessão, entra de novo com a senha que ela mesma criou.
- **Cancelar** no assistente oferece (opcional) uma conta temporária `andrey` / `live`, só dessa sessão. Nunca trava o boot: se o assistente falhar, cai nessa conta temporária.
- **Sistema instalado**: tudo isso vive na RAM da sessão live, então não vai pro disco. No instalador (Calamares) a pessoa cria a conta definitiva.
- Boot com `andrey.temp` na linha de comando do kernel **pula o assistente** (útil pra testar em VM sem digitar).
- O hook `0300` **derruba o build** se faltar `whiptail`, `kbd`, `passwd`, `lightdm`, `live-config`, `sudo` ou o serviço do assistente.

### Se algo der errado no login live

Pelo GRUB, edite a linha do kernel e adicione `andrey.temp`; depois:

```bash
systemctl status andrey-firstboot
journalctl -b -u andrey-firstboot -u live-config | tail -60
tail -40 /var/log/lightdm/lightdm.log
```

## Próximos passos

- Dock lateral esquerda (hoje só existe o dock inferior): painel MATE vertical ou plank.
- Tema GTK próprio (hoje: Adwaita-dark + CSS vermelho).
- Tema Plymouth com o logo animado.
- Ícones vermelhos (os do seu desenho são em Cairo; dá para exportar como SVG).
- Branding do Calamares (slideshow + cores).

## Gerar a ISO pelo GitHub Actions (sem precisar de Debian)

1. Crie um repositório no GitHub e envie esta pasta (`git init && git add . && git commit -m "Andrey OS" && git push`).
2. Na aba **Actions**, escolha **Build Andrey OS ISO** e clique em **Run workflow**.
3. Quando terminar (30–90 min), baixe o artefato **andrey-os-iso** na página da execução.
4. Se falhar, baixe o artefato **build-log** e analise o `build.log`.

Criar uma tag (`git tag v1.0 && git push --tags`) também gera a ISO e a anexa a uma release.
