## NVIDIA DGX Spark Fan Control

[zh-CN](README.md)

Override fan settings by invoking the EC's existing command 5 via the SoC's FF-A eSPI service:
```text
Full fan speed: 07 05 00 BC 34    # 0x34BC = 13500 RPM
Restore default: 07 05 00 FF FF    # 0xFFFF = Override slot disabled
```

Once installed, the module binds to `arm-ffa-17` and creates the following:
```text
/sys/bus/arm_ffa/devices/arm-ffa-17/fan
```
A request is sent each time `max` or `auto` is written to `fan`; reading this attribute returns `ready`, `max`, `auto`, or `error <errno>`. --------

### Control Link

```text
Linux Kernel Module
-> ARM FF-A Direct Request 2
-> arm-ffa-17 / eSPI Service
-> OEM1 command 17 + ns_shm0 shared page
-> eSPI Memory32 0x06000800 request region
-> eSPI Memory32 0x06000504 doorbell/status
-> EC outer service 0x07
-> EC thermal control mailbox inner command 5
-> EC SRAM 0x119192 = 13500
-> Dual-fan target policy
-> RPM to PWM conversion, clamped to 100%
-> PWM0/TACH0 and PWM1/TACH1
```

FF-A raw-eSPI partition information:

```text
device: /sys/bus/arm_ffa/devices/arm-ffa-17
UUID: 884a63a0-3285-4120-83aa-eec008a0a546
partition ID: 0x11
properties: 0x109
shared page: 0x933DD000, size 0x1000
```

### Fan Control for [`EC 0x3000508`](https://fwupd.org/lvfs/devices/com.nvidia.dgx.spark.ec.firmware) Firmware

#### Hardware Mapping and Basic Parameters

| Item | Firmware Implementation |
|---|---|
| Number of fans | 2 |
| fan0 | PWM0 / TACH0 |
| fan1 | ​​PWM1 / TACH1 |
| PWM upper limit | 100% |
| PWM frequency conversion constant | 28000, approx. 28 kHz |
| Primary temperature input | Sensor IDs `0x4C`, `0x49`; policy uses the higher value |
| Default control mode | `0`, RPM mode |
| fan0 RPM Range | 1260–9000 RPM |
| fan1 RPM Range | 1890–13500 RPM |
| Ramp-up Rate | Max increase of 10% per control update |
| Ramp-down Rate | Decrease of 1% per control update |
| Final PWM Clamp | Max `0x64` (i.e., 100%) |

#### Two Static Thermal Control Profiles

Profile A:

| Level | Temperature Threshold | PWM | Step-down Hysteresis |
|---:|---:|---:|---:|
| 0 | 30 °C | 30% | 10 °C |
| 1 | 80 °C | 40% | 30 °C |
| 2 | 90 °C | 54% | 20 °C |
| 3 | 95 °C | 75% | 10 °C |
| 4 | 101 °C | 100% | 15 °C |

Profile B:

| Level | Temperature Threshold | PWM | Step-down Hysteresis |
|---:|---:|---:|---:|
| 0 | 20 °C | 30% | 10 °C |
| 1 | 40 °C | 45% | 30 °C |
| 2 | 70 °C | 70% | 20 °C |
| 3 | 85 °C | 100% | 10 °C |
| 4 | 101 °C | 100% | 15 °C |

#### Dynamic Override

During EC initialization, the following two 16-bit slots are set to `0xFFFF`:

| Address | Command | Strategy Function |
|---|---|---|
| `0x119190` | 2 (Read), 3 (Write) | Low-end override slot; used to limit the target upper bound (i.e., capping) | |
| `0x119192` | 4 Read, 5 Write | High-end override slot; effectively used to raise the target floor (i.e., set a minimum baseline).

`0xFFFF` indicates that the slot is disabled. In RPM mode, the slot values ​​are converted into PWM percentages based on the respective RPM ranges of fan0 and fan1, then combined with the temperature curve target; in percentage mode, the low byte of the value is used directly.

With the default setting `0x119190 = 0xFFFF`, writing `13500` to `0x119192` yields the following result:

```text
fan0: 13500 > 9000  -> RPM conversion function returns 100%
fan1: 13500 = 13500 -> RPM conversion function returns 100%
The final common PWM path then executes min(target, 100)
```

### Complete list of inner commands for the EC thermal control mailbox

Here, "EC command" refers to the second byte—processed by EC function `0x000C3900`—within the outer service `0x07`. The generic frame header is:

