# 主题一 · 终端是什么：tty、行规程、控制字符

## 你已知的

在终端里敲 `ls` 回车，程序才收到一行；按 `Ctrl-C` 命令就中断；按退格键字符会消失。
这些**都不是** shell 干的——是内核里一层叫 **tty（终端）** 的子系统在中间处理。

## 四层结构

```
硬件 UART（0x3f8）
  ↑ 读/写寄存器
串口驱动（8250）
  ↑
tty 核心 + 行规程（line discipline，默认 n_tty）
  ↑ read/write 系统调用
你的程序 / shell
```

`/dev/ttyS0`（4:64）是串口那侧的入口，`/dev/console`（5:1）是“系统控制台”的别名，
`/dev/tty`（5:0）是当前进程的控制终端——三者最终都落到 tty 层。

## 行规程（line discipline）做什么

| 功能 | 例子 | 关掉它的办法 |
|---|---|---|
| 行缓冲（规范模式） | 敲完回车才把整行交给 `read()` | `stty raw` |
| 回显 | 你敲的字符由内核打印回来 | `stty -echo` |
| 行编辑 | 退格（`ERASE`）、`Ctrl-U` 删行（`KILL`） | — |
| 控制字符 → 信号 | `Ctrl-C`→SIGINT、`Ctrl-\`→SIGQUIT、`Ctrl-Z`→SIGTSTP | `stty -isig` |
| 流控 | `Ctrl-S`/`Ctrl-Q` 暂停/继续输出 | `stty -ixon` |

`Ctrl-C` 在很多人的理解里是“发 SIGINT”，更准确的说法是：**行规程在收到字节 0x03 时
发现它等于 `INTR_CHAR`，于是去给“前台进程组”发 SIGINT**。收字节和发信号是两件事，
本次实验正是靠这一点做出对照（主题四）。

## 三个关键概念

| 概念 | 含义 | 本次实测怎么体现 |
|---|---|---|
| tty_struct | 一个终端设备的内核对象 | `/dev/console` 与 `/dev/ttyS0` 是**两个** |
| 前台进程组 | 收到 Ctrl-C 的那个组 | `/proc/<pid>/stat` 的第 8 个字段是 pgrp |
| 控制终端（ctty） | 会话绑定的那个 tty | 没有它，tty 不知道信号该发给谁 |

## 为什么 initramfs 里 sh 会说 `can't access tty`

因为 1 号进程起的 sh **没有建立会话、也没有控制终端**：`setsid()` 不会自动把某个 tty
变成控制终端，要靠 `TIOCSCTTY`（或“session leader 首次 open 一个 tty”这条路径）。

没有控制终端 → tty 的 `ctrl.pgrp` 为空 → 收到 Ctrl-C 时 `tty_get_pgrp()` 返回 NULL →
`__isig()` 什么也不发。命令照样能跑，只是**作业控制失效**，所以 shell 打印那句提示并
关掉 job control。

## 代码位置

| 位置 | 内容 |
|---|---|
| `drivers/tty/n_tty.c:1682` `n_tty_receive_buf_common()` | 收到字节后的第一站（行规程） |
| `drivers/tty/n_tty.c:1339` `n_tty_receive_char_special()` | 控制字符分派 |
| `drivers/tty/n_tty.c:1075` `isig()` | 控制字符 → 信号 |
| `drivers/tty/n_tty.c:1052` `__isig()` | 真正投递（`tty_get_pgrp()` 为空就放弃） |
| `drivers/tty/tty_jobctrl.c` | 会话/控制终端/pgrp 的维护 |
