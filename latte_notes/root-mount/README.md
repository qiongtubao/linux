# 根文件系统切换：`/init` 不存在时内核怎么办

上一站（`latte_notes/initramfs-rootfs.md`）讲清了 `/init` 是怎么被解进 rootfs 的。
这一站反过来问：**如果 initramfs 里没有 `/init`，内核怎么办？** 答案是
`prepare_namespace()` → `mount_root()` 去挂一个真正的根；挂不上就 panic。
6 个主题、每个主题一份文档，代码注释与打点同步落地。

## 主题划分（按依赖顺序读）

| # | 主题 | 文档 | 一句话 | 依赖 |
|---|---|---|---|---|
| 1 | 岔路口 | [01-rdinit-gate.md](01-rdinit-gate.md) | `init_eaccess("/init")` 一行返回值决定两条路 | — |
| 2 | 挂根的固定动作 | [02-prepare-namespace.md](02-prepare-namespace.md) | `prepare_namespace()` 里六个“该等的先等” | 1 |
| 3 | `ROOT_DEV` 分派 | [03-mount-root-dispatch.md](03-mount-root-dispatch.md) | 网络/通用/块设备三条挂法 + 两轮重试 | 2 |
| 4 | 老式 initrd | [04-old-initrd-path.md](04-old-initrd-path.md) | `initrd_load()` 为什么这次是空转 | 2 |
| 5 | 换根三步 | [05-switch-root-steps.md](05-switch-root-steps.md) | `devtmpfs` + `MS_MOVE` + `chroot` 把新根变成 `/` | 3 |
| 6 | 失败面 | [06-failure-modes.md](06-failure-modes.md) | “挂不上根”和“没有 init”是两条 panic | 3、5 |

依赖关系：01 是唯一入口；02 是它唯一的下游；03/04 是 02 的两个分支；05 是 03 的
成功收尾；06 是 03 的失败收尾。所以乱序读也只需要记住这一条主线。

## 全景

```
kernel_init_freeable()
 └─ init_eaccess("/init")                ← 只判存在 + 可访问
     ├─ 0     → 什么都不做，稍后 kernel_init() execve /init
     └─ != 0  → ramdisk_execute_command = NULL
                 └─ prepare_namespace()
                      ├─ root_delay / wait_for_device_probe / md_run_setup
                      ├─ parse_root_device(root=) → ROOT_DEV
                      ├─ initrd_load()      ← BLK_DEV_RAM=n 时恒 false
                      ├─ wait_for_root()    ← rootwait 才有
                      ├─ mount_root()       ← 按 ROOT_DEV 分派，挂不上就 panic
                      └─ out: devtmpfs_mount / MS_MOVE / init_chroot
```

## 实测对照（同一内核、两个 initramfs）

镜像 A = `bb_initramfs.cpio`（自带 `/init`，2195968 字节）；
镜像 B = 从同一份 rootfs 去掉 `init` 重新打包（2195456 字节）。

| 观察点 | A | B |
|---|---|---|
| `init_eaccess(/init)` | `0` | `-2`（ENOENT） |
| `check access ... ignoring` 警告 | 无 | 有 |
| `prepare_namespace()` | **未调用**（0 条打点） | 调用（9 条打点） |
| 结局 | `Run /init as init process` → busybox shell | `VFS: Cannot open root device ""` → panic |
| 时间线 | 3.79s 探路 → 4.16s execve | 3.57s 探路 → 3.80s panic |
| `devtmpfs + switch root` 打点 | 无（没走这条路） | 无（panic 在它之前） |

**一行返回值，两条启动路径**——这就是这一站最想传达的东西。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
CMD="console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1"

timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd bb_initramfs.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-rootA.log 2>&1

rm -rf /tmp/noinit && cp -a bb_rootfs /tmp/noinit && rm -f /tmp/noinit/init
( cd /tmp/noinit && find . | cpio -o -H newc --quiet ) > /tmp/bb_noinit.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd /tmp/bb_noinit.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-rootB.log 2>&1
```

`panic=1` + `-no-reboot` 让 B 的 panic 自己退出 QEMU，不用等超时。

## 打点清单（10 个）

| 打点 | A | B |
|---|---|---|
| `init_eaccess(%s) = %d` | 1（`= 0`） | 1（`= -2`） |
| `prepare_namespace: enter real-root path` | — | 1 |
| `prepare_namespace: root=%s` | — | 1（`(none)`） |
| `prepare_namespace: initrd_load` | — | 1 |
| `initrd_load: mount_initrd=%d ROOT_DEV=%u` | — | 1（`1` / `0`） |
| `prepare_namespace: mount_root` | — | 1 |
| `mount_root: ROOT_DEV=%u` | — | 1（`0`） |
| `mount_block_root: /dev/root` | — | 1 |
| `mount_root_generic: ROOT_DEV=%u` | — | 1（`0`） |
| `prepare_namespace: devtmpfs + switch root` | — | — |

最后一行两列都空：A 没走这条路，B 在上一行之后 panic 了。**打点链断在哪里，就是
死在哪里。**

## 自查 3 问

1. 有 `/init` 时，`prepare_namespace()` 为什么一次都不被调用？——因为调用它的条件
   就是 `init_eaccess("/init") != 0`（主题一）。
2. 本次 `initrd_load()` 打了点却什么也没做，为什么？——`CONFIG_BLK_DEV_RAM` 没开，
   `rd_load_image()` 是 `return 0` 的空桩（主题四）。
3. 怎么从日志区分“没有根”和“没有 init”？——前者是 `VFS: Unable to mount root fs`，
   后者是 `No working init found`（主题六）。

## 下一阶段

真根挂上之后接棒的是根文件系统里的 `/sbin/init`（systemd/sysvinit/脚本），那是
用户态的故事；内核侧还有一个方向是回到 mm 看这些页之后怎么被回收。
