# 主题一 · 启动全景：从 BIOS 到 init 的五个阶段

## 你已知的

按下电源键之后发生的事，粗略是：主板固件跑起来 → 找到一块能启动的盘 → 读盘上的
程序 → 一个操作系统开始跑。对 Linux 来说这条链是五段：

```
BIOS/UEFI  →  bootloader  →  内核自解压进场  →  内核初始化  →  1 号进程
 （固件）      （GRUB）        （内核 stub）     （start_kernel）  （execve /init）
```

| 阶段 | 谁在跑 | 干的事 | 本次 QEMU 里是谁 |
|---|---|---|---|
| 1 固件 | BIOS / UEFI | 自检硬件，找可启动设备 | QEMU 内置 SeaBIOS，正常跑 |
| 2 bootloader | GRUB / syslinux | 把内核镜像和 initrd 读进内存，跳到内核入口 | **跳过**：`-kernel` 让 QEMU 按 boot protocol 直接加载 |
| 3 内核进场 | 内核自带的解压 stub | 解压 `bzImage`、建页表、切到 64 位、进 `start_kernel()` | `arch/x86/boot/` 那套代码 |
| 4 内核初始化 | 0 号进程（swapper） | 认内存、配中断、初始化驱动、跑八段 initcall | `start_kernel()` 一整条 |
| 5 交棒用户态 | 1 号进程 `kernel_init` | 找一个能跑的用户态程序 `execve` 它 | busybox 的 `/init` |

## 本次实测的时间轴（镜像 A，自带 `/init`）

| 时刻 | 打点 / 日志 | 说明 |
|---|---|---|
| 0.000s | `start_kernel -> setup_arch` | 开始认硬件、认内存布局 |
| 1.098s | `start_kernel -> rest_init` | 分出 0/1/2 号进程（CPU、内存已可用） |
| 1.288s | `do_basic_setup -> do_initcalls` | 八段 initcall：驱动、文件系统、设备模型 |
| 2.160s | `populate_rootfs: schedule async` | 登记 initramfs 解包（异步） |
| 2.185s | `Unpacking initramfs...` | 把归档铺进 rootfs |
| 2.237s | `Freeing initrd memory: 2148K` | initrd 占的内存还给 buddy |
| 3.794s | `init_eaccess(/init) = 0` | 问一句“内存根里有 /init 吗” |
| 3.866s | `Freeing unused kernel image (initmem) memory: 2856K` | 回收 `__init` 段 |
| 4.163s | `Run /init as init process` | `execve("/init")`，1 号进程进入用户态 |

从 0 到能跑用户程序约 4 秒，其中 2.2 秒之前是“内核收拾自己”，之后才是“找根、找 init”。

## initrd 是怎么交到内核手里的

bootloader（本次是 QEMU）把 initrd 原样放在内存某处，把“地址 + 长度”写进 boot
protocol 规定的交接区；内核早期用 `reserve_initrd_mem()` 把这块内存记进
`memblock`（`memblock_reserve()`），于是有了全局变量 `initrd_start/initrd_end`。
上一站讲的“解压完再把内存还回去”，还的就是这块。

## 这一组文档讲的是第 5 阶段的岔路

第 5 阶段内核要做的第一件事，是判断“1 号进程从哪来”：

- 内存根里有 `/init` → 直接 `execve` 它（旧内核叫 `rdinit`，所以变量名是
  `ramdisk_execute_command`）；
- 没有 `/init` → 去**挂一个真正的根文件系统**（磁盘或网络），再从那里找 init。

后者就是主题 02~07 的内容：磁盘是什么（02）、内存根是什么（03）、岔路怎么判（04）、
怎么挂（05、06）、挂不上会怎样（07）。跨过这条岔路之后，1 号进程就不再是内核线程，
而是用户态的第一个程序——那是“CPU 与进程”领域的话题。
