# 主题六 · zram：把交换区放进内存并压缩

`swap/01`~`05` 用的是**磁盘**上的交换区，`qa/01` 还解释了 `pswpout` 为什么偏小。
这篇换成 **zram**：交换区本身就在内存里，写进去的页先压缩再存——于是“换出”能真的腾出内存，
但**只对可压缩的数据有效**。

> 本篇需要改内核配置：本树原先 `# CONFIG_ZRAM is not set`。
> 启用方式是 `./scripts/config -e ZRAM && make olddefconfig`（`ZSMALLOC` 会被自动选中），
> 默认压缩算法为 `lzo-rle`。

## 你已知的

- 没有 swap 时匿名页/tmpfs 页不可回收（`swap/01`）；
- 换出/换入走 `swap_writeout → __swap_writepage` 和 `do_swap_page → swap_read_folio`（`swap/03`、`swap/05`）；
- 全零页被 zeromap 短路，不产生 IO（`qa/01`）。

## 内核里对应什么：一个压缩的块设备

zram 把自己伪装成 `/dev/zram0`（一个 block device），但背后没有磁盘：

```
swapon /dev/zram0
 └─ 交换槽位 → 读写这个"块设备" → zram 驱动

swap_writepage() → submit_bio()
 └─ zram_bvec_write()            drivers/block/zram/zram_drv.c:1927
     └─ zcomp_compress()         drivers/block/zram/zcomp.c:137
         用 lzo-rle 把 4KB 页压成压缩流
         └─ 存进 zsmalloc 管理的压缩页池（仍然占 RAM，但小得多）
```

关键 sysfs：

| 文件 | 作用 |
|---|---|
| `/sys/block/zram0/disksize` | 设“盘”多大（**逻辑大小**，不是内存占用） |
| `/sys/block/zram0/comp_algorithm` | 压缩算法，默认 `lzo-rle` |
| `/sys/block/zram0/mm_stat` | 统计：`orig_data_size compr_data_size mem_used_total …` |
| `/sys/class/zram-control/hot_add` | 再要一块新 zram（本内核开机已自带 `zram0`） |

`mm_stat` 前三个字段是回答“省没省”的全部证据：

- `orig_data_size`：换进来的**原始**字节数（≈ 换出量）；
- `compr_data_size`：压缩后**内容**字节数；
- `mem_used_total`：这堆数据**实际占的内存**（含 zsmalloc 元数据与碎片）。

## 实测 A：90MB 可压缩数据 + 10MB 随机数据

160M 机器（`MemTotal 113680 kB`），zram 64MB，`swapon` 后填 tmpfs：

```
=== before ===
MemTotal: 113680 kB   MemFree: 95924 kB   SwapTotal/Free: 65532 kB

=== A: 写 90MB 可压缩数据（全是 a）===
MemFree: 8796 kB   SwapFree: 65532 kB   pswpout 0
mm_stat:     4096       73    12288 ...

=== B: 再写 10MB 随机数据（不可压缩）===
MemFree: 3008 kB   SwapFree: 60688 kB   pswpout 1211
mm_stat:  4964352    68943   114688 ...
```

两段各有信息：

**A 段没有发生换出**：90MB 的 `'a'` 页全塞进了内存（MemFree 从 96MB 掉到 8.8MB），
`SwapFree` 一点没动、`pswpout 0`。这说明 zram 不是一个“自动省内存”的开关——
它只在**换出真正发生时**才开始工作。

**B 段触发了换出**：再写 10MB 随机数据把内存压到底，内核开始换出（`pswpout 1211`），
`SwapFree` 用掉 4844 kB。而 zram 的统计非常反直觉地漂亮：

| 指标 | 数值 | 说明 |
|---|---|---|
| `orig_data_size` | **4964352 B**（4.96MB） | 换进来的原始数据 ≈ 1211 页 × 4KB ✓ |
| `compr_data_size` | **68943 B**（67KB） | 压缩后内容 |
| 压缩比 | **≈ 72 : 1** | 因为这一轮换出去的大多是那段 `'a'` 页 |
| `mem_used_total` | **114688 B**（112KB） | 实际占用内存（含元数据） |

也就是说：**4.84MB 的换出，实际只花了 112KB 内存**。
`SwapFree` 减 4844 kB、`pswpout` 1211 页、`orig_data_size` 4.96MB —— 三个口径依然对得上（`swap/05` 的方法）。

## 实测 B：不可压缩数据的代价

第一次实验直接用 `dd if=/dev/urandom` 写 100MB，结果**整台机器卡住**（60 秒超时）：

- 100MB 随机页要进 tmpfs；
- 物理内存只有 113MB，多出来的部分只能换出到 64MB 的 zram；
- 但随机数据压缩比 ≈1，**换出去的页几乎仍占同样多的内存**——
  等于“用内存换内存”，于是陷入抖动。

这是 zram 最重要的一条边界：**它省的是“可压缩”的那部分**。
文本、零页、稀疏数据能省几个数量级；压缩过的包、随机数据、图片基本不省。

## 三个结论

