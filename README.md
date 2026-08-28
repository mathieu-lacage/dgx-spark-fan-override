## NVIDIA DGX Spark 风扇控制

[en-US](README_EN.md)

通过 SoC 的 FF-A eSPI Service 调用 EC 已有的 command 5 以覆盖风扇设置：
```text
风扇全速：07 05 00 BC 34    # 0x34BC = 13500 RPM
恢复默认：07 05 00 FF FF    # 0xFFFF = 覆盖槽未启用
```

安装后的模块会持续绑定 `arm-ffa-17`，并创建：
```text
/sys/bus/arm_ffa/devices/arm-ffa-17/fan            (可读写)
/sys/bus/arm_ffa/devices/arm-ffa-17/fan_caps        (只读)
/sys/bus/arm_ffa/devices/arm-ffa-17/fan_limits      (只读)
/sys/bus/arm_ffa/devices/arm-ffa-17/fan_telemetry   (只读)
/sys/bus/arm_ffa/devices/arm-ffa-17/fan_rpm         (只读)
/sys/bus/arm_ffa/devices/arm-ffa-17/fan_override    (可读写)
```
每次向 `fan` 写入 `max` 或 `auto` 时发送一次请求，读取该属性会返回 `ready`、`max`、`auto` 或 `error <errno>`。`fan_caps`/`fan_telemetry` 以十六进制文本返回 EC 原始回复；`fan_limits` 解码能力回复（控制模式、fan0/fan1 最小/最大值）；`fan_rpm` 解码遥测快照中经验证得到的两路实时风扇转速字段。`fan_override` 读取时直接回读两个 EC 覆盖槽；写入 `"<low> <high>"`（十进制或 `0x` 前缀十六进制，均为 0-65535）可一次性设置两个槽，用于测试下文内核温控区所依赖的钳位机制。当 `nvfancontrol_ec` 温控区（见下文）处于 enabled 状态时，`fan` 与 `fan_override` 的写入会被拒绝并返回 `-EBUSY`，因为此时两个槽由内核控制回路占用。

--------

### 控制链路

```text
Linux 内核模块
  -> ARM FF-A Direct Request 2
  -> arm-ffa-17 / eSPI Service
  -> OEM1 command 17 + ns_shm0 共享页
  -> eSPI Memory32 0x06000800 请求区
  -> eSPI Memory32 0x06000504 门铃/状态
  -> EC 外层服务 0x07
  -> EC 温控邮箱 inner command 5
  -> EC SRAM 0x119192 = 13500
  -> 双风扇目标策略
  -> RPM 转 PWM，并钳位为 100%
  -> PWM0/TACH0 与 PWM1/TACH1
```

FF-A raw-eSPI 分区信息：

```text
device：/sys/bus/arm_ffa/devices/arm-ffa-17
UUID：884a63a0-3285-4120-83aa-eec008a0a546
partition ID：0x11
properties：0x109
共享页：0x933DD000，大小 0x1000
```

### [`EC 0x3000508`](https://fwupd.org/lvfs/devices/com.nvidia.dgx.spark.ec.firmware) 固件的风扇控制

#### 硬件映射与基本参数

| 项目 | 固件实现 |
|---|---|
| 风扇数量 | 2 |
| fan0 | PWM0 / TACH0 |
| fan1 | PWM1 / TACH1 |
| PWM 上限 | 100% |
| PWM 频率换算常量 | 28000，约 28 kHz |
| 主要温度输入 | 传感器 ID `0x4C`、`0x49`，策略使用较高值 |
| 默认控制模式 | `0`，RPM 模式 |
| fan0 RPM 范围 | 1260--9000 RPM |
| fan1 RPM 范围 | 1890--13500 RPM |
| 升速斜率 | 每次控制更新最多增加 10% |
| 降速斜率 | 每次控制更新减少 1% |
| 最终 PWM 钳位 | 最大 `0x64`，即 100% |

#### 两套静态温控曲线

Profile A：

| 档位 | 温度阈值 | PWM | 降档回滞 |
|---:|---:|---:|---:|
| 0 | 30 °C | 30% | 10 °C |
| 1 | 80 °C | 40% | 30 °C |
| 2 | 90 °C | 54% | 20 °C |
| 3 | 95 °C | 75% | 10 °C |
| 4 | 101 °C | 100% | 15 °C |

