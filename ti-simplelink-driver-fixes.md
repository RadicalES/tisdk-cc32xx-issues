# SimpleLink CC32xx Host Driver - Thread Safety Fixes

-**Submitted by:** Radical Electronic Systems
-**Date:** 2026-03-25
-**SDK Version:** SimpleLink CC32xx SDK 7.10.00.13
-**Platform:** CC3220SF, TI-RTOS 7 (SYS/BIOS), TI Clang ARM LLVM 2.1.3.LTS
-**Affected Files:** `source/ti/drivers/net/wifi/source/driver.c`, `source/ti/drivers/net/wifi/source/spawn.c`

## Summary

We have identified and patched four thread safety issues in the SimpleLink WiFi host driver (`ti/drivers/net/wifi/source/`) that cause SPI bus lockups, system crashes, and WiFi freezes in multi-threaded TI-RTOS environments. These issues manifest under load when multiple RTOS tasks (HTTP client, MQTT, OTA, application engine) concurrently access the network processor via SimpleLink APIs.

---

## Issue 1: Global Mutex Deletion on Fatal Error Causes Cascading Crash

**File:** `driver.c`, function `_SlDrvHandleFatalError()`
**Severity:** High

### Problem

When a fatal error is detected (e.g., command timeout, device abort), `_SlDrvHandleFatalError()` deletes the `GlobalLockObj` mutex:

```c
/* Original code */
SL_SET_RESTART_REQUIRED;

(void)sl_LockObjDelete(&GlobalLockObj);
SL_UNSET_GLOBAL_LOCK_INIT;
```

Any threads currently blocked on `sl_LockObjLock(&GlobalLockObj, SL_OS_WAIT_FOREVER)` — for example in `_SlDrvObjGlobalLockWaitForever()` — receive a `MUTEX_DELETED` error. This return value is not handled as a valid abort condition, causing those threads to proceed with invalid state or crash. In a multi-threaded environment with 3-5 tasks using SimpleLink APIs, this reliably causes cascading failures.

### Observed Symptoms

- Device reboots shortly after a single SPI communication error
- Multiple tasks fault simultaneously after a fatal error event
- Stack corruption observed in tasks that were waiting on the global lock

### Fix

Remove the mutex deletion. The `SL_SET_RESTART_REQUIRED` flag is already set before this point, and the sync object signaling loop that follows already wakes all waiting threads. Threads that check `SL_IS_RESTART_REQUIRED` after waking will return `SL_API_ABORTED` gracefully via the existing checks in `_SlDrvSyncObjWaitForever()` and `_SlDrvObjGlobalLockWaitForever()`.

```c
/* Fixed code */
SL_SET_RESTART_REQUIRED;

/* Instead of deleting the mutex (which crashes threads waiting on it),
   signal all sync objects so waiting threads wake up and check
   SL_IS_RESTART_REQUIRED, then return SL_API_ABORTED gracefully. */

/* signal all waiting sync objects */
for (i=0; i< MAX_CONCURRENT_ACTIONS; i++)
{
    SL_DRV_SYNC_OBJ_SIGNAL(&g_pCB->ObjPool[i].SyncObj);
}
```

The `SL_UNSET_GLOBAL_LOCK_INIT` and `sl_LockObjDelete(&GlobalLockObj)` lines are removed entirely. The global lock remains valid so that threads can acquire it, check the restart flag, and exit cleanly.

---

## Issue 2: Unbounded Wait in TX Flow Control Causes Deadlock

**File:** `driver.c`, functions `_SlDrvDataReadOp()` and `_SlDrvDataWriteOp()`
**Severity:** High

### Problem

When the TX buffer pool is exhausted (`TxPoolCnt <= FLOW_CONT_MIN`), both data read and data write operations wait indefinitely for a TX buffer to become available:

```c
/* Original code in _SlDrvDataReadOp() and _SlDrvDataWriteOp() */
SL_DRV_SYNC_OBJ_WAIT_FOREVER(&g_pCB->FlowContCB.TxSyncObj);
```

If the NWP (network processor) stops responding or the SPI bus is in an error state, no TX buffers will ever be released. All tasks that attempt network operations will block here permanently, resulting in a complete WiFi freeze with no recovery path and no fatal error notification.

### Observed Symptoms

- WiFi communication stops completely; device appears hung
- No fatal error callback is invoked
- All network-related tasks are blocked on the same semaphore
- Only a hardware reset recovers the device

### Fix

Replace the unbounded wait with a bounded timeout (`SL_DRIVER_TIMEOUT_SHORT`, 10 seconds). On timeout, trigger a fatal error so the existing error recovery path is invoked.

Applied in both `_SlDrvDataReadOp()` and `_SlDrvDataWriteOp()`:

