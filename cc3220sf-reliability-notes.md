# CC3220SF WiFi Co-Processor Reliability Notes

## Overview

The TI CC3220SF uses a dual-processor architecture with a dedicated Network Processor (NWP) handling WiFi operations. While this offloads networking from the main application, it introduces complexity and reliability challenges documented by many developers.

## Known Issues

### 1. NWP Fatal Abort Errors

The NWP can encounter fatal errors requiring reset:

- `SL_DEVICE_EVENT_FATAL_DEVICE_ABORT` - Triggered by NWP communication issues or internal abort
- `SL_DEVICE_EVENT_FATAL_NO_CMD_ACK` - Command acknowledgment timeout
- `SL_DEVICE_EVENT_FATAL_CMD_TIMEOUT` - Async event timeout

**Recovery varies by error type:**
- Some errors require only NWP reset (`sl_Stop()` / `sl_Start()`)
- Others require full MCU reset

**References:**
- https://e2e.ti.com/support/wireless-connectivity/wi-fi-group/wifi/f/wi-fi-forum/1038544/cc3220sf-launchxl-provisioning-process-fatal-error-abort-nwp-event-detected-aborttype-1-abortdata-0x5b883
- https://e2e.ti.com/support/wireless-connectivity/wifi/f/wi-fi-forum/724931/rtos-cc3220-what-can-i-do-for-sl_device_event_fatal_device_abort-error

### 2. API Context Restrictions (Critical)

**TI explicitly states:** "It is prohibited to call any sl_ API from interrupt or asynchronous handler context."

Violations cause:
- Driver crashes
- RTOS instability
- I2C/peripheral conflicts when SimpleLink APIs called from timer callbacks

**Best Practice:** Always call SimpleLink APIs from application thread context. Use flags/events to signal from callbacks and process in main thread.

**References:**
- https://e2e.ti.com/support/wireless-connectivity/wifi/f/wi-fi-forum/820662/cc3220sf-i2c-transmit-issues-if-simplelink-api-called-from-timer-callback

### 3. Random Disconnections

Reported causes:
- Power management policy (sleep modes) causing disconnections
- AP compatibility issues (certain routers problematic)
- WPA Enterprise timing issues (disconnects after ~100 seconds)
- TCP keep-alive timeout mismatches

**Workarounds:**
- Set power policy to `SL_WLAN_LOW_LATENCY_POLICY` (increases power consumption)
- Configure TCP keep-alive: `sl_SetSockOpt()` with `SL_SO_KEEPALIVE` and `SL_SO_KEEPALIVETIME`
- Test with multiple AP vendors

**References:**
- https://e2e.ti.com/support/wireless-connectivity/wi-fi-group/wifi/f/wi-fi-forum/735376/cc3220-reset-every-5mins-over-and-over-again-when-testing-the-wifi-door-lock-ref-design
- https://e2e.ti.com/support/wireless-connectivity/wi-fi-group/wifi/f/wi-fi-forum/865475/cc3220moda-wi-fi-disconnects-after-100-seconds-from-wpa-enterprise-network-with-security-type-eap-tls

### 4. Race Conditions

- FastConnect can complete before application networking code is ready
- Socket/DNS calls may fail if WiFi connection isn't established
- Zephyr RTOS documents specific race between driver init and app startup

**References:**
- https://github.com/zephyrproject-rtos/zephyr/issues/11889

### 5. Statistics Collection Issues

The `sl_DeviceStatStart()`, `sl_DeviceStatGet()`, `sl_DeviceStatStop()` APIs are particularly sensitive:
- Must be called from correct thread context
- Cannot run while other network operations are in progress
- Require pausing HTTP/MQTT clients before collection
- Need sufficient delay (1-2 seconds) after pausing clients

## Recommended Practices

