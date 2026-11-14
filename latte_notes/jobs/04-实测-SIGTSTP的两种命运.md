# 主题四 · 实测：同一句 `kill -TSTP`，两种命运

## 两次启动，只差“有没有控制终端”

| | A：init 直接跑 | B：`setsid` + 控制终端在 `/dev/ttyS0` |
|---|---|---|
| `set -m` | 失败：`can't access tty; job control turned off` | `rc=0` |
| 子进程与 shell 的进程组 | **同一个** | 子进程单独一个 |
| 该进程组是否孤儿 | **是** | 否 |
| `kill -STOP` | `state=T`（打点 `signr=19`） | —— |
| `kill -TSTP` | **`state` 仍是 S，没有打点** | **`state=T`（打点 `signr=20`）** |
| `kill -CONT` | 回到 `S` | 回到 `S` |
| `kill -KILL` + `wait` | `wait-rc=137` | —— |

## A 的原始输出（无控制终端）

```
=== set -m ===
/init: set: line 8: can't access tty; job control turned off
setm-rc=0
--- fresh ---
  state=S
--- SIGSTOP (不可捕获) ---
  state=T
[latte] do_signal_stop: signr=19 comm=busybox
--- SIGCONT ---
  state=S
--- SIGTSTP (可捕获/组停止) ---
  state=S              ← 没停！而且没有任何 do_signal_stop 打点
--- SIGKILL ---
wait-rc=137
```

## B 的原始输出（有控制终端）

```
=== with controlling tty ===
setm-rc=0
  state=S
--- kill -TSTP ---
  state=T
[latte] do_signal_stop: signr=20 comm=sleep
--- kill -CONT ---
  state=S
```

## 为什么 A 里 `SIGTSTP` 会“消失”

不是没发出去，是**内核按规则丢弃了**（`kernel/signal.c:2974` 附近的原文）：

```c
/* The job control signals do nothing in an orphaned pgrp,
 * but SIGSTOP always works. */
if (signr != SIGSTOP) {
	...
	if (is_current_pgrp_orphaned())
		goto relock;
}
```

链条是：

```
没有控制终端
 → set -m 失败（shell 无法建新进程组并把它设为前台）
     → 子进程留在 shell 的进程组里
         → 这个进程组是“孤儿”（shell 的父进程不在同一会话）
             → SIGTSTP/SIGTTIN/SIGTTOU 被丢弃
                 → 只有 SIGSTOP 能停下它
```

这种“丢弃”是有意设计：孤儿进程组没人能 `fg` 回来，停了就永远停住。

## 打点怎么帮我们判断

```
A: do_signal_stop: signr=19 comm=busybox     ← 只有 SIGSTOP 进来过
B: do_signal_stop: signr=20 comm=sleep       ← SIGTSTP 也进来了
```

`19` = SIGSTOP、`20` = SIGTSTP。**同一行打点、不同的编号，直接区分了两种信号的命运。**

## 一个踩过的坑（值得记住）

第一次把 `exec </dev/ttyS0 >/dev/ttyS0 2>&1` 写在 `mount -t devtmpfs` 之前，
`/dev/ttyS0` 还不存在 → 重定向失败 → 非交互 shell 直接退出 → **1 号进程死亡**：

```
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000100
```

`0x100 >> 8 = 1`，就是脚本的退出码。**init 脚本里任何一次“让 shell 退出”的错误都会变成
panic**，所以 `exec` 重定向、`cd`、`source` 都要放在设备就绪之后。

## 复现（B：有控制终端，最短路径）

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/jobtest4 && cp -a bb_rootfs /tmp/jobtest4
cat > /tmp/jobtest4/init <<'SH'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc; mount -t sysfs none /sys; mount -t devtmpfs none /dev
$B setsid $B sh -c '
exec </dev/ttyS0 >/dev/ttyS0 2>&1
set -m; echo "setm-rc=$?"
sleep 30 &
P=$!
awk "{print \"  state=\"\$3}" /proc/$P/stat
kill -TSTP $P; sleep 1
awk "{print \"  state=\"\$3}" /proc/$P/stat
kill -CONT $P; sleep 1
awk "{print \"  state=\"\$3}" /proc/$P/stat
kill -KILL $P
'
exec /bin/sh
SH
chmod +x /tmp/jobtest4/init
( cd /tmp/jobtest4 && find . | cpio -o -H newc --quiet ) > /tmp/bb_jobtest4.cpio
timeout 45 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_jobtest4.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-job4.log 2>&1
grep -E "state=|do_signal_stop|setm-rc" /tmp/latte-job4.log
```

期望：`setm-rc=0`，状态依次 `S → T → S`，打点 `signr=20`。