```c
/* Fixed code */
/* Use bounded timeout to prevent deadlock if TX pool is never replenished */
if (sl_SyncObjWait(&g_pCB->FlowContCB.TxSyncObj, SL_DRIVER_TIMEOUT_SHORT))
{
    /* Timeout waiting for TX flow control - trigger fatal error */
    _SlDrvHandleFatalError(SL_DEVICE_EVENT_FATAL_DRIVER_ABORT, 0, 0);
    return SL_API_ABORTED;
}
if (SL_IS_RESTART_REQUIRED)
{
    return SL_API_ABORTED;
}
```

This ensures that a stalled NWP or SPI bus error is detected within 10 seconds and escalated to the fatal error handler, allowing the application to perform recovery (e.g., device restart).

---

## Issue 3: Unprotected Read of ApiInProgressCnt

**File:** `driver.c`, function `_SlDrvIsApiInProgress()`
**Severity:** Medium

### Problem

`_SlDrvIsApiInProgress()` reads `ApiInProgressCnt` without holding the protection lock:

```c
/* Original code */
_i8 _SlDrvIsApiInProgress(void)
{
    if (g_pCB != NULL)
    {
        return (g_pCB->ApiInProgressCnt > 0);
    }
    return TRUE;
}
```

However, `_SlDrvUpdateApiInProgress()` modifies the same counter under the protection lock:

```c
static void _SlDrvUpdateApiInProgress(_i8 Value)
{
    SL_DRV_PROTECTION_OBJ_LOCK_FOREVER();
    g_pCB->ApiInProgressCnt += Value;
    SL_DRV_PROTECTION_OBJ_UNLOCK();
}
```

The unprotected read can return stale or torn values on ARM Cortex-M4 when another thread is concurrently modifying the counter, leading to incorrect API-in-progress state detection.

### Fix

Acquire the protection lock before reading the counter:

```c
/* Fixed code */
_i8 _SlDrvIsApiInProgress(void)
{
    _i8 result;

    if (g_pCB != NULL)
    {
        _SlDrvProtectionObjLockWaitForever();
        result = (g_pCB->ApiInProgressCnt > 0);
        _SlDrvProtectionObjUnLock();
        return result;
    }

    return TRUE;
}
```

---

## Issue 4: Non-Volatile IRQ Counters in Spawn Task

**File:** `spawn.c`, struct `_SlInternalSpawnCB_t`
**Severity:** Medium

### Problem

The `IrqWriteCnt` and `IrqReadCnt` counters are used to synchronize between the IRQ handler and the spawn task:

```c
/* Original code */
typedef struct
{
    _SlSyncObj_t                SyncObj;
    _u8                         IrqWriteCnt;
    _u8                         IrqReadCnt;
    void*                       pIrqFuncValue;
    /* ... */
}_SlInternalSpawnCB_t;
```

`IrqWriteCnt` is incremented in `_SlInternalSpawn()` (called from IRQ context via `_SlDrvRxIrqHandler`), while `IrqReadCnt` is incremented and `IrqWriteCnt` is read in `_SlInternalSpawnWaitForEvent()` (spawn task context):

```c
/* IRQ context */
g_SlInternalSpawnCB.IrqWriteCnt++;

/* Task context */
while (g_SlInternalSpawnCB.IrqWriteCnt != g_SlInternalSpawnCB.IrqReadCnt)
{
    _SlDrvMsgReadSpawnCtx(g_SlInternalSpawnCB.pIrqFuncValue);
    g_SlInternalSpawnCB.IrqReadCnt++;
}
```

Without the `volatile` qualifier, the compiler may optimize the loop condition by caching `IrqWriteCnt` in a register, causing the spawn task to miss IRQ events that arrive during loop execution.

### Fix

Mark both counters as `volatile`:

```c
/* Fixed code */
typedef struct
{
    _SlSyncObj_t                SyncObj;
    volatile _u8                IrqWriteCnt;
    volatile _u8                IrqReadCnt;
    void*                       pIrqFuncValue;
    /* ... */
}_SlInternalSpawnCB_t;
```

---

## Additional Observation: WaitForCmdResp Flag

**File:** `driver.c`
**Severity:** Medium-High (not patched)

The `WaitForCmdResp` flag in the driver control block is read and written from multiple contexts (command path, async response path) without explicit memory barriers or synchronization beyond implicit ordering from the global lock. While the global lock provides some protection, there are code paths where this flag is checked outside the lock scope. We have not patched this issue as it would require more invasive changes to the driver architecture, but we believe it may contribute to rare command/response mismatch conditions under heavy concurrent API usage.

---

## Reproduction Environment

- **Hardware:** CC3220SF LaunchPad and custom production board
- **RTOS Tasks making concurrent SimpleLink API calls:**
  - HTTP client (periodic REST transactions, 5-10 second intervals)
  - MQTT client (persistent connection with publish/subscribe)
  - OTA download (large HTTP GET with streaming tar processing)
  - Application engine (network status queries, IP configuration)
- **Failure rate:** SPI lockup occurs approximately every 4-8 hours under normal operation; more frequently during OTA downloads concurrent with MQTT traffic

## Testing

These fixes have been deployed on production CC3220SF devices. We recommend TI review and incorporate them into the official SimpleLink CC32xx SDK.
