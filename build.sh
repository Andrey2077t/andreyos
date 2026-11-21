#!/bin/bash
# Gera a ISO do Cisma OS. Rode num Debian/Ubuntu como root:  sudo ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
    echo "Rode como root: sudo ./build.sh" >&2
    exit 1
fi

echo "==> Instalando ferramentas de build"
apt-get update
apt-get install -y live-build debootstrap xorriso squashfs-tools grub-pc-bin grub-efi-amd64-bin \
                   mtools dosfstools g++ pkg-config libcairo2-dev libx11-dev fonts-inter dpkg-dev jq qt6-base-dev qt6-svg-dev

# (zip/Windows perdem o bit de execucao)
chmod +x auto/* config/hooks/live/*.chroot pkg/*.sh config/includes.chroot/usr/local/sbin/*

# regenera wallpaper, tela de boot e icones a partir do codigo-fonte da UI
echo "==> Renderizando arte (wallpaper, icones)"
pkg/render-assets.sh
mkdir -p config/bootloaders/grub-pc config/bootloaders/grub-efi
cp pkg/cisma-os-base/root/usr/share/backgrounds/cisma/cisma-os.png config/bootloaders/grub-pc/splash.png
cp pkg/cisma-os-base/root/usr/share/backgrounds/cisma/cisma-os.png config/bootloaders/grub-efi/splash.png

# o codigo-fonte da UI vai junto na ISO (uma unica copia em ui/; esta e gerada e ignorada pelo git)
mkdir -p config/includes.chroot/usr/share/cisma-os/ui-src
cp -f ui/cisma_os.cpp config/includes.chroot/usr/share/cisma-os/ui-src/

# monta o pacote cisma-os-base (mesmo pacote que as atualizacoes distribuem) e embute na ISO
VERSION="$(tr -d ' \n\r' < VERSION)"
echo "==> Montando cisma-os-base $VERSION"
rm -rf config/includes.chroot/usr/share/cisma-os/pkg
pkg/build-deb.sh "$VERSION" config/includes.chroot/usr/share/cisma-os/pkg

echo "==> Configurando e construindo (leva de 15 a 60 min)"
lb clean --purge || true
lb config
lb build

ls -lh ./*.iso || { echo "ERRO: o build terminou mas nenhuma ISO foi gerada." >&2; exit 1; }
echo "Pronto. Teste com:  qemu-system-x86_64 -m 4G -enable-kvm -cdrom cisma-os-amd64.hybrid.iso"