Profile B：

| 档位 | 温度阈值 | PWM | 降档回滞 |
|---:|---:|---:|---:|
| 0 | 20 °C | 30% | 10 °C |
| 1 | 40 °C | 45% | 30 °C |
| 2 | 70 °C | 70% | 20 °C |
| 3 | 85 °C | 100% | 10 °C |
| 4 | 101 °C | 100% | 15 °C |

#### 动态覆盖

EC 初始化时把以下两个 16 位槽设为 `0xFFFF`：

| 地址 | command | 策略作用 |
|---|---|---|
| `0x119190` | 2 读取、3 写入 | 低端覆盖槽；实际用于限制目标上限，即封顶 |
| `0x119192` | 4 读取、5 写入 | 高端覆盖槽；实际用于抬高目标下限，即托底 |

`0xFFFF` 表示该槽未启用。RPM 模式下，槽值分别按 fan0/fan1 的 RPM 范围转换为 PWM 百分比，然后和温度曲线目标组合；百分比模式则直接使用数值的低字节。

在默认 `0x119190 = 0xFFFF` 时，把 `0x119192` 写成 13500 的效果是：

```text
fan0: 13500 > 9000  -> RPM 转换函数返回 100%
fan1: 13500 = 13500 -> RPM 转换函数返回 100%
最终公共 PWM 路径再次执行 min(target, 100)
```

### 内核温控区集成（v1.1.0+）

向两个覆盖槽写入相同的值即可将风扇目标精确钳位在该值上（EC 的应用函数会把曲线输出钳制在低端/封顶槽与高端/托底槽之间），因此这两个槽合起来就是一个完整的 `set_fan_speed(rpm)` 原语，EC 自身的限速逻辑（每次更新最多 +10%/-1%）还能免费提供平滑过渡。模块利用这一点在 Linux 通用温控框架中注册了一个温控区（thermal zone）和一个制冷设备（cooling device）：

```text
/sys/class/thermal/thermal_zoneX/   (type: nvfancontrol_ec)
/sys/class/thermal/cooling_deviceY/ (type: nvfancontrol_fan)
```

加载模块时两者都会被注册，但温控区保持 **disabled** 状态——这与上文"加载不发出 EC 请求"的约定一致。在向该温控区的 `mode` 属性写入 `enabled` 之前，任何行为都不会改变：

```text
echo enabled > /sys/class/thermal/thermal_zoneX/mode   # 内核调速器开始接管
echo disabled > /sys/class/thermal/thermal_zoneX/mode  # 将两个槽释放回 EC（0xFFFF/0xFFFF）
```

写入 `enabled` 本身不会立即触发任何 EC 请求——它只是让 `step_wise` 调速器开始按轮询周期评估温度；真正的 EC 覆盖写入，要等到当前温度跨过某个触发点时才会发生。若启用时温度已经低于最低触发点，两个覆盖槽会保持写入前的原值（通常仍是 `0xFFFF/0xFFFF`），此时风扇转速完全由 EC 自身的原生曲线决定，`cooling_deviceY/cur_state` 会读到 `0`。

启用后，内核的 `step_wise` 调速器会根据 5 个触发点（trip）驱动制冷设备的 6 个状态（0-4 为钳位状态，各自对应一个触发点；5 为释放），这些触发点**默认近似 EC 自身的曲线**（30/75/89/95/96 °C，回滞 15/10/10/20/20 °C——最后一级从最初的 98 °C 调整为 96 °C，因为一次满载热压测试显示 EC 原生曲线实际在约 95-96 °C 就跳到了 100% 档位，而非 98 °C）——因此启用后在未经调优前变化很小，可通过该温控区的标准 sysfs 接口调优：

```text
cat  /sys/class/thermal/thermal_zoneX/trip_point_0_temp
echo 70000 > /sys/class/thermal/thermal_zoneX/trip_point_0_temp   # 单位：毫摄氏度
echo 5000  > /sys/class/thermal/thermal_zoneX/trip_point_0_hyst
```

