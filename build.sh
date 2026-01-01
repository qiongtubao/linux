#!/bin/bash

# 清理 + 编译
make clean 
make -j4 bzImage

# 删除cpio 
rm -rf bb_rootfs 
rm -rf bb_initramfs.cpio
mkdir -p bb_rootfs/{bin,sbin,etc,proc,sys,dev}

# 复制静态 busybox
cp /bin/busybox bb_rootfs/bin/

# 创建常用命令链接（可选但推荐）
cd bb_rootfs/bin
for applet in sh ls cat echo ps mount umount dmesg; do
    ln -sf busybox "$applet"
done
cd ../..

cat > bb_rootfs/init <<'EOF'
#!/bin/sh
echo "=== Mounting essential filesystems ==="
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo "=== Welcome to Linux 5.19 (initramfs) ==="
exec /bin/sh
EOF

chmod +x bb_rootfs/init
cd bb_rootfs/
find . | cpio -o --format=newc > ../bb_initramfs.cpio

cd ..

#启动
qemu-system-x86_64   -kernel arch/x86/boot/bzImage   -initrd bb_initramfs.cpio   -appenc