# 主题四 · 实测：dentry 缓存随创建增长，`drop_caches` 清不动它

## 实验设计

在一个 2G 内存的 QEMU 里（避免内存压力干扰），让 init 依次做：

1. 读一次 `/proc/sys/fs/dentry-state`（基线）；
2. `touch` 100 个文件；
3. 再读一次；
4. `ls` 100 个**不存在**的路径（想造“负 dentry”）；
5. 再读一次；
6. `echo 2 > /proc/sys/vm/drop_caches`（手工丢 dentry/inode 缓存）；
7. 最后再读一次。

## 实测数据

`/proc/sys/fs/dentry-state` 的六个字段是
`nr_dentry  nr_unused  age_limit  want_pages  nr_negative  dummy`：

| 阶段 | nr_dentry | nr_unused | nr_negative |
|---|---|---|---|
| 基线 | 621 | 1 | 0 |
| `touch` 100 个文件后 | **721**（+100） | 2 | 0 |
| `ls` 100 个不存在路径后 | 721（不变） | 2 | 0 |
| `drop_caches=2` 之后 | 723 | 3 | 0 |

打点：

```
lookup_open: create <名字>      →  103 次
    100 次来自本次 touch，3 次来自启动时 initramfs 解包（/init、/bin/busybox…）
drop_caches: value=2            →  2 次
```

## 三个能读出来的结论

1. **dentry 缓存与“名字”一一对应**：建 100 个文件，`nr_dentry` 正好 +100。
   每一个名字（不管有没有人打开）都会在内核里留下一个 dentry 对象（大小见
   `/proc/slabinfo` 的 `objsize` 列）。
2. **`nr_unused` 才是“可回收量”**：它始终只有 2~3。`drop_caches` 只回收**无人引用**的
   dentry，所以清完之后 `nr_dentry` 几乎不动（723）。
   把 `nr_dentry` 当成“内存占用”、把 `drop_caches` 当成“清空缓存”，都会误判。
3. **负 dentry 这条路没走通**：`ls /nope1 … /nope100` 之后 `nr_dentry` 与
   `nr_negative` 都没变。可能是 busybox `ls` 对不存在的参数提前返回，也可能是这条
   内核路径对只读查找不缓存负 dentry——**原因未查清，按观察如实记录**。

## 一个关于“打点位置”的教训

最初把打点加在 `vfs_create()` 上，日志里一次都没出现，差点得出“创建没走 VFS”的错误
结论。实际原因是：`open(O_CREAT)` 走的是 `lookup_open()` 里的创建分支，而
`vfs_create()` 只在 `mknod()` 系路径上被调用。

把打点移到 `lookup_open()` 的 `if (open_flag & O_CREAT)` 里之后，立刻看到 103 次。

**教训：打点打在“函数名看起来对”的地方没用，要打在你验证过会被执行的那一行。**

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/dent3 && cp -a bb_rootfs /tmp/dent3
cat > /tmp/dent3/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
echo "=== baseline ==="; cat /proc/sys/fs/dentry-state
i=1; ARGS=""; while [ $i -le 100 ]; do ARGS="$ARGS /d$i"; i=$((i+1)); done
$B touch $ARGS
echo "=== after create ==="; cat /proc/sys/fs/dentry-state
echo 2 > /proc/sys/vm/drop_caches
sleep 1
echo "=== after drop_caches ==="; cat /proc/sys/fs/dentry-state
exec /bin/sh
EOF
chmod +x /tmp/dent3/init
( cd /tmp/dent3 && find . | cpio -o -H newc --quiet ) > /tmp/bb_dent3.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_dent3.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-dent3.log 2>&1
grep -E "dentry-state|^[0-9]+" /tmp/latte-dent3.log
```

## 待解的两个问题

- 为什么建了 100 个文件后 `nr_unused` 仍然只有 2？按代码 `dput()` 到 0 就该进 LRU 才对。
  可能是 rootfs（tmpfs）的 inode 缓存一直持有 alias，或 `nr_unused` 的统计口径与预期
  不同——需要再读 `get_nr_dentry_unused()` 与 `dput()` 的完整路径才能定性。
- 负 dentry 为什么没被统计到（见结论 3）。
