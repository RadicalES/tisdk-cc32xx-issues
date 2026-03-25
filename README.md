# SimpleLink CC32xx SDK - Reliability & Thread Safety Issues

**E2E Forum Thread:** [CC3220SF - SPI comms to WiFi CoCPU unreliable](https://e2e.ti.com/support/wireless-connectivity/wi-fi-group/wifi/f/wi-fi-forum/1628553/cc3220sf-spi-comms-to-wifi-cocpu-unreliable)

## SDK & Toolchain

| Component | Version |
|-----------|---------|
| **MCU** | TI CC3220SF (Cortex-M4 + integrated WiFi NWP), P/N: CC3220SF23ARGKT |
| **SDK** | SimpleLink CC32xx SDK 7.10.00.13 |
| **RTOS** | TI-RTOS 7 (SYS/BIOS) |
| **Compiler** | TI Clang ARM LLVM 2.1.3.LTS |
| **IDE** | Code Composer Studio (CCS) 12.3.0 |
| **NWP Service Pack** | Included with SDK 7.10.00.13 |

All issues documented here are against the **SimpleLink CC32xx SDK 7.10.00.13** release. The affected source files are located under `<SDK_INSTALL>/source/ti/drivers/net/wifi/source/` in the SDK distribution.

## Background

We are developing a production IoT device on the CC3220SF running 5+ concurrent RTOS tasks that use SimpleLink APIs (HTTP client, MQTT publish/subscribe, OTA firmware updates, application engine, reefer monitoring). Under sustained load, the SPI communication between the host MCU and the WiFi NWP becomes unreliable, leading to fatal errors, WiFi freezes, and device reboots.

This repository documents the issues we have found, the analysis performed, and the patches we have applied to the SimpleLink host driver to improve reliability. We are sharing this in the hope that TI can review and incorporate fixes into the official SDK, and to help other developers experiencing similar problems.

## Documents

### [Thread Safety Analysis](spi-wifi-thread-safety-analysis.md)

Detailed code-level analysis of the SimpleLink WiFi host driver (`source/ti/drivers/net/wifi/source/`) identifying 6 thread safety issues in the SPI communication layer. Covers locking mechanisms, race conditions, flow control deadlock potential, and global state access patterns.

**Key findings:**
- Global mutex deleted during fatal error handling while threads are waiting on it (High)
- Unbounded wait in TX flow control can deadlock if NWP stops responding (High)
- Spawn task IRQ counters not volatile, can miss async events (Medium-High)
- `ApiInProgressCnt` read without lock protection (Medium)

### [Proposed Driver Fixes](ti-simplelink-driver-fixes.md)

Four concrete patches to `driver.c` and `spawn.c` with full before/after code, rationale, and observed symptoms. Formatted for submission to TI SDK team.

**Patches:**
1. Remove `sl_LockObjDelete(&GlobalLockObj)` in `_SlDrvHandleFatalError()` - prevents cascading thread crashes
2. Replace `SL_DRV_SYNC_OBJ_WAIT_FOREVER` with bounded timeout in TX flow control - prevents permanent WiFi deadlock
3. Protect `ApiInProgressCnt` read in `_SlDrvIsApiInProgress()` with lock
4. Mark spawn IRQ counters as `volatile`

### [SPI Driver Error Analysis](cc3220sf-spi-driver-errors.md)

Analysis of the fatal error types (`SYNC_LOSS`, `DRIVER_ABORT`, `NO_CMD_ACK`, `CMD_TIMEOUT`, `DEVICE_ABORT`), their code paths in `driver.c`, and observed error patterns. Includes analysis of conflict between `sl_DeviceStatStart/Get/Stop` APIs and concurrent socket operations.

### [General Reliability Notes](cc3220sf-reliability-notes.md)

Broader reliability documentation covering NWP fatal abort errors, API context restrictions, random disconnections, race conditions, and statistics collection issues. Includes workarounds, recovery strategies, and comparison with alternative platforms.

## Our Environment

```
Concurrent RTOS Tasks using SimpleLink APIs:
  - HTTP Client    (periodic REST POST every 5-10s)
  - MQTT Client    (persistent connection, publish/subscribe)
  - OTA Download   (large HTTP GET, streaming tar processing)
  - Engine         (network status queries, IP configuration)
  - WiFi Stats     (sl_DeviceStatGet every 60s, pauses other clients)
```

## Observed Failure Modes

| Failure | Frequency | Symptom | Root Cause |
|---------|-----------|---------|------------|
| WiFi freeze | Every 4-8 hours | All network tasks blocked, no fatal error | TX flow control deadlock (Issue #2) |
| Cascading crash after SPI error | On every fatal error | Device reboots within seconds of first error | GlobalLockObj deletion (Issue #1) |
| Missed async events | Intermittent | Stale connection state, delayed responses | Non-volatile IRQ counters (Issue #4) |
| NWP conflict during stats | During stats collection | `DRIVER_ABORT` with `0x4000` flag | Stats API blocks NWP |

## How to Apply the Patches

The patches target two files in the SimpleLink SDK:

```
<SDK_INSTALL>/source/ti/drivers/net/wifi/source/driver.c
<SDK_INSTALL>/source/ti/drivers/net/wifi/source/spawn.c
```

See [ti-simplelink-driver-fixes.md](ti-simplelink-driver-fixes.md) for the exact changes.

## Contributing

If you are experiencing similar issues with the CC32xx SimpleLink SDK, feel free to open an issue or submit additional findings. Reference the [E2E forum thread](https://e2e.ti.com/support/wireless-connectivity/wi-fi-group/wifi/f/wi-fi-forum/1628553/cc3220sf-spi-comms-to-wifi-cocpu-unreliable) for discussion with TI engineers.

## License

This documentation is provided as-is for the benefit of the TI developer community. The code snippets reference TI's SimpleLink SDK which is subject to TI's own license terms.
