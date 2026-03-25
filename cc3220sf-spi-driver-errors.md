# CC3220SF SPI Driver Error Analysis

## Overview

The CC3220SF uses SPI communication between the host MCU (Cortex-M4) and the Network Processor (NWP). The SimpleLink driver manages this communication, and errors can occur when the SPI link loses synchronization or the NWP becomes unresponsive.

## Fatal Error Types

### 1. SL_DEVICE_EVENT_FATAL_SYNC_LOSS

**Source:** `driver.c:1265, 2142` in `_SlDrvRxHdrRead()`

**Cause:** SPI communication loses synchronization with NWP.

**Triggers:**
- SPI read times out waiting for sync pattern
- NWP doesn't respond with expected sync bytes
- Communication interrupted during header read

**Code path:**
```c
// driver.c:2076-2078
if (_SlDrvIsTimeoutExpired(&TimeoutInfo))
{
    return SL_API_ABORTED;  // → triggers SYNC_LOSS
}

// driver.c:1261-1266
if (_SlDrvRxHdrRead((_u8*)(uBuf.TempBuf)) == SL_API_ABORTED)
{
    SL_DRV_LOCK_GLOBAL_UNLOCK(TRUE);
    _SlDrvHandleFatalError(SL_DEVICE_EVENT_FATAL_SYNC_LOSS, 0, 0);
    return SL_API_ABORTED;
}
```

**Typical scenarios:**
- NWP is busy processing another command
- SPI bus contention
- NWP internal error causing delayed response
- Power management state transitions

### 2. SL_DEVICE_EVENT_FATAL_DRIVER_ABORT

**Source:** `trace.h:55` via `_SlDrvAssert()`

**Cause:** Internal driver assertion failure.

**Triggers:**
- Driver receives unexpected response class
- Internal state machine receives invalid data
- Protocol violation detected

**Code path:**
```c
// trace.h:55
#define _SlDrvAssert() _SlDrvHandleFatalError(SL_DEVICE_EVENT_FATAL_DRIVER_ABORT, 0, 0)

// driver.c:1930-1931
default:
    _SL_ASSERT_ERROR(0, SL_API_ABORTED);  // Unexpected message class
```

**Typical scenarios:**
- Corrupted SPI data
- Out-of-sequence responses
- NWP firmware issue

### 3. SL_DEVICE_EVENT_FATAL_NO_CMD_ACK

**Cause:** NWP didn't acknowledge a command.

**Triggers:**
- Command sent but no response received
- NWP crashed or hung

### 4. SL_DEVICE_EVENT_FATAL_CMD_TIMEOUT

**Cause:** Async event timeout.

**Triggers:**
- Expected async event never arrived
- NWP stuck processing

### 5. SL_DEVICE_EVENT_FATAL_DEVICE_ABORT

**Cause:** NWP internal abort.

**Triggers:**
- NWP firmware assertion
- NWP internal error

**Additional data available:**
- `AbortType` - Type of abort
- `AbortData` - Additional context

## Observed Error Pattern

### Debug Log Analysis
```
[MQTT::DEBUG] PU OK
[MQTT::DEBUG] COS 12 -> 18
[WIFI::ERROR] FATAL ERROR: Driver Abort detected
[WIFI::DEBUG] unexpected event: 536887296
```

### Decoding Event 536887296 (0x20004000)

| Bits | Value | Meaning |
|------|-------|---------|
| 0x20000000 | Unknown | Possibly async flag |
| 0x00004000 | `_SL_DRV_STATUS_BIT_DEVICE_STAT_IN_PROGRESS` | Stats collection active |

**Implication:** The NWP was in stats collection mode when a socket event occurred, causing a conflict.

### Error Sequence

1. MQTT publishes successfully
2. Stats collection may be running (0x4000 flag active)
3. HTTP client attempts connection
4. SPI communication fails - NWP busy with stats
5. Driver abort or sync loss occurs
6. System attempts recovery but encounters repeated failures

## Root Cause Analysis

The `sl_DeviceStatStart()`, `sl_DeviceStatGet()`, and `sl_DeviceStatStop()` APIs put the NWP into a special mode for collecting WiFi statistics. During this time:

1. The NWP may not respond to other commands promptly
2. Socket operations may be delayed or fail
3. SPI communication timing is affected

Even with the pause/resume mechanism for HTTP and MQTT clients, there can be:
- Race conditions between pause notification and ongoing operations
- NWP internal state conflicts
- SPI bus timing issues

## Mitigation Strategies

### 1. Extended Pause Delay
Increase delay after PAUSE event to ensure all pending operations complete:
```c
// Current: 1 second
// Consider: 2-3 seconds or dynamic based on activity
sleep(2);
```

### 2. Check Driver Status Before Stats
```c
// Check if stats already in progress
if (SL_IS_DEVICE_STAT_IN_PROGRESS(status)) {
    // Skip this collection cycle
    return;
}
```

### 3. Skip Stats After Errors
```c
static uint32_t lastErrorTime = 0;

if ((currentTime - lastErrorTime) < ERROR_COOLDOWN_PERIOD) {
    // Skip stats collection after recent error
    return;
}
```

### 4. Reduce Stats Frequency
Increase interval from 60 seconds to 120+ seconds to reduce conflict probability.

### 5. Check Socket Activity
```c
// Don't collect stats if sockets are active
if (activeSocketCount > 0) {
    // Reschedule for later
    StartAsyncEvtTimer(10);  // Retry in 10 seconds
    return;
}
```

### 6. Use sl_WlanRxStatGet() Instead
This simpler API doesn't require start/stop and may be less intrusive:
```c
SlWlanGetRxStatResponse_t rxStat;
sl_WlanRxStatGet(&rxStat, 0);
// Note: May return less accurate RSSI values
```

## Recovery Handling

Current recovery in `cc3220sf.c`:
```c
void SimpleLinkFatalErrorEventHandler(SlDeviceFatal_t *slFatalErrorEvent)
{
    // Log the error
    LOG_ERROR("FATAL ERROR: ...");

    // Signal state machine to handle
    netdev_wlan_cc3220sf_send_event(CC3220SF_EVENT_FATAL_ERROR);
}
```

The state machine then:
1. Notifies clients of disconnect
2. Stops the NWP (`sl_Stop()`)
3. Restarts the NWP (`sl_Start()`)
4. Attempts to reconnect

## References

- TI SimpleLink SDK: `source/ti/drivers/net/wifi/source/driver.c`
- Protocol definitions: `source/ti/drivers/net/wifi/source/protocol.h`
- Device events: `source/ti/drivers/net/wifi/device.h`
- TI E2E Forum: https://e2e.ti.com/support/wireless-connectivity/wifi/

## Recommendations

1. **For Production:** Consider disabling stats collection if stability is critical
2. **For Debugging:** Enable NWP logging to capture internal state during failures
3. **For Long-term:** Evaluate alternative WiFi modules (ESP32) if reliability issues persist
