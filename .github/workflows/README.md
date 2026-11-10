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

Usuário live: `andrey` / senha `live` (padrão do live-config).

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