状态 0-4 写入的值，是在 fan1 的控制域范围 `[fan1_min, fan1_max]`（首次启用时由 caps command 1 读取一次并缓存）上按 EC 曲线百分比（14/40/54/75/85%）线性插值得到的——**并非**字面意义上的 RPM 目标值。在验证所用的机器上，将该值钳位到接近该范围顶端时，实测 `fan_rpm` 明显高于所写入的值，也高于文档记载的 `fan1_max`；这是 EC 内部"控制域数值 → 实际转速"映射本身的特性，不是 bug，也不影响钳位机制本身的正确性。状态 5（释放）则完全将控制权交还给 EC 的原生曲线——配置的触发点区间顶端也落在这里，因此 EC 固件自身的温控保护始终是过热时的最终防线。

**温度来源。** `.get_temp` 直接对平台上每一个 ACPI `ThermalZone()` 对象读取 `_TMP`（首次调用时通过 `acpi_walk_namespace(ACPI_TYPE_THERMAL, ...)` 一次性找到全部对象并缓存句柄），取其中的最大值——这与 `drivers/acpi/thermal.c` 自身内部使用的是同一个标准的、已导出的 ACPICA 调用，而非 EC 自身的遥测数据。不产生 FF-A 事务，因此轮询是免费的；只有真正的风扇设定值变化才会触碰 EC 邮箱。这并非最初尝试的方案：一开始曾用看似更简单的 `thermal_zone_get_zone_by_name("acpitz")`，但该调用在设计上就会在多个已注册温控区共用同一名字时明确返回 `-EEXIST`（已在内核源码中确认，并在本机上实测复现——该平台全部 7 个 `acpitz` 温控区恰好共用同一名字）——它是刻意拒绝有歧义的匹配，而非静默挑一个返回，因此在这台机器上它从未真正解析出一个可用的温控区。直接读取 ACPI `_TMP`彻底绕开了这个问题：遍历的是 ACPI 对象本身而非 Linux 温控类设备列表，不存在命名歧义，而且天然聚合了全部 7 个真实传感器，无需依赖某一个恰好追踪到最大值。

**故障保护（failsafe）。** 三个模块参数，均可在运行时通过 `/sys/module/nvfancontrol/parameters/` 调整：

| 参数 | 默认值 | 作用 |
|---|---:|---|
| `failsafe_temp` | 100（°C） | 温度一旦达到该值，无论调速器处于何种状态，立即释放两个槽（0xFFFF/0xFFFF） |
| `failsafe_hyst` | 5（°C） | 只有当温度降到 `failsafe_temp - failsafe_hyst` 以下，内核控制才会重新接管 |
| `override_fail_limit` | 3 | 连续多少次 EC 覆盖写入失败后，温控区自我禁用（同时释放槽位） |

`failsafe_temp`（100 °C）相对镜像曲线顶端触发点（96 °C）留有充分余量，因此默认配置下完整曲线是可达的；一次满载压测（CPU `stress-ng` 叠加 GPU GEMM 满载）显示，在持续 20 分钟的近最坏情况满载下，整机会稳定在约 90-96 °C 的平衡点，全程未触发故障保护，因此 100 °C 相对正常运行状态仍留有实际余量。

无论温控区是否曾被启用，一个重启/关机通知器都会尽力释放两个槽位，因为正常重启后槽位状态会一直保持，直到 EC 断电重置才会复位。唯一无法覆盖的情形是硬崩溃（没有正常关机路径）——这由同样的"顶端释放回 EC"状态与故障保护机制兜底。

### EC 温控邮箱的全部 inner command

这里的 EC command 指外层服务 `0x07` 内、由 EC 函数 `0x000C3900` 处理的第二个字节。通用帧头为：

```text
[0] 序号/外层服务号，本机使用 07
[1] inner command
[2] 状态；请求置 00，成功回复 00，不支持回复 FF
[3...] 小端数据或输出
```

