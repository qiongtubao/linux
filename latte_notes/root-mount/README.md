# 根文件系统切换：一次“没有 `/init`”的开机

上一站（`latte_notes/initramfs-rootfs.md`）讲 `/init` 是怎么被解进内存根的。这一站
反过来问：**万一内存根里没有 `/init`，内核怎么办？** 答案是一条岔路：去挂真正的根
（磁盘/网络），挂不上就 panic。

本页只做索引与实验记录，概念细节在 7 篇主题文档里，每篇都按
“你已知的概念 → 内核里对应什么 → 代码在哪 → 实测证据”展开。

## 主题划分（按领域）

| 领域 | 主题 | 文档 | 一句话 |
|---|---|---|---|
| 启动 | 全景 | [01-启动-从BIOS到init](01-启动-从BIOS到init.md) | 开机的五个阶段 + 本次实测时间轴 |
| 磁盘与文件系统 | 磁盘、分区、文件系统、根 | [02-磁盘-块设备分区与根](02-磁盘-块设备分区与根.md) | 块设备/超级块/挂载/`root=` 到底是什么 |
| 内存 | 内存里的根 | [03-内存-ramfs与initramfs](03-内存-ramfs与initramfs.md) | 为什么 `/` 一开始在内存里，不靠磁盘 |
| 启动 | 岔路 | [04-岔路-有init还是没init](04-岔路-有init还是没init.md) | `init_eaccess("/init")` 一行返回值决定两条路 |
| 磁盘与文件系统 | 挂真根 | [05-挂真根-等设备与重试](05-挂真根-等设备与重试.md) | 六个等待 + `ROOT_DEV` 分派 + 两轮重试 |
| 启动 | 换根 | [06-换根-新根变成斜杠](06-换根-新根变成斜杠.md) | `MS_MOVE` + `chroot` 把新根变成 `/` |
| 启动 + 磁盘 | 失败面与历史包袱 | [07-失败面与老式initrd](07-失败面与老式initrd.md) | 两条 panic；老式 initrd 为何空转 |

读法建议：01 先建立时间感，02、03 补“磁盘 vs 内存”的常识，04 是分水岭，
05、06 是分水岭右侧的两步，07 是右侧的失败面。

## 全景

```
BIOS/UEFI → bootloader → 内核进场 → 内核初始化（内存/驱动/initcall）
                                              ↓
                                    initramfs 解进内存根（上一站）
                                              ↓
                              init_eaccess("/init")     ← 分水岭
                              ├─ =0  → execve /init     → 用户态 1 号进程
                              └─ ≠0  → prepare_namespace()
                                          ├─ 等设备/认设备号（05）
                                          ├─ mount_root() 挂真根（05）
                                          │     └─ 失败 → panic（07）
                                          └─ 换根三步（06）→ 从真根找 init
```

## 实验：同一内核，两个 initramfs

| | 镜像 A | 镜像 B |
|---|---|---|
| 内容 | `bb_initramfs.cpio`（含 `/init`） | 同一 rootfs 去掉 `init` 重新打包 |
| 大小 | 2195968 字节 | 2195456 字节 |
| `init_eaccess(/init)` | `0` | `-2`（ENOENT） |
| `prepare_namespace()` | 未调用（0 条打点） | 调用（9 条打点） |
| 结局 | `Run /init` → busybox shell | `VFS: Cannot open root device ""` → panic |
| 时间 | 3.79s 探路 → 4.16s execve | 3.57s 探路 → 3.80s panic |

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
CMD="console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1"

# A：自带 /init
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd bb_initramfs.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-rootA.log 2>&1

# B：去掉 init 的对照镜像
rm -rf /tmp/noinit && cp -a bb_rootfs /tmp/noinit && rm -f /tmp/noinit/init
( cd /tmp/noinit && find . | cpio -o -H newc --quiet ) > /tmp/bb_noinit.cpio
timeout 40 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd /tmp/bb_noinit.cpio \
  -append "$CMD" -nographic -m 2G -no-reboot > /tmp/latte-rootB.log 2>&1
```

`panic=1` + `-no-reboot` 让 B 的 panic 自己退出 QEMU，不用干等超时。

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

最后两列都空的那条，是判断“有没有走到换根”的关键：A 没走这条路，B 在它之前 panic。

## 自查 3 问

1. 有 `/init` 时，`prepare_namespace()` 为什么一次都不被调用？（04）
2. `initrd_load()` 打了点却什么也没做，为什么？（07：`BLK_DEV_RAM` 没开，
   `rd_load_image()` 是空桩）
3. 怎么从日志区分“没有根”和“没有 init”？（07：`VFS: Unable to mount root fs`
   vs `No working init found`）

## 下一阶段

真根挂上之后接棒的是根文件系统里的 `/sbin/init`（systemd/sysvinit/脚本），那是
用户态的故事；内核侧还可以回到内存看这些页之后怎么被回收。
