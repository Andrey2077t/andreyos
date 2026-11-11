# Andrey OS

Distro personalizada baseada em **Debian 13 (trixie)** + **MATE**, construída com `live-build`.

## Estrutura

```
andrey-os/
├── VERSION                       # versão atual (usada pelas ISOs)
├── build.sh                      # gera a ISO (rodar como root)
├── auto/                         # config / build / clean do live-build
├── ui/andrey_os.cpp              # sua UI em C++/Cairo (wallpaper + ícones SVG)
├── pkg/                          # pacote andrey-os-base e repositório apt
│   ├── andrey-os-base/           #   control, postinst e root/ (arquivos que vão para o sistema)
│   ├── build-deb.sh              #   monta o .deb
│   ├── render-assets.sh          #   recompila a UI e regenera wallpaper/ícones
│   └── update-apt-repo.sh        #   atualiza o repositório apt (GitHub Pages)
├── config/
│   ├── package-lists/            # pacotes instalados na ISO
│   ├── hooks/live/               # 0050 instala o andrey-os-base, 0100 branding, 0300 login/boot
│   └── includes.chroot/          # só o que é exclusivo da sessão live (assistente de conta)
└── .github/workflows/
    ├── release.yml               # "Publicar versão (atualização)"
    └── build-iso.yml             # "Build Andrey OS ISO" (manual)
```

> Visual, painéis, ícones, wallpaper, Plymouth, conky, branding do Calamares e o atualizador
> ficam em `pkg/andrey-os-base/root/` — é o **mesmo pacote** que entra na ISO e que as
> atualizações distribuem.

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

## Interface (o que já está implementado)

- **Barra superior**: menu Aplicativos/Locais/Sistema, bandeja e relógio (painel MATE).
- **Dock lateral esquerdo**: painel vertical com grade de apps, arquivos, Firefox, terminal, monitor, configurações e desligar.
- **Dock inferior**: grade de apps, Firefox, terminal, arquivos, configurações e lixeira.
- **Ícones vermelhos**: `andrey_os --icons` exporta os ícones do desenho em SVG para o tema `Andrey` (`usr/share/icons/Andrey`, herda do Adwaita). Os lançadores próprios são `usr/share/applications/andrey-*.desktop`.
- **Tema**: Adwaita-dark + CSS vermelho em `~/.config/gtk-3.0/gtk.css` (veio de `/etc/skel`), compositing do Marco ligado (transparência dos painéis e do conky).
- **Boot**: tema Plymouth `andrey` (wallpaper + barra de progresso vermelha).
- **Instalador**: o hook `0300` troca nome, cores e logo do branding do Calamares.

## Próximos passos

- Slideshow próprio do Calamares.
- Relógio centralizado na barra superior (o layout clássico do mate-panel só alinha à esquerda/direita).
- Tema GTK completo (hoje: Adwaita-dark + CSS).
- Painel conky: se não aparecer, veja `~/.cache/conky.log`.

## Recursos estilo Windows (vêm por atualização, sem ISO nova)

| Recurso | Como usar | Onde mora |
|---|---|---|
| **Menu Iniciar próprio** | tecla **Super** sozinha, ou o botão de grade nos docks. Fixados, recomendados, todos os apps (A-Z), busca de apps/arquivos/contas (`2+2`)/comandos/web, energia | `usr/bin/andrey-start` |
| Tecla Super abre o menu **sem atrapalhar** Super+E, Super+D etc. | escuta passiva do teclado (XRecord, como o xcape) | `usr/bin/andrey-superkey` |
| **Barra de tarefas** | lista de janelas abertas na barra superior + botão "mostrar área de trabalho" no canto | `andrey.layout` |
| **Histórico da área de transferência** | **Super+V** (clique cola na janela anterior; só na memória) | `usr/bin/andrey-clipboard` |
| **Recorte de tela** | **Super+Shift+S** (copia a região) e **Print** | `usr/bin/andrey-screenshot` |
| **Loja de Aplicativos** + duplo clique em `.deb` | Menu Iniciar → Loja; instala Chrome, VLC, LibreOffice, Steam, Wine... | `usr/bin/andrey-store`, `usr/lib/andrey-os/apt-helper` |
| Bloquear tela | **Super+L** | `usr/bin/andrey-lock` |
| **Volume pelo teclado** | **Fn+F2** diminui, **Fn+F3** aumenta, **Fn+F1** muda (teclas XF86Audio\*); alternativa **Super+F2/F3/F4**. Passo de 5%, notificação com o nível | `usr/bin/andrey-volume` |
| Atalhos de janela | **Super+D** área de trabalho, **Super+↑/↓** maximizar/restaurar, **Super+←/→** encaixar | `90_andrey-os.gschema.override` |
| Outros atalhos | **Super+E** Arquivos, **Super+R** Executar, **Super+S** busca, **Super+I** Configurações, **Ctrl+Shift+Esc** gerenciador de tarefas, **Super+F1** lista | `usr/bin/andrey-keys` |
| "Novo documento" no botão direito | pasta `~/Modelos` | `usr/lib/andrey-os/session-setup` |
| Toque para clicar no touchpad | ligado por padrão | gschema override |