| inner command | 方向 | 作用 | 数据/回复 |
|---:|---|---|---|
| `0` | 无效 | 不支持 | status=`FF` |
| `1` | 读 | 查询能力、模式和两路控制范围 | 13 字节回复：能力、模式、fan0 min/max、fan1 min/max |
| `2` | 读 | 读取 `0x119190` 低端覆盖槽 | `+3..4` 返回 LE16 |
| `3` | 写 | 把 `+3..4` 的 LE16 写入 `0x119190` | status=`00`；输入值保留在回复中 |
| `4` | 读 | 读取 `0x119192` 高端覆盖槽 | `+3..4` 返回 LE16 |
| `5` | 写 | 把 `+3..4` 的 LE16 写入 `0x119192` | status=`00`；输入值保留在回复中 |
| `6` | 无效 | 固件 jump table 明确不支持 | status=`FF` |
| `7` | 读 | 生成温控/风扇运行遥测快照 | 从 `0x1188E2` 复制 64 字节到回复 `+3`；RPM/百分比模式下会刷新两路对应状态字段 |
| 其他 | 无效 | 不支持 | status=`FF` |

command 1 的实机回复：

```text
07 01 00 01 00 EC 04 28 23 62 07 BC 34
```

解码：

| 偏移 | 值 | 含义 |
|---:|---|---|
| 0 | `07` | 序号 |
| 1 | `01` | command 1 |
| 2 | `00` | 成功 |
| 3 | `01` | capability=1 |
| 4 | `00` | RPM mode |
| 5--6 | `EC 04` | fan0 min=1260 |
| 7--8 | `28 23` | fan0 max=9000 |
| 9--10 | `62 07` | fan1 min=1890 |
| 11--12 | `BC 34` | fan1 max=13500 |

若 mode 为 1，command 1 返回的两路范围均为 14--100，表示 PWM 百分比模式。

### SoC eSPI Service 的 OEM1 command 1--18

这些是 FF-A 安全分区的外层 OEM1 调试/服务命令，不是上表中的 EC inner command。只有 OEM1 command 17 封装并执行完整 EC EMI mailbox 流程。

| OEM1 command | 作用 | 本项目使用情况 |
|---:|---|---|
| `1` | 向 I/O 端口 `0x80` 输出 POST code | 未使用 |
| `2` | eSPI flash sector erase | 未使用；破坏性 |
| `3` | eSPI flash write | 未使用；破坏性 |
| `4` | eSPI flash read | 未使用 |
| `5` | eSPI RPMC operation 1 | 未使用；安全/计数器相关 |
| `6` | eSPI RPMC operation 2 | 未使用；安全/计数器相关 |
| `7` | 单字节 I/O 写 | 仅 v4 做过可恢复 marker；结果不能证明底层成功 |
| `8` | 单字节 I/O 读 | v2/v3 状态探测使用 |
| `9` | 单字节 Memory32 写 | v5 marker 使用；handler 隐藏底层失败 |
| `10` | 单字节 Memory32 读 | v5 使用；零值存在失败歧义 |
| `11` | 可变长度 Memory32 写 | v6 长度 1 marker 使用；隐藏底层失败 |
| `12` | 可变长度 Memory32 读 | v6 使用；零值存在失败歧义 |
| `13` | 可变长度 Memory64 写 | 未使用 |
| `14` | 可变长度 Memory64 读 | 未使用 |
| `15` | eSPI OOB write | 未使用 |
| `16` | 设置 eSPI general I/O | 未使用 |
| `17` | 通过 `ns_shm0` 执行完整通用 EC EMI 请求 | v7、v10、v11；当前方案唯一使用的控制入口 |
| `18` | 固定读取 Memory32 `0x06000798` 两字节并检查位 0/7/8/9/10/11 | v8/v9 只读诊断使用 |

OEM1 command 17 的共享页布局：

| 偏移 | 大小 | 含义 |
|---:|---:|---|
| `0x00` | 1 | 输入长度 |
| `0x01` | 1 | 输出长度 |
| `0x02` | 1 | EC 输出起始偏移 |
| `0x03` | 1 | accepted |
| `0x04` | 1 | ready |
| `0x10` | N | 输入数据，完成后由 EC 输出覆盖 |

服务状态：

| 状态 | 含义 |
|---:|---|
| `0` | 安全服务完成请求 |
| `5` | 底层状态读、请求写或门铃写失败；约 54 ms 时通常是 controller completion 超时 |
| `10` | EC mailbox 状态忙 |
