rm ./ext2-lite.img
make clean && make
insmod ./ext2-lite.ko
touch ext2-lite.img && truncate -s 128M ext2-lite.img
mkfs.ext2 -b 1024 -L "ext2-lite fs" -O none -m 0 ./ext2-lite.img
mount -t ext2-lite -o loop ./ext2-lite.img /mnt/testdisk