1. **zram 只在“换出发生”时才省内存**：A 段 90MB 可压缩数据一点没换出，`mm_stat` 几乎为空；
   触发换出后（B 段）才看到 72:1 的压缩比。它不是 `malloc` 的优化，是 swap 的优化。
2. **“换出量”和“内存占用”是两个数**：`SwapFree` 掉 4.84MB，
   实际内存只少了 112KB（`mem_used_total`）。看 zram 有没有奇效，必须看 `mm_stat`，
   不能只看 `SwapFree`。
3. **不可压缩数据会让 zram 变成“抖动机”**：第一次实验 100MB `urandom` 直接把系统拖死，
   因为换出去的页仍然按原大小占内存，swap 压力没有缓解。
   生产上 zram 一般配 `vm.swappiness` 与 `zram` 的容量一起调，就是这个原因。

## 复现

**先改配置**（本树默认没开）：

```bash
cd /home/dong/Documents/github/linux
./scripts/config -e ZRAM -e ZSMALLOC
make olddefconfig          # ZSMALLOC/ZRAM_BACKEND_LZO 会被自动选中
grep -E "^CONFIG_(ZRAM|ZSMALLOC)=" .config
# CONFIG_ZSMALLOC=y
# CONFIG_ZRAM=y
make -j16 bzImage          # 配置变了会大面积重编，约 3 分钟
```

然后：

```bash
rm -rf /tmp/zram && cp -a bb_rootfs /tmp/zram
cat > /tmp/zram/init <<'EOF'
#!/bin/sh
B=/bin/busybox
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
$B ls /dev/zram*                      # 开机已自带 zram0
$B cat /sys/block/zram0/comp_algorithm
echo 64M > /sys/block/zram0/disksize
$B mkswap /dev/zram0 >/dev/null && $B swapon /dev/zram0 && echo "swapon ok"
$B grep -E "^(MemTotal|MemFree|SwapTotal|SwapFree)" /proc/meminfo
$B mkdir -p /tf
$B mount -t tmpfs -o size=140M none /tf
# A: 90MB 可压缩数据
$B yes aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa | $B head -c 94371840 > /tf/a.bin
$B grep -E "^(MemFree|SwapFree)" /proc/meminfo
$B grep -E "^(pswpout|pswpin)" /proc/vmstat
echo -n "mm_stat: "; $B cat /sys/block/zram0/mm_stat
# B: 10MB 随机数据，把内存压到底
$B dd if=/dev/urandom of=/tf/b.bin bs=1M count=10 2>/dev/null
$B grep -E "^(MemFree|SwapFree)" /proc/meminfo
$B grep -E "^(pswpout|pswpin)" /proc/vmstat
echo -n "mm_stat: "; $B cat /sys/block/zram0/mm_stat
echo "=== done ==="
exec /bin/sh
EOF
chmod +x /tmp/zram/init
( cd /tmp/zram && find . | cpio -o -H newc --quiet ) > /tmp/bb_zram.cpio
timeout 60 qemu-system-x86_64 -kernel arch/x86/boot/bzImage \
  -initrd /tmp/bb_zram.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr loglevel=3 log_buf_len=2M panic=1" \
  -nographic -m 160M -no-reboot > /tmp/latte-zram.log 2>&1 || true
tr -d '\r' < /tmp/latte-zram.log | sed -n '/开机就有的/,/done/p'
```

两个必须注意的点：

- **数据要可压缩**：用 `yes aaaa… | head -c N` 造“文本”，不要一上来就 `dd if=/dev/urandom`
  ——第一次实验就是这么把机器拖死的。
- **`hot_add` 不是必须的**：`CONFIG_ZRAM=y` 时开机就有 `/dev/zram0`；
  `hot_add` 是**再加一块**（实测返回 `zram1`）。要几块都行，`disksize` 各自设。

## 代码位置

| 位置 | 符号 | 做什么 |
|---|---|---|
| `drivers/block/zram/zram_drv.c:2485` | `disksize_store()` | 设逻辑磁盘大小（`/sys/block/zram0/disksize`） |
| `drivers/block/zram/zram_drv.c:1927` | `zram_bvec_write()` | 一次写请求：压缩一个页并存进压缩池 |
| `drivers/block/zram/zcomp.c:137` | `zcomp_compress()` | 调 `lzo-rle` 真正压缩 |
| `drivers/block/zram/zram_drv.c:1495` | `mm_stat_show()` | `/sys/block/zram0/mm_stat` 的六个字段 |
| `drivers/block/zram/zram_drv.c:1373` | `comp_algorithm_show()` | 可用/当前压缩算法 |
| `mm/zsmalloc.c` | zsmalloc | 压缩页的分配器（碎片会影响 `mem_used_total`） |

## 一句话收尾

zram 把“换出”从“写到磁盘”变成“压缩后放回内存”：
可压缩的数据能省两个数量级（本次 4.84MB → 112KB），
不可压缩的数据则一点不省，甚至因为挤压而抖动。
判断有没有效，别只看 `SwapFree`，要看 `mm_stat` 的 `orig / compr / mem_used` 三个数。
