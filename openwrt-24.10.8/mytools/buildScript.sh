

make mt7621_rfb_defconfig
make menuconfig
make -j$(nproc)

# make CROSS_COMPILE=mipsel-linux-gnu- -j$(nproc)

# python3 mytools/update_defines.py

cp bin/targets/ramips/mt7621/openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin /mnt/d/document/github/hlk/SNANDer/flash/openwrt-ramips-mt7621-hilink_hlk-7621a-evb-initramfs-kernel.bin
cp bin/targets/ramips/mt7621/openwrt-ramips-mt7621-hilink_hlk-7621a-evb-squashfs-sysupgrade.bin /mnt/d/document/github/hlk/SNANDer/flash/openwrt-ramips-mt7621-hilink_hlk-7621a-evb-squashfs-sysupgrade.bin

# .\Droid-MAX_SNANDer\src\snander.exe -S 10M -a 0x000000 -l 0x7D000 -e

