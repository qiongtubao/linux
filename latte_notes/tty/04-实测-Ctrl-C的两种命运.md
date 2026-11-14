# 主题四 · 实测：Ctrl-C 的两种命运

三次启动，只改“控制终端建在哪里”，看 `Ctrl-C` 是否生效。输入通过 QEMU 的 stdio
喂给串口（`-nographic`），脚本定时发送 `x` 和 `\003`（Ctrl-C）。

## 实验设计

```bash
{ sleep 6; printf 'x'; sleep 1; printf '\003'; sleep 5;
  printf 'x'; sleep 1; printf '\003'; sleep 6; } \
  | timeout 45 qemu-system-x86_64 ... -nographic
```

`init` 里依次做三件事：直接 `cat`（无 ctty）→ `setsid` 后把 ctty 建在
`/dev/console` → 再把 ctty 建在 `/dev/ttyS0`。

## 实测结果

| 场景 | 行规程打点 | `cat` 结局 |
|---|---|---|
| 无控制终端 | `isig: sig=2` → `__isig: sig=2 pgrp=none` | 只被 `timeout` 杀掉：`p1-rc=143` |
| ctty 在 `/dev/console` | 同上：`pgrp=none` | 收不到信号，停在 `cat` |
| ctty 在 `/dev/ttyS0` | `__isig: sig=2 pgrp=set` | `Ctrl-C` 杀掉：`p2b-rc=130` |

关键日志（第三次，成功那次）：

```
[    4.551528] [latte] n_tty_rx: 1 bytes      ← 收到 'x'
x                                             ← 回显
[    5.551462] [latte] n_tty_rx: 1 bytes      ← 收到 0x03
[    5.551983] [latte] isig: sig=2            ← 认成 INTR
[    5.552376] [latte] __isig: sig=2 pgrp=set ← 有前台进程组，投递
^Cp2b-rc=130                                  ← 128 + 2
```

第一次（无 ctty）的同一段：

```
[latte] isig: sig=2
[latte] __isig: sig=2 pgrp=none               ← 同样是 Ctrl-C，但无处可投
p1-rc=143                                     ← 只能等 timeout(SIGTERM)
```

## 三个可以直接读出来的结论

1. **`isig` 与 `pgrp` 是两件事**：`isig` 每次都会触发（行规程确实认出了 0x03），
   差别只在 `tty_get_pgrp()` 是否为 NULL。所以“Ctrl-C 没反应”不是键盘/驱动问题，
   而是**没有前台进程组**。
2. **`rc=130` 是“被 SIGINT 杀死”的指纹**（128+2），`rc=143` 是 `timeout` 的 SIGTERM
   （128+15）。shell 里判断“命令是被 Ctrl-C 停的吗”就看这个。
3. **控制终端必须建在真正收字符的那个 tty 上**：`/dev/console`（5:1）只是别名，
   字符实际从 `/dev/ttyS0`（4:64）进来，两个 `tty_struct` 各记各的前台进程组。

## 复现（成功那一次，最短路径）

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/ttytest2 && cp -a bb_rootfs /tmp/ttytest2
cat > /tmp/ttytest2/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
echo "=== phase2b setsid ctty=ttyS0 ==="
$B setsid $B sh -c 'exec </dev/ttyS0 >/dev/ttyS0 2>&1; exec /bin/cat'
echo "p2b-rc=$?"
echo "=== ttytest2 done ==="
exec /bin/sh
EOF
chmod +x /tmp/ttytest2/init
( cd /tmp/ttytest2 && find . | cpio -o -H newc --quiet ) > /tmp/bb_ttytest2.cpio
{ sleep 6; printf 'x'; sleep 1; printf '\003'; sleep 5; } \
  | timeout 30 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
      -initrd /tmp/bb_ttytest2.cpio \
      -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
      -nographic -m 2G -no-reboot > /tmp/latte-tty2.log 2>&1
```

期望在日志里看到 `pgrp=set` 与 `p2b-rc=130`。
