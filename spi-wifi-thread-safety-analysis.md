# SPI WiFi Driver Thread Safety Analysis

- **Date:** 2026-03-25
- **Firmware:** v3.7.3
- **Scope:** `tifw/net/wifi/source/` - SimpleLink NWP SPI communication layer

## Overview

Multiple RTOS tasks (ENGINE, HTTPCLIENT, MQTT, OTA, REEFER) call SimpleLink APIs concurrently to communicate with the CC3220SF network processor over the internal SPI bus. This analysis identifies thread safety issues that cause SPI bus lockups and system instability.

## Architecture

The SimpleLink driver uses a layered locking approach:

- **GlobalLockObj** - API-level mutex serializing command/response transactions
- **ProtectionLockObj** - Protects driver control block structures
- **TxLockObj** - Protects TX flow control pool management
- **SelectLockObj** - Protects multi-select operations (multi-threaded mode)

SPI transactions are performed via `spi_Write()` and `spi_Read()` in `cc_pal.c`, using TI-RTOS `SPI_transfer()` in blocking mode with DMA limited to 4096 bytes per transaction. These functions have no explicit locking — they rely on the higher-level GlobalLockObj for serialization.

## Issues Found

### HIGH SEVERITY

#### 1. Global Lock Deletion on Fatal Error

**File:** `driver.c:2861` (`_SlDrvHandleFatalError`)

When a fatal SPI error occurs, the handler **deletes the GlobalLockObj mutex** while other threads may still be waiting on it:

```c
sl_LockObjDelete(&GlobalLockObj);   // line 2861
```

Threads blocked on this mutex receive `MUTEX_DELETED` and crash or enter undefined state. This is the most likely cause of cascading instability after an initial SPI error.

**Impact:** System-wide crash after any fatal SPI error.

#### 2. Flow Control Deadlock

**File:** `driver.c:934-977`

TX flow control uses `TxPoolCnt` with a `TxSyncObj` semaphore. Tasks wait on `TxSyncObj` when `TxPoolCnt <= FLOW_CONT_MIN`:

```c
if (g_pCB->FlowContCB.TxPoolCnt <= FLOW_CONT_MIN) {
    SL_DRV_SYNC_OBJ_WAIT_FOREVER(&g_pCB->FlowContCB.TxSyncObj);  // line 948
}
```

If all tasks are waiting for TX buffers and none can release them (e.g., after a partial SPI failure), the system deadlocks — complete WiFi freeze with no recovery path.

**Impact:** Complete WiFi freeze, unrecoverable without reboot.

### MEDIUM-HIGH SEVERITY

#### 3. Spawn IRQ Counter Race Condition

**File:** `spawn.c:122,80`

`IrqWriteCnt` is incremented in IRQ context while `IrqReadCnt` is incremented in the spawn task context. Neither uses atomic operations. Both are 8-bit counters.

```c
// IRQ context (spawn.c:122)
IrqWriteCnt++;

// Task context (spawn.c:80,84)
IrqReadCnt++;
```

**Impact:** Async events may be lost on concurrent access or counter overflow, causing missed responses from the NWP.

#### 4. WaitForCmdResp Flag Racing

**File:** `driver.c:853,1717,1727,1740`

The `WaitForCmdResp` flag is read and written from multiple contexts without explicit synchronization:

- Set to TRUE at line 853 (command path)
- Read in while loop at line 1717 (async path)
- Set to FALSE at lines 1727, 1740 (response path)

Only implicit ordering via the global lock provides partial protection.

**Impact:** Command responses may be misclassified; potential message corruption between command and async paths.

### MEDIUM SEVERITY

#### 5. ApiInProgressCnt Read Without Lock

**File:** `driver.c:2946`

`_SlDrvIsApiInProgress()` reads the counter without holding the protection lock:

```c
_i8 _SlDrvIsApiInProgress(void)
{
    if (g_pCB != NULL)
    {
        return (g_pCB->ApiInProgressCnt > 0);  // No lock held
    }
    return TRUE;
}
```

While `_SlDrvUpdateApiInProgress()` correctly acquires the lock for writes. The read may see stale or torn values.

**Impact:** Incorrect state detection; logic errors in API-in-progress checks.

#### 6. RxIrqCnt Not Atomic

**File:** `driver.c:180`

`RxIrqCnt` (volatile, modified in IRQ) is compared against `RxDoneCnt` (in control block, modified in task context) to detect pending messages:

