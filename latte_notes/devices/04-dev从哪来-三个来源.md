# 主题四 · `/dev` 从哪来：三个来源，各管一段

## 你已知的

Linux 里 `/dev/sda`、`/dev/null`、`/dev/ttyS0` 都是“文件”。它们从哪来？谁建的？
答案不是一个来源，而是三个，按“从早到晚”接力。

## 来源一：打包在 initramfs 里（静态节点）

内核自带一份极小的 builtin initramfs，里面就写着：

```
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /root 0700 0 0
```

`nod` 表示“设备节点”：字符设备（c）、主 5、次 1。它被解压进内存根之后，
**内核在完全没有用户态的情况下就已经有一个可用的 `/dev/console`**——这就是
`console_on_rootfs()` 能打开它、把 0/1/2 接到终端的原因。

同理，你打包 initramfs 时放的 `/dev` 节点都是这一类：它们不依赖任何驱动是否就绪，
纯粹是“一个记着设备号的文件”。

## 来源二：devtmpfs（内核自动建节点）

设备热插拔、驱动 probe 成功后，内核希望 `/dev` 里**自动**出现节点。做这件事的是
`devtmpfs`：

```
设备注册（device_add）
 └─ devtmpfs_create_node()      ← 产生一条请求
     └─ kdevtmpfs 内核线程      ← devtmpfsd()，从队列取请求
         └─ handle_create()     ← vfs_mknod(nodename, mode, dev->devt)
```

挂载点在 `devtmpfs_mount()`：它用**相对路径** `"dev"` 调 `init_mount()`，所以
挂到哪儿取决于当时的 cwd——`prepare_namespace()` 里 cwd 是 `/root`，于是落到新根的
`/dev`；而在 initramfs 场景下 cwd 是 `/`，就落到内存根的 `/dev`。

一个关键细节：`devtmpfs` 的节点**由内核线程创建**，所以不依赖 udev 是否在跑。
这也是为什么很多最小系统（包括本系列的 busybox initramfs）不装 udev，
`/dev/ttyS0`、`/dev/null` 也照样存在。

## 来源三：udev / mdev（用户态“美颜”）

devtmpfs 只保证“节点存在、设备号正确”。真实发行版还需要：

- 改名：`/dev/disk/by-uuid/xxxx`、`/dev/disk/by-label/...`（符号链接）；
- 权限与属主：`/dev/kvm` 要进 kvm 组、`/dev/snd/*` 要进 audio 组；
- 触发加载：设备出现时跑规则、执行程序。

这些都需要用户态（systemd-udevd 或 busybox 的 mdev）。本系列的最小 rootfs 没有它，
所以 `/dev` 里只有 devtmpfs 给的“素颜”节点。

## 三者关系

| 来源 | 谁执行 | 何时 | 依赖 | 本次是否有 |
|---|---|---|---|---|
| initramfs 里的静态节点 | 内核解包 cpio | 解压 initramfs 时 | 无 | ✅ `/dev/console` |
| devtmpfs | 内核线程 `kdevtmpfs` | 设备注册时 | 无 | ✅（`/dev/ttyS0` 等） |
| udev / mdev | 用户态守护进程 | 用户态起来后 | 需要 udevd/mdev | ❌ 没装 |

## 实测：诊断 init 里的 `/dev`（111 项）

让 init 自己 `ls -l /dev`，能看到 devtmpfs 生成的真实节点（节选）：

```
crw------- 1 0 0    5,   1  console     ← builtin initramfs 带的那一个（5:1）
crw------- 1 0 0    4,  64  ttyS0       ← 8250 驱动 probe 后由 devtmpfs 建（4:64）
crw-rw-rw- 1 0 0    1,   3  null
crw-rw-rw- 1 0 0    1,   8  random
crw-rw-rw- 1 0 0    5,   0  tty
brw------- 1 0 0    7,   0  loop0
brw------- 1 0 0   11,   0  sr0
```

`/proc/mounts` 印证了挂载点：

```
none /dev devtmpfs rw,relatime,size=1009404k,nr_inodes=252351,mode=755 0 0
```

也就是说：`console` 这类内核早期就要用的节点是**打包带的**，`ttyS0`、`loop0`、`sr0`
这类是**驱动出现后由 devtmpfs 自动建的**，而权限/别名（`/dev/disk/by-uuid`）
才是 udev 的活——本 rootfs 没装 udev，所以只有“素颜”节点。

## 代码位置

| 位置 | 内容 |
|---|---|
| `usr/default_cpio_list` | 静态节点 `/dev/console`（c 5 1） |
| `drivers/base/devtmpfs.c:443` `devtmpfsd()` | `kdevtmpfs` 线程主循环 |
| `drivers/base/devtmpfs.c:222` `handle_create()` | 真正 `vfs_mknod()` 的地方 |
| `drivers/base/devtmpfs.c:369` `devtmpfs_mount()` | 挂载 devtmpfs（相对路径 `dev`） |
| `init/main.c:1627` `console_on_rootfs()` | 打开 `/dev/console` 并 dup 到 0/1/2 |
