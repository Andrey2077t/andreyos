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
                   mtools dosfstools g++ pkg-config libcairo2-dev libx11-dev fonts-inter

# (opcional) regenera o wallpaper a partir do código-fonte da UI
if [ -f ui/andrey_os.cpp ]; then
    echo "==> Renderizando wallpaper"
    g++ -std=c++17 -O2 ui/andrey_os.cpp -o /tmp/andrey_os $(pkg-config --cflags --libs cairo x11)
    /tmp/andrey_os --bg config/includes.chroot/usr/share/backgrounds/andrey/andrey-os.png
    echo "==> Exportando icones (tema Andrey)"
    /tmp/andrey_os --icons config/includes.chroot/usr/share/icons/Andrey/scalable_tmp
    mkdir -p config/includes.chroot/usr/share/icons/Andrey/scalable
    cp -rf config/includes.chroot/usr/share/icons/Andrey/scalable_tmp/. config/includes.chroot/usr/share/icons/Andrey/scalable/
    rm -rf config/includes.chroot/usr/share/icons/Andrey/scalable_tmp
    cp -f config/includes.chroot/usr/share/backgrounds/andrey/andrey-os.png config/includes.chroot/usr/share/plymouth/themes/andrey/bg.png
    mkdir -p config/bootloaders/grub-pc config/bootloaders/grub-efi
    cp config/includes.chroot/usr/share/backgrounds/andrey/andrey-os.png config/bootloaders/grub-pc/splash.png
    cp config/includes.chroot/usr/share/backgrounds/andrey/andrey-os.png config/bootloaders/grub-efi/splash.png
fi

chmod +x auto/* config/hooks/live/*.chroot

echo "==> Configurando e construindo (leva de 15 a 60 min)"
lb clean --purge || true
lb config
lb build

ls -lh ./*.iso
echo "Pronto. Teste com:  qemu-system-x86_64 -m 4G -enable-kvm -cdrom andrey-os-amd64.hybrid.iso"
