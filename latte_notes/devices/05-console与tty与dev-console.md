# 主题五 · console、tty 与 `/dev/console`：三样东西别混

## 你已知的

`/dev/console`、`/dev/tty`、`/dev/ttyS0` 看起来都是“终端”，但它们不是同一个层次的东西。

## 四层结构

```
硬件 UART（0x3f8）
  ↑  写寄存器
8250 驱动（uart_port）
  ↑
tty 层（tty_struct / line discipline）  ← 行编辑、信号（Ctrl-C）、作业控制
  ↑
console 抽象（struct console）           ← 内核日志的出口
  ↑
/dev/console (5:1)  /dev/tty (5:0)  /dev/ttyS0 (4:64)   ← 用户态看到的三个节点
```

| 节点 | 设备号 | 是什么 |
|---|---|---|
| `/dev/ttyS0` | `4:64` | 具体串口设备，读写它直接到 UART |
| `/dev/tty` | `5:0` | “当前进程的控制终端”，随进程不同而不同 |
| `/dev/console` | `5:1` | **系统控制台**：由 `console=` 选出的首选 console 的别名 |

## `console=` 选的是“首选 console”

`console=ttyS0` 的意思是：把 ttyS0 注册成首选 console——内核日志走它，
`/dev/console` 也指向它。证据来自本次诊断 init 读的 `/proc/consoles`：

```
ttyS0                -W- (EC    a)    4:64
```

逐段读（格式见 `fs/proc/consoles.c:56`）：

| 字段 | 值 | 含义 |
|---|---|---|
| 名字 | `ttyS0` | console 名 + 序号 |
| `-W-` | 读? 写? unblank? | 不能读、能写、无 unblank 回调 |
| `(EC    a)` | 标志位 | `E`=ENABLED、`C`=CONSDEV（首选）、`a`=ANYTIME |
| `4:64` | 设备号 | 就是 `/dev/ttyS0` |

名单里只有一行，说明**真 console 现在只有一个**（earlyser 已下台，VGA 的 tty0 在
`console=ttyS0` 下不是首选）。

## `console_on_rootfs()`：把 `/dev/console` 接到 0/1/2

```c
struct file *file = filp_open("/dev/console", O_RDWR, 0);
init_dup(file);   /* fd 0 = stdin  */
init_dup(file);   /* fd 1 = stdout */
init_dup(file);   /* fd 2 = stderr */
```

所以在 `/init` 脚本里 `echo` 的输出会出现在串口上——它不是“内核日志”，而是用户态
往 `/dev/console` 写，经 tty 层落到同一个 UART。

## 为什么 sh 会说 `can't access tty; job control turned off`

`can't access tty` 说的是**没有控制终端（controlling tty）**：shell 想做作业控制
（Ctrl-Z、`fg`/`bg`）需要它。这不代表输出没地方去——0/1/2 已经接到
`/dev/console` 了，只是这个终端没有和进程组绑定成一个“控制终端”。

本次启动里这句话出现，正是因为 initramfs 里的 sh 是在没有 `setsid` + `TIOCSCTTY`
的情况下被拉起来的。

## 三个节点的关系，一句话

- `ttyS0`（4:64）是**硬件**；
- `console`（5:1）是**策略**（日志和系统控制台走哪）；
- `tty`（5:0）是**进程视角**的当前终端。

## 代码位置

| 位置 | 内容 |
|---|---|
| `init/main.c:1627` `console_on_rootfs()` | `/dev/console` → 0/1/2 |
| `fs/proc/consoles.c:56` | `/proc/consoles` 的格式（`-W- (EC a)` 怎么来的） |
| `include/linux/console.h:190` | `CON_ENABLED` / `CON_CONSDEV` / `CON_BOOT` 等标志 |
| `drivers/tty/tty_io.c` | tty 层：`tty_open`、作业控制、行规程 |