```c
#define _SL_PENDING_RX_MSG(pDriverCB)   (RxIrqCnt != (pDriverCB)->RxDoneCnt)
```

The volatile qualifier prevents compiler optimization but does not guarantee atomic read-modify-write on ARM Cortex-M4.

**Impact:** Pending RX messages may be missed or double-processed in rare timing windows.

## Root Cause of SPI Instability

The most likely failure sequence:

1. An SPI timeout or NWP communication error triggers `_SlDrvHandleFatalError()` (Issue #1)
2. The handler deletes GlobalLockObj while HTTPCLIENT, MQTT, or ENGINE threads are waiting on it
3. Those threads crash or enter undefined state
4. The fatal error counter in `cc3220sf.c` increments toward `FATAL_ERROR_REBOOT_THRESHOLD` (10)
5. Meanwhile, flow control state may be corrupted (Issue #2), causing additional threads to deadlock
6. Eventually the threshold triggers `hal_reset()`, but the next boot may immediately hit the same race condition

## Recommended Fixes

### Fix 1: Don't Delete Lock on Fatal Error (High Priority)

In `_SlDrvHandleFatalError()`, instead of deleting the mutex, set a restart-required flag and release all waiting threads gracefully:

```c
// Instead of: sl_LockObjDelete(&GlobalLockObj);
// Set flag and unlock so waiting threads can check and exit
SL_SET_RESTART_REQUIRED;
SL_DRV_LOCK_GLOBAL_UNLOCK(TRUE);
```

Waiting threads should check `SL_IS_RESTART_REQUIRED` after acquiring the lock and bail out cleanly.

### Fix 2: Add Timeout to Flow Control Wait (High Priority)

Replace `WAIT_FOREVER` with a bounded timeout:

```c
// Instead of: SL_DRV_SYNC_OBJ_WAIT_FOREVER(&g_pCB->FlowContCB.TxSyncObj);
// Use a timeout (e.g., 5 seconds):
status = SL_DRV_SYNC_OBJ_WAIT_TIMEOUT(&g_pCB->FlowContCB.TxSyncObj, 5000);
if (status == SL_RET_CODE_TIMEOUT) {
    return SL_API_ABORTED;
}
```

### Fix 3: Application-Level API Mutex (Defensive, Lower Risk)

Add a single application-level mutex that all processes acquire before any `sl_*`, `HTTPClient_*`, or `MQTTClient_*` call. This provides an additional serialization layer above the SimpleLink driver, reducing the chance of hitting internal race conditions.

This can be implemented in `resfw/utils/utils_if.c` as a wrapper around SimpleLink API entry points.

### Fix 4: Atomic Counters for IRQ/Task Communication (Medium Priority)

Use ARM Cortex-M4 `__sync_fetch_and_add()` or equivalent for `IrqWriteCnt`, `IrqReadCnt`, and `RxIrqCnt` to ensure atomic access.

## Summary Table

| # | Issue | File | Severity | Impact |
|---|-------|------|----------|--------|
| 1 | Global lock deletion on fatal error | driver.c:2861 | High | Cascading crash after SPI error |
| 2 | Flow control deadlock | driver.c:934-977 | High | Complete WiFi freeze |
| 3 | Spawn IRQ counter race | spawn.c:122,80 | Medium-High | Lost async events |
| 4 | WaitForCmdResp flag racing | driver.c:853+ | Medium-High | Message corruption |
| 5 | ApiInProgressCnt unprotected read | driver.c:2946 | Medium | Incorrect state detection |
| 6 | RxIrqCnt not atomic | driver.c:180 | Medium | Missed/duplicate RX messages |

## Files Analyzed

| File | Purpose |
|------|---------|
| `tifw/net/wifi/source/driver.c` | Main driver with locking logic, command/response handling |
| `tifw/net/wifi/source/driver.h` | Driver structures, macros, lock definitions |
| `tifw/net/wifi/source/cc_pal.c` | SPI transactions, platform abstraction, mutex/semaphore primitives |
| `tifw/net/wifi/source/cc_pal.h` | Platform API definitions |
| `tifw/net/wifi/source/spawn.c` | Async spawn mechanism |
| `tifw/net/wifi/source/nonos.c` | Non-OS spawn fallback |
| `tifw/net/wifi/source/flowcont.c` | Flow control implementation |
| `tifw/net/wifi/source/flowcont.h` | Flow control API |
