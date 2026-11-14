# 主题四 · 实测：`st_dev` 在挂载点两侧不同

## 实验设计

1. 读 `/` 与 `/mnt` 的 `st_dev`/`st_ino`；
2. `mount -t tmpfs -o size=8M none /mnt`；
3. 再读 `/mnt`，并看 `/proc/self/mountinfo` 里多出来的那一行；
4. 在 `/mnt` 里建个文件，看它的 `st_dev`；
5. `umount /mnt`，确认回到原样。

## 实测输出

```
=== before ===
/      dev=2 ino=1
/mnt   dev=2 ino=20
=== mount tmpfs on /mnt ===
mount-rc=0
/mnt   dev=15 ino=1
24 2 0:21 / /mnt rw,relatime - tmpfs none rw,size=8192k
=== file inside the mount ===
/mnt/x dev=15 ino=2
=== umount ==="
umount-rc=0
/mnt   dev=2 ino=20
```

打点：

```
[latte] do_new_mount: none -> tmpfs
[latte] do_umount: flags=0x0
```

## 怎么读这些数字（一个坑）

`busybox stat -c %D` 输出的设备号是**十六进制**：

| `stat %D` | 十进制 | mountinfo 里的 `主:次` |
|---|---|---|
| `2` | 2 | rootfs 的匿名设备 |
| `15` | **21** | `0:21` ← 与 mountinfo 完全一致 |

如果不注意进制，会以为 `dev=15` 和 `0:21` 对不上。

## 四个能读出来的结论

1. **挂载点两侧属于不同 superblock**：`/mnt` 的 `st_dev` 从 `0x2` 变成 `0x15`（21），
   `/mnt/x` 继承新设备号。这是“跨挂载点”最硬的证据。
2. **`/mnt` 的 inode 号也变了**（20 → 1）：因为解析到它时换成了 tmpfs 的根目录，
   而 tmpfs 根目录的 inode 号是 1。
3. **mountinfo 一行能逐列读**：`24 2 0:21 / /mnt rw,relatime - tmpfs none rw,size=8192k`
   = 本挂载 ID 24、父 ID 2、设备 0:21、sb 内根 `/`、挂载点 `/mnt`、选项 `rw,relatime`、
   类型 tmpfs、源 none、sb 选项 `rw,size=8192k`。
4. **`umount` 是“揭开盖子”而不是“删掉目录”**：`/mnt` 的 `st_dev`/`st_ino` 回到
   `0x2`/20，原来那个空目录又露出来了。

## 顺便看到的内核自己的挂载

打点显示启动时内核也走了同一条路：

```
do_new_mount: devtmpfs -> devtmpfs     ← 1.30s，内核自己挂 /dev
do_new_mount: none -> proc             ← 4.24s，/init 里的 mount -t proc
do_new_mount: none -> sysfs
do_new_mount: none -> devtmpfs
```

所以 `/proc`、`/sys`、`/dev` 并不是“内核内置的目录”，而是**被挂进这棵树的文件系统**。
这条也解释了 `devices/04` 里 `/dev` 为什么能有 111 个自动生成的节点。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/mounttest && cp -a bb_rootfs /tmp/mounttest
cat > /tmp/mounttest/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
$B stat -c '%n dev=%D ino=%i' /
$B mkdir -p /mnt
$B stat -c '%n dev=%D ino=%i' /mnt
mount -t tmpfs -o size=8M none /mnt
$B stat -c '%n dev=%D ino=%i' /mnt
$B sed -n '/\/mnt/p' /proc/self/mountinfo
$B touch /mnt/x
$B stat -c '%n dev=%D ino=%i' /mnt/x
$B umount /mnt
$B stat -c '%n dev=%D ino=%i' /mnt
exec /bin/sh
EOF
chmod +x /tmp/mounttest/init
( cd /tmp/mounttest && find . | cpio -o -H newc --quiet ) > /tmp/bb_mounttest.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_mounttest.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-mount.log 2>&1
grep -E "do_new_mount|do_umount|dev=|mountinfo|tmpfs" /tmp/latte-mount.log
```