```text
[0] Sequence number/Outer service ID; local unit uses 07
[1] Inner command
[2] Status; set to 00 for requests, 00 for successful replies, FF for unsupported
[3...] Little-endian data or output
```

| Inner command | Direction | Function | Data/Reply |
|---:|---|---|---|
| `0` | N/A | Unsupported | status=`FF` |
| `1` | Read | Query capabilities, mode, and control ranges for both channels | 13-byte reply: capabilities, mode, fan0 min/max, fan1 min/max |
| `2` | Read | Read low-end override slot at `0x119190` | Returns LE16 at `+3..4` |
| `3` | Write | Write LE16 from `+3..4` to `0x119190` | status=`00`; input value preserved in reply |
| `4` | Read | Read high-end override slot at `0x119192` | Returns LE16 at `+3..4` |
| `5` | Write | Write LE16 from `+3..4` to `0x119192` | status=`00`; input value preserved in reply |
| `6` | N/A | Explicitly unsupported by firmware jump table | status=`FF` |
| `7` | Read | Generate thermal control/fan operation telemetry snapshot | Copy 64 bytes from `0x1188E2` to reply `+3`; refreshes status fields for both channels (RPM/percentage mode) |
| Others | N/A | Unsupported | status=`FF` |

Actual reply from device for command 1:

```text
07 01 00 01 00 EC 04 28 23 62 07 BC 34
```

Decoding:

| Offset | Value | Meaning |
|---:|---|---|
| 0 | `07` | Sequence number |
| 1 | `01` | command 1 |
| 2 | `00` | Success |
| 3 | `01` | capability=1 |
| 4 | `00` | RPM mode |
| 5--6 | `EC 04` | fan0 min=1260 |
| 7--8 | `28 23` | fan0 max=9000 |
| 9--10 | `62 07` | fan1 min=1890 |
| 11--12 | `BC 34` | fan1 max=13500 |

If the mode is 1, the ranges for both channels returned by command 1 are 14–100, indicating PWM percentage mode.

### SoC eSPI Service OEM1 commands 1–18

These are the outer-layer OEM1 debug/service commands for the FF-A secure partition, distinct from the EC inner commands listed in the table above. Only OEM1 command 17 encapsulates and executes the full EC EMI mailbox process. | OEM1 command | Function | Usage in this project |
|---:|---|---|
| `1` | Output POST code to I/O port `0x80` | Unused |
| `2` | eSPI flash sector erase | Unused; destructive |
| `3` | eSPI flash write | Unused; destructive |
| `4` | eSPI flash read | Unused |
| `5` | eSPI RPMC operation 1 | Unused; security/counter-related |
| `6` | eSPI RPMC operation 2 | Unused; security/counter-related |
| `7` | Single-byte I/O write | Used only in v4 for recoverable marker; result does not confirm underlying success |
| `8` | Single-byte I/O read | Used in v2/v3 for status probing |
| `9` | Single-byte Memory32 write | Used in v5 marker; handler masks underlying failure |
| `10` | Single-byte Memory32 read | Used in v5; zero value creates ambiguity regarding failure |
| `11` | Variable-length Memory32 write | Used in v6 (length 1) marker; masks underlying failure |
| `12` | Variable-length Memory32 read | Used in v6; zero value creates ambiguity regarding failure |
| `13` | Variable-length Memory64 write | Unused |
| `14` | Variable-length Memory64 read | Unused |
| `15` | eSPI OOB write | Unused |
| `16` | Configure eSPI general I/O | Unused |
| `17` | Execute full generic EC EMI request via `ns_shm0` | v7, v10, v11; the only control entry point used in the current scheme |
| `18` | Fixed read of two bytes from Memory32 `0x06000798` and check bits 0/7/8/9/10/11 | Used in v8/v9 for read-only diagnostics |

OEM1 command 17 Shared page layout:

| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | 1 | Input length |
| `0x01` | 1 | Output length |
| `0x02` | 1 | EC output start offset |
| `0x03` | 1 | Accepted |
| `0x04` | 1 | Ready |
| `0x10` | N | Input data; overwritten by EC output upon completion |

Service status:

| Status | Meaning |
|---:|---|
| `0` | Security service request completed |
| `5` | Underlying status read, request write, or doorbell write failed; a duration of ~54 ms usually indicates a controller completion timeout |
| `10` | EC mailbox status busy |
