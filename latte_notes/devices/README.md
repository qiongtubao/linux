# 设备与中断：我们的日志、`/dev` 和 console 从哪来

前几站的证据全部来自串口日志。这一站反过来问：**这些日志本身是怎么出来的？
`/dev` 下的文件是谁建的？** 答案是三条链：printk → console → UART、
earlycon 的临时工交接、devtmpfs 的自动建节点。
1 份索引 + 6 篇主题文档，配 6 个一次性打点与代码中文注释。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 设备 | 三件套 | [01-设备三件套-设备号节点驱动](01-设备三件套-设备号节点驱动.md) | 设备号（主:次）+ 设备节点 + 驱动，节点只是设备号的载体 |
| 设备 | 日志出口 | [02-一次printk的旅程](02-一次printk的旅程.md) | ring buffer → console 名单 → 8250 轮询写 UART |
| 设备 | 早期输出 | [03-early-console-临时工](03-early-console-临时工.md) | `CON_BOOT` 临时工，真 console 一来就下台 |
| 设备 | `/dev` 来源 | [04-dev从哪来-三个来源](04-dev从哪来-三个来源.md) | initramfs 静态节点 / devtmpfs / udev，各管一段 |
| 设备 | console 与 tty | [05-console与tty与dev-console](05-console与tty与dev-console.md) | `ttyS0`(4:64) 是硬件、`console`(5:1) 是策略、`tty`(5:0) 是进程视角 |
| 设备 | 实测 | [06-实测-三台启动三种看得见](06-实测-三台启动三种看得见.md) | A 正常 / B 只 earlyprintk / C 诊断 init |

## 一条链

```
printk("...")
 └─ vprintk_store()        写环形缓冲区（任意上下文都安全）
     └─ console_flush_all()        逐个 console 输出
         ├─ earlyser (CON_BOOT)    轮询 UART，早期临时工
         └─ ttyS0   (CON_ENABLED)  8250 驱动，正式工
用户态 echo "..." → /dev/console(5:1) → tty 层 → 同上一个 UART
/dev 里的节点：initramfs 静态 nod + devtmpfs(kdevtmpfs) 自动 mknod
```

## 三次启动的关键差异

| | A 正常 | B 只 earlyprintk | C 诊断 init |
|---|---|---|---|
| 串口有没有日志 | 全程 | 第 396 行后静默 | 全程 |
| 首个 console | `earlyser`(0x9) | `earlyser`(0x9) | 同 A |
| 真 console | `ttyS0`(0x11) | `tty0`(VGA，0x1) | `ttyS0` |
| 额外收获 | 全链路日志 | earlycon 交接反例 | `/dev` 111 项、`/proc/consoles` |

## 打点清单（6 个，全在一次性初始化路径上）

| 打点 | 位置 | 说明 |
|---|---|---|
| `early console registered: %s (keep=%d)` | `arch/x86/kernel/early_printk.c` | 临时工上岗 |
| `register_console: %s%d flags=0x%x` | `kernel/printk/printk.c` | 每个 console 注册一次 |
| `console_init: run __con_initcall section` | `kernel/printk/printk.c` | 驱动初始化开始 |
| `devtmpfsd: kdevtmpfs thread running` | `drivers/base/devtmpfs.c` | 自动建节点的线程 |
| `devtmpfs_mount: mount_dev=%d` | `drivers/base/devtmpfs.c` | 挂 devtmpfs |
| `console_on_rootfs: /dev/console -> fd 0/1/2` | `init/main.c` | 用户态 0/1/2 接控制台 |

**故意不在 `printk()` 热路径打点**：在 `vprintk_store()` 里 `printk` 会自我递归。
这一站的取巧办法是“初始化点打点 + 用户态诊断程序取证”。

## 自查 3 问

1. 为什么 `printk` 不直接写串口？（02：可能在中断/NMI/panic 里调用，同步写设备会卡死）
2. `earlyprintk=serial` 的日志为什么在真 console 注册后消失？（03：`CON_BOOT` 临时工被撤，
   若真 console 是 VGA 就看不见了）
3. `/dev/console` 与 `/dev/ttyS0` 的关系？（05：前者是策略别名 5:1，后者是硬件 4:64）

## 下一阶段

还能继续往下钻的两处：**中断**（UART 收数据靠 IRQ，`request_irq` → IDT → 处理函数）与
**tty 层**（行编辑、`Ctrl-C` 怎么变成信号）。也可以回到内存看这些页的回收。