### Thread Safety
```c
// WRONG - calling from timer callback
void TimerCallback(void) {
    sl_WlanDisconnect();  // Will crash!
}

// CORRECT - signal from callback, process in thread
void TimerCallback(void) {
    Event_post(eventHandle, EVENT_DISCONNECT);
}

void AppThread(void) {
    Event_pend(eventHandle, EVENT_DISCONNECT, ...);
    sl_WlanDisconnect();  // Safe in thread context
}
```

### Error Recovery
```c
void SimpleLinkFatalErrorEventHandler(SlDeviceFatal_t *pError) {
    switch(pError->Id) {
    case SL_DEVICE_EVENT_FATAL_DEVICE_ABORT:
        // Signal main thread to reset NWP
        SignalEvent(EVENT_NWP_RESET);
        break;
    }
}
```

### Power Management
```c
// For maximum stability (at cost of power)
sl_WlanPolicySet(SL_WLAN_POLICY_PM, SL_WLAN_LOW_LATENCY_POLICY, NULL, 0);
```

### Service Pack Updates
Keep the NWP service pack updated - many fatal errors are fixed in newer versions. Check TI's SimpleLink SDK releases for service pack updates.

## Alternatives Considered

| Feature | CC3220SF | ESP32 |
|---------|----------|-------|
| Architecture | Dual-core (App + NWP) | Single/Dual-core |
| WiFi Stack | Runs on NWP (closed) | Runs on main core (open) |
| Security | Hardware crypto, secure boot | Hardware crypto |
| Community | Limited | Large, active |
| Debugging | NWP is black box | Full visibility |
| Reliability | Sensitive to API context | More forgiving |

The CC3220SF's hardware security features (secure boot, hardware crypto, secure storage) are superior, but the ESP32 offers better debuggability and community support.

## Project-Specific Notes

### WiFi Statistics Collection

Our implementation pauses HTTP/MQTT clients during stats collection to avoid NWP conflicts:

1. Send PAUSE event to clients
2. Wait 1 second for operations to complete
3. Call `sl_DeviceStatStart()`
4. Wait 2 seconds for RSSI accumulation
5. Call `sl_DeviceStatGet()` and `sl_DeviceStatStop()`
6. Send RESUME event to clients

Total blocking time: ~3 seconds per collection (runs every 60 seconds in production).

### State Verification

Always verify WiFi is still connected before/after blocking operations:
```c
if(g_ctx->state != SLWIFICONN_STATE_CONNECTED) {
    // Abort operation, reschedule
    return -1;
}
```

## Reliability Improvements Implemented

### 1. Exponential Backoff on Fatal Errors

In `slwificonn.c`, consecutive fatal errors trigger increasing delays before NWP reset:
- 1st error: 2 second delay
- 2nd error: 5 second delay
- 3rd error: 10 second delay
- 4th error: 20 second delay
- 5th+ errors: 30 second delay

This prevents rapid reset cycles that can destabilize the NWP.

### 2. Power Policy: Always Awake

Set `SL_WLAN_LOW_LATENCY_POLICY` policy to prevent power management transitions that can cause SPI timing issues with the NWP. Trade-off: Higher power consumption for better stability.

### 3. Error Tracking and System Reboot Threshold

In `cc3220sf.c`, fatal errors are tracked. After 10 consecutive fatal errors without successful reconnection:
1. `hal_reset()` is called to signal RTS for graceful shutdown
2. The WiFi state machine hangs in an infinite loop to prevent further issues
3. RTS watchdog handles the actual system reboot

Error counter resets when connection is successfully established.

### 4. Recovery Delay After NWP Reset

After `sl_Stop()`/`sl_Start()` cycle, a 2-second stabilization delay is enforced before resuming operations.

## Conclusion

The CC3220SF is usable in production but requires:
- Strict adherence to API context rules
- Robust error recovery mechanisms
- Careful timing around network operations
- Thorough AP compatibility testing

The NWP architecture makes debugging difficult since the WiFi stack is a "black box". Plan for field issues that may require firmware updates to address NWP-related problems.
