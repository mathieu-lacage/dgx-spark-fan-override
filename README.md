## NVIDIA DGX Spark 风扇控制

[en-US](README_EN.md)

通过 SoC 的 FF-A eSPI Service 调用 EC 已有的 command 5 以覆盖风扇设置：
```text
风扇全速：07 05 00 BC 34    # 0x34BC = 13500 RPM
恢复默认：07 05 00 FF FF    # 0xFFFF = 覆盖槽未启用
```

安装后的模块会持续绑定 `arm-ffa-17`，并创建：
```text
/sys/bus/arm_ffa/devices/arm-ffa-17/fan
```
每次向 `fan` 写入 `max` 或 `auto` 时发送一次请求，读取该属性会返回 `ready`、`max`、`auto` 或 `error <errno>`。

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
