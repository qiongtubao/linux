# 主题四 · 实测：硬链接让 dentry +1，而 inode 不变

## 实验设计

依次做四件事，每步后读 `/proc/sys/fs/inode-state`、`dentry-state`、
`/proc/slabinfo` 的两行（`shmem_inode_cache`、`inode_cache`）：

1. `touch /a` —— 新建文件
2. `ln /a /b` —— **硬链接**
3. `ln -s /a /c` —— 符号链接
4. `rm /b` —— 删掉一个名字

## 实测数据

| 阶段 | `nr_inodes` | `nr_dentry` | `shmem_inode_cache` active |
|---|---|---|---|
| 基线 | 652 | 622 | 143 |
| `touch /a` | 655 | 624 | 143 |
| **`ln /a /b`（硬链接）** | **655（不变）** | **625（+1）** | 143 |
| `ln -s /a /c`（符号链接） | 656（+1） | 626（+1） | **154（+11）** |
| `rm /b` | 656（不变） | 625（−1） | 154 |

打点与 `ls -l` 的交叉验证：

```
[latte] vfs_link: b (i_nlink=1)      ← 链接前 /a 的 nlink 是 1
-rw-r--r--  2  0  0  ...  /a         ← 链接后两个名字都显示 2
-rw-r--r--  2  0  0  ...  /b
[latte] vfs_unlink: b (i_nlink=2)    ← 删除前是 2
```

## 四个能读出来的结论

1. **硬链接只加名字，不加本体**：`ln /a /b` 之后 `nr_inodes` **完全不变**、
   `nr_dentry` +1，`ls -l` 的链接计数 1→2。这就是“一个 inode 多个 dentry”的直接证据。
2. **符号链接是另一个文件**：`ln -s` 让 `nr_inodes` +1（链接有自己的 inode，内容是
   字符串 `/a`），dentry 也 +1。
3. **`rm` 只减计数**：`rm /b` 后 `nr_inodes` 不变（`/a` 还在，inode 必须活），
   `nr_dentry` −1（`/b` 的 negative dentry 被回收）。
4. **`slabinfo` 不是精确到对象的计数**：符号链接只新增 1 个 inode，`active_objs`
   却从 143 跳到 154（+11）。因为 SLUB 按“整页 slab”统计，新建一个 slab 页会
   一次记上它的全部槽位（11 个）。**读 slabinfo 要看趋势，不要当精确值。**

## 与上一站的联系

上一站留下的问题是“建 100 个文件，为什么 `nr_unused` 几乎不动”。本站给出更可靠的
观察方式：看 `slabinfo` 的 `active_objs` 与 `inode-state` 的 `nr_inodes`，
而不是 `nr_unused`。至于 `nr_unused` 的统计口径，仍需要读
`get_nr_dentry_unused()` 与 `d_lru_add()` 的完整路径才能定性——**没有查清就不下结论。**

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/inodetest && cp -a bb_rootfs /tmp/inodetest
cat > /tmp/inodetest/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
show() {
  echo "--- $1"
  cat /proc/sys/fs/inode-state
  cat /proc/sys/fs/dentry-state
  $B sed -n '/^dentry /p;/^shmem_inode_cache /p' /proc/slabinfo
}
show baseline
$B touch /a;      $B ls -l /a;      show "after /a"
$B ln /a /b;      $B ls -l /a /b;   show "after /b hardlink"
$B ln -s /a /c;   $B ls -l /c;      show "after /c symlink"
$B rm /b;         $B ls -l /a;      show "after rm /b"
exec /bin/sh
EOF
chmod +x /tmp/inodetest/init
( cd /tmp/inodetest && find . | cpio -o -H newc --quiet ) > /tmp/bb_inodetest.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_inodetest.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-inode.log 2>&1
grep -E "vfs_link:|vfs_unlink:|^--- |^[0-9]+\s" /tmp/latte-inode.log
```
