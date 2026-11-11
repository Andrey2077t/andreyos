#!/bin/bash
# Gera a ISO do Andrey OS. Rode num Debian/Ubuntu como root:  sudo ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
    echo "Rode como root: sudo ./build.sh" >&2
    exit 1
fi

echo "==> Instalando ferramentas de build"
apt-get update
apt-get install -y live-build debootstrap xorriso squashfs-tools grub-pc-bin grub-efi-amd64-bin \
                   mtools dosfstools g++ pkg-config libcairo2-dev libx11-dev fonts-inter dpkg-dev jq

chmod +x auto/* config/hooks/live/*.chroot pkg/*.sh

# regenera wallpaper, tela de boot e icones a partir do codigo-fonte da UI
echo "==> Renderizando arte (wallpaper, icones)"
pkg/render-assets.sh
mkdir -p config/bootloaders/grub-pc config/bootloaders/grub-efi
cp pkg/andrey-os-base/root/usr/share/backgrounds/andrey/andrey-os.png config/bootloaders/grub-pc/splash.png
cp pkg/andrey-os-base/root/usr/share/backgrounds/andrey/andrey-os.png config/bootloaders/grub-efi/splash.png

# monta o pacote andrey-os-base (mesmo pacote que as atualizacoes distribuem) e embute na ISO
VERSION="$(tr -d ' \n\r' < VERSION)"
echo "==> Montando andrey-os-base $VERSION"
rm -rf config/includes.chroot/usr/share/andrey-os/pkg
pkg/build-deb.sh "$VERSION" config/includes.chroot/usr/share/andrey-os/pkg

echo "==> Configurando e construindo (leva de 15 a 60 min)"
lb clean --purge || true
lb config
lb build

ls -lh ./*.iso
echo "Pronto. Teste com:  qemu-system-x86_64 -m 4G -enable-kvm -cdrom andrey-os-amd64.hybrid.iso"
