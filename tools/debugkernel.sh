#!/bin/sh
# Build and install a debug kernel (KASAN, lockdep, kmemleak, UBSAN,
# atomic-sleep and list checks) from Debian's own kernel source, then
# make it the default boot entry. Run inside a Debian 13 VM as a sudoer.
# Takes a while; reboot afterwards.
set -eux
# It rewrites grub's default to an unsigned debug kernel. Fine in a
# throwaway VM; on real hardware with Secure Boot, the next boot fails.
if ! systemd-detect-virt -q && [ "${WAVEDEVICE_ON_METAL:-}" != 1 ]; then
	echo "not a VM; set WAVEDEVICE_ON_METAL=1 if you really mean it" >&2
	exit 1
fi
export DEBIAN_FRONTEND=noninteractive
ver=$(uname -r | sed 's/+.*//; s/-.*//')	# e.g. 6.12.111
series=$(echo "$ver" | cut -d. -f1-2)	# e.g. 6.12

sudo apt-get -qq update
sudo apt-get -qq install -y build-essential bc bison flex libssl-dev \
	libelf-dev dwarves rsync cpio kmod dpkg-dev debhelper fakeroot python3 \
	"linux-source-$series"

cd ~
rm -rf "linux-source-$series"
tar xf "/usr/src/linux-source-$series.tar.xz"
cd "linux-source-$series"

cp "/boot/config-$(uname -r)" .config
# Debian's config points at its signing certs, which we don't have.
scripts/config --set-str SYSTEM_TRUSTED_KEYS "" \
	--set-str SYSTEM_REVOCATION_KEYS "" --set-str LOCALVERSION "-wddebug"
lsmod > /tmp/lsmod
yes '' | make LSMOD=/tmp/lsmod localmodconfig >/dev/null
scripts/config -e KASAN -e KASAN_GENERIC -e KASAN_INLINE \
	-e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_KMEMLEAK \
	-e UBSAN -e UBSAN_BOUNDS -e DEBUG_LIST -e DEBUG_OBJECTS \
	-e DETECT_HUNG_TASK -e SOFTLOCKUP_DETECTOR -e HARDLOCKUP_DETECTOR \
	-e DEBUG_INFO_NONE -d DEBUG_INFO_DWARF5 -d DEBUG_INFO_BTF \
	-d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
make olddefconfig
for opt in KASAN PROVE_LOCKING DEBUG_ATOMIC_SLEEP DEBUG_KMEMLEAK UBSAN; do
	grep -q "^CONFIG_$opt=y" .config || { echo "missing $opt" >&2; exit 1; }
done

make -j"$(nproc)" bindeb-pkg >/dev/null
sudo dpkg -i ../linux-image-*wddebug*_*.deb ../linux-headers-*wddebug*_*.deb

kver=$(ls /lib/modules | grep wddebug | head -1)
sudo sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=saved/' /etc/default/grub
sudo update-grub
sudo grub-set-default "gnulinux-advanced-$(sudo grub-probe --target=fs_uuid /)>gnulinux-$kver-advanced-$(sudo grub-probe --target=fs_uuid /)"
echo "installed $kver; reboot to use it"
