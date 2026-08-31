

make O=build mt7621_rfb_defconfig
make O=build menuconfig
make O=build -j$(nproc)

make O=build CROSS_COMPILE=mipsel-linux-gnu- -j$(nproc)

python3 mytools/update_defines.py

cp build/u-boot-mt7621.bin /mnt/d/document/github/hlk/SNANDer/flash/u-boot-mt7621.bin

.\Droid-MAX_SNANDer\src\snander.exe -S 10M -a 0x000000 -l 0x7D000 -e

