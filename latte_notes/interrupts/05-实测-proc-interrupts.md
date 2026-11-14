# 主题五 · 实测：`/proc/interrupts` 里的数字就是“中断真的发生了”

## 你已知的

`/proc/interrupts` 是内核统计中断次数的地方。每个数字都代表“这个向量确实进过一次
中断处理”。让 init 读一遍，等于给前面几篇的结论做体检。

## `/proc/interrupts`（实测，镜像 D）

```
           CPU0
  0:        927  IO-APIC   2-edge      timer
  1:          9  IO-APIC   1-edge      i8042
  4:          6  IO-APIC   4-edge      ttyS0
  8:          1  IO-APIC   8-edge      rtc0
  9:          0  IO-APIC   9-fasteoi   acpi
 12:        125  IO-APIC  12-edge      i8042
 14:          0  IO-APIC  14-edge      ata_piix
 15:         25  IO-APIC  15-edge      ata_piix
NMI:          0
LOC:       3116   Local timer interrupts
SPU:          0   Spurious interrupts
PMI:          0   Performance monitoring interrupts
```

怎么读：

| 列 | 含义 |
|---|---|
| `CPU0` 表头 | 单核；多核时每列一个 CPU，一个 IRQ 在各 CPU 上的计数分开算 |
| `0:` `4:` | IRQ 号，对应 `request_irq()` 的第一个参数 |
| `IO-APIC 2-edge` | 路由到 IO-APIC、触发方式（edge 边沿 / fasteoi 电平） |
| `timer` `ttyS0` | `request_irq()` 时给的 `name`（主题四的打点里出现过） |
| `LOC:` | 本地 APIC 定时器向量，不计入 IRQ 号，但正是 tick 的来源 |
| `SPU:` | 伪中断（spurious），正常应接近 0 |

## `/proc/softirqs`（下半部）

```
       HI:    0    TIMER:  138    NET_TX: 0    NET_RX: 0
   BLOCK: 0   IRQ_POLL: 0     TASKLET: 1      SCHED: 0
 HRTIMER: 0      RCU:  552
```

`TIMER`（138）是定时器下半部，`RCU`（552）是 RCU 回调——说明中断返回后
`irq_exit()` 里的 softirq 也在跑。**上半部只做“清标志+记账”，真正的活在这里。**

## `/proc/stat` 的 `intr` 行

```
intr 4395 927 9 0 0 158 0 0 0 1 0 0 0 125 …
```

- 第一个数是**所有向量累计**（含 `LOC` 的 3116）；
- 后面按向量号排开：`4395` 里外部 IRQ 之和约 1093，其余是本地 APIC 向量。

同一份输出里还有 `ctxt 1489`（上下文切换）、`processes 59`（创建过的进程数）、
`softirq 703`（下半部总数）——都用得上，但不要把它们和中断数混着看。

## 三个可以直接对照的结论

| 观察 | 说明 |
|---|---|
| `timer` 927 + `LOC` 3116 都在涨 | tick 由本地 APIC 定时器驱动（`setup_APIC_timer` 打点已验证） |
| `ttyS0` 有 6 次 | 3.45s 才 `request_irq` 的串口，注册后确实收到中断（TX 完成/modem 状态） |
| `ata_piix` 14 是 0、15 是 25 | 共享中断：盘上只有 hda 一侧有活动，另一侧 0 次很正常 |

## 时间线（与打点对齐）

| 时刻 | 打点 | 之后 `/proc` 里开始有它 |
|---|---|---|
| 0.000s | `request_irq: irq=0 name=timer` | `timer` 行 |
| 0.890s | `setup_APIC_timer: cpu0 uses lapic` | `LOC` 行 |
| 2.500s | `request_irq: irq=14/15 name=ata_piix` | `ata_piix` 行 |
| 3.239s | `request_irq: irq=1/12 name=i8042` | `i8042` 行 |
| 3.450s | `request_irq: irq=4 name=ttyS0` | `ttyS0` 行 |

**注册在案才有计数**：这正是“中断三要素”的第三条——设备驱动 `request_irq()`。

## 复现

```bash
cd /home/dong/Documents/github/linux
make -j16 bzImage
rm -rf /tmp/irqdiag && cp -a bb_rootfs /tmp/irqdiag
cat > /tmp/irqdiag/init <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "=== interrupts ==="; cat /proc/interrupts
echo "=== softirqs ==="; cat /proc/softirqs
echo "=== stat ==="; cat /proc/stat
echo "=== irqdiag done ==="
exec /bin/sh
EOF
chmod +x /tmp/irqdiag/init
( cd /tmp/irqdiag && find . | cpio -o -H newc --quiet ) > /tmp/bb_irqdiag.cpio
timeout 30 qemu-system-x86_64 -kernel arch/x86/boot/bzImage -initrd /tmp/bb_irqdiag.cpio \
  -append "console=ttyS0 earlyprintk=serial nokaslr ignore_loglevel panic=1" \
  -nographic -m 2G -no-reboot > /tmp/latte-irqD.log 2>&1
```
