# 主题二 · prepare_namespace()：挂真根的六个固定动作

## 它为什么存在

`kernel_init_freeable()` 里只有一条路能走到它：`init_eaccess("/init")` 失败，也就是
initramfs 里**没有**可执行的 init（主题一）。所以这个函数的任务不是“挂根”一个动作，
而是“把挂根之前该等的、该组的、该兼容的，全部做完”。

## 六个动作

| 顺序 | 动作 | 解决什么问题 |
|---|---|---|
| 1 | `root_delay` 秒的 `ssleep()` | 慢速存储要几秒才响应，`rootdelay=5` 就是给它留时间 |
| 2 | `wait_for_device_probe()` | 等已注册设备的探测跑完，否则根设备的驱动还在 probe |
| 3 | `md_run_setup()` | 根在软 RAID 上时先把阵列组起来，否则找不到 `/dev/mdX` |
| 4 | `parse_root_device(saved_root_name)` | 把 `root=/dev/sda2`、`root=/dev/nfs` 这类字符串变成 `ROOT_DEV` |
| 5 | `initrd_load(saved_root_name)` | 老式 initrd 兼容路径（主题四）；返回 true 就直接跳过真根 |
| 6 | `wait_for_root()` + `mount_root()` | `rootwait` 时轮询等设备出现，再按 `ROOT_DEV` 挂根（主题三） |

顺序上的讲究：1、2、3 都是“等”（等设备、等探测、等阵列），4 是纯解析，5、6 才是
真正的挂载。把“等”放在前面，是因为挂载失败一次就要 panic，没有第二次机会。

## 三行收尾不是顺便写的

```c
out:
	devtmpfs_mount();
	init_mount(".", "/", NULL, MS_MOVE, NULL);
	init_chroot(".");
```

这三行才是“把新根变成 `/`”的动作，见主题五。它们同时是老式 initrd 路径的出口
（`initrd_load()` 返回 true 时 `goto out`，因为 `/linuxrc` 已经替内核挂好了根）。

## 两条正常出口 + 一条异常出口

- `initrd_load()` 返回 true：`goto out`；
- `mount_root()` 挂成功：顺序落到 `out`；
- `mount_root()` 挂不上：不返回，直接在 `mount_root_generic()` 里 panic（主题六）。

## 代码位置与打点

| 位置 | 打点 |
|---|---|
| `init/do_mounts.c:491` `prepare_namespace()` 入口 | `[latte] prepare_namespace: root=%s` |
| `:520` `initrd_load()` 之前 | `[latte] prepare_namespace: initrd_load` |
| `:528` `mount_root()` 之前 | `[latte] prepare_namespace: mount_root` |
| `:532` `devtmpfs_mount()` 之前 | `[latte] prepare_namespace: devtmpfs + switch root` |

四个打点的先后顺序就是上表的执行顺序，缺一个就说明中间某步提前返回了。