**Como chega nas pessoas:** o `session-setup` roda a cada login (via `user-sync`) e aplica uma vez por usuário:
atalhos, modelos de documento, associação do `.deb` e recarga dos painéis (`dconf reset /org/mate/panel/` + o
mate-session reinicia o painel com o layout novo). Atalhos que a pessoa mudar depois **não** são sobrescritos;
`andrey-keys --reset` restaura os padrões. Depois da atualização é preciso **sair e entrar** na sessão.

**Dependências:** nenhuma nova (só `python3-gi`/GTK3 que o pacote já exigia), por isso a atualização funciona
mesmo antes de existir lista de pacotes do Debian no sistema. A Loja roda `apt-get update` sozinha quando precisa.

## Gerar a ISO pelo GitHub Actions (sem precisar de Debian)

1. Envie esta pasta para o repositório no GitHub.
2. Na aba **Actions**, escolha **Build Andrey OS ISO** e clique em **Run workflow**.
3. Quando terminar (30–90 min), baixe o artefato **andrey-os-iso** na página da execução.
4. Se falhar, baixe o artefato **build-log** e analise o `build.log`.

A ISO só precisa ser gerada de novo para **instalar do zero** ou quando mudar algo da sessão live.
Para o resto, use as atualizações abaixo.

## Atualizações (sem gerar ISO nova)

O sistema instalado se atualiza a partir de um **repositório apt** hospedado no GitHub Pages
(branch `gh-pages`). Quem instalou a ISO recebe as versões novas pelo **Atualizar Andrey OS**
(menu Aplicativos), que mostra "versão instalada → nova versão", a versão anterior e as notas.
Também avisa sozinho ~90 s depois do login quando há versão nova.

### Publicar uma versão nova
1. Faça as mudanças e envie para o `main` (`git add -A && git commit -m "..." && git push`).
2. **Actions → Publicar versão (atualização) → Run workflow**.
3. Digite o número (ex.: `0.0.6`, sempre maior que o anterior) e o que mudou. Pronto: o workflow
   monta o `.deb`, atualiza o repositório apt, o `latest.json` e cria a release `v0.0.6`.

### Onde a versão aparece (sempre igual ao que você publicou)
A versão vem do pacote `andrey-os-base`. A cada instalação/atualização o script
`/usr/lib/andrey-os/sync-version` copia esse número para `/etc/os-release`, `/etc/issue`, `/etc/issue.net`
e `/etc/motd` (então `hostnamectl`, o login e o terminal mostram o mesmo número).
A tela **Sobre o Andrey OS** (`andrey-about`) mostra logo, versão instalada, versão disponível no
repositório e as novidades; fica em **Sistema → Preferências** e no Centro de Controle do MATE.
No terminal: `andrey-about --text`.

### Configurar o GitHub Pages (uma vez só)
1. Rode **Publicar versão** pela primeira vez (isso cria a branch `gh-pages`).
2. No repositório: **Settings → Pages → Build and deployment → Source: Deploy from a branch**,
   **Branch: `gh-pages`**, pasta **`/ (root)`** → **Save**.
3. Em ~1 min abra `https://SEU_USUARIO.github.io/SEU_REPO/latest.json` — deve mostrar a versão.
4. **Instale uma ISO nova uma vez** (ela já vem com o repositório configurado). Dali em diante, só atualizações.

Usa Cloudflare Pages? Dá para servir a pasta `apt/` de lá também: defina `ANDREY_UPDATE_URL`
(ex.: `https://andreyos.pages.dev`) no `pkg/build-deb.sh`/workflow, e publique o conteúdo de `gh-pages` no Cloudflare.

### Assinar o repositório (recomendado, depois que funcionar)
Sem chave, o repositório usa `[trusted=yes]` (confia no HTTPS do GitHub). Para assinar:

```bash
# no Git Bash (Windows) — gera uma chave SEM senha, só para o repositório
gpg --batch --passphrase '' --quick-gen-key "Andrey OS <andrey-os@localhost>" rsa4096 sign never
gpg --armor --export-secret-keys "Andrey OS"      # copie TUDO que aparecer
```
No GitHub: **Settings → Secrets and variables → Actions → New repository secret**, nome
`APT_GPG_PRIVATE_KEY`, valor = o texto copiado. Na próxima publicação o repositório sai
assinado e o pacote leva a chave pública. **Nunca** coloque a chave privada no repositório.

### Mudanças que vivem na HOME do usuário
O `/etc/skel` só vale para usuários **novos**. Para mudar algo de quem já existe (recarregar o layout
dos painéis, trocar o `~/.conkyrc`...), crie `pkg/andrey-os-base/root/usr/lib/andrey-os/migrations/<versão>.sh`
(ex.: `0.0.6.sh`). Roda uma vez por usuário no próximo login. Veja o README dentro da pasta.

### Limitações
- A **sessão live** guarda tudo na RAM: atualização só vale para o sistema **instalado**.
- Kernel e pacotes do Debian continuam vindo do `apt upgrade` normal.
