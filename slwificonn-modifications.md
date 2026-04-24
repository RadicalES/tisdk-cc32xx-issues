# SlWifiConn Module Modifications

**SDK Version:** SimpleLink CC32xx SDK 7.10.00.13
**File:** `source/ti/drivers/net/wifi/slnetif/slwificonn.c`

## Summary

The `slwificonn.c` connection manager module required several modifications to achieve stable operation in a production multi-threaded environment. The original implementation had insufficient error recovery, a stack overflow in the timer thread, and no mechanism for WiFi statistics collection.

---

## 1. Timer Thread Stack Overflow Fix

**Problem:** The timer thread stack size was set to 256 bytes, which is insufficient for the POSIX timer callback that processes deferred events. Additionally, the `pthread_attr_t` for the timer thread was a local variable in `SlWifiConn_init()` — it was destroyed when the function returned, but the timer thread continued to reference it, causing intermittent crashes.

**Changes:**
- Increased `TIMER_TASK_STACK_SIZE` from 256 to 1024
- Changed `pthread_attr_t timerThreadAttr` from a local variable to a static `g_timerThreadAttr` so it persists after init returns
- Uses `pthread_attr_setstacksize()` instead of direct struct member access

---

## 2. Exponential Backoff on NWP Reset

**Problem:** When the NWP encounters a fatal error, `ResetNWP()` immediately calls `sl_Stop()`/`sl_Start()` with no delay. If the NWP is in a bad state, rapid reset cycles can make the situation worse, leading to repeated SYNC_LOSS or DRIVER_ABORT errors.

**Changes to `ResetNWP()`:**
- Tracks consecutive fatal errors via `fatalErrorCount` in the context struct
- Applies increasing delay before restart using `g_recoveryDelays[]`: 2s, 5s, 10s, 20s, 30s
- After `sl_Start()`, applies a 2-second stabilization delay
- Reinitializes connection policy via `InitNWPConnPolicy()` after reset (policy may change after sl_Stop/sl_Start cycle)
- Sets power policy to `SL_WLAN_LOW_LATENCY_POLICY` after reset

**New fields in `SlWifiConn_t`:**
```c
uint8_t fatalErrorCount;    // Consecutive fatal errors
uint8_t recoveryDelayLevel; // Current backoff level (0-4)
```

---

## 3. Error Tracking Reset on Successful Connection

**Problem:** Without resetting the error counters on successful connection, the backoff delay would remain at the maximum level permanently after any error sequence.

**Changes to `EnterCONNECTED()`:**
- Resets `fatalErrorCount` and `recoveryDelayLevel` to 0 when connection is successfully established
- Logs recovery for diagnostics

---

## 4. WiFi Statistics Collection (`WIFI_STATS`)

**Problem:** No built-in mechanism to periodically collect WiFi RSSI statistics. The `sl_DeviceStatStart/Get/Stop` APIs require careful sequencing and must not overlap with other network operations.

**New functions (compiled with `#ifdef WIFI_STATS`):**

- **`CollectAndNotifyStats()`** — Internal function that:
  1. Verifies device is still in CONNECTED state
  2. Notifies application to pause network clients (`WifiConnEvent_STATS_PAUSE`)
  3. Waits 1 second for pending operations to complete
  4. Calls `sl_DeviceStatStart(0)`, waits 5 seconds for RSSI accumulation
  5. Calls `sl_DeviceStatGet()` and `sl_DeviceStatStop()`
  6. Notifies application to resume clients (`WifiConnEvent_STATS_RESUME`)
  7. Sends results via `WifiConnEvent_STATS_RESULT` callback
  8. Reschedules timer for next collection

- **`SlWifiConn_enableStats(uint16_t interval)`** — Public API to enable periodic collection (minimum 10 second interval). Starts the async timer immediately if already connected.

- **`SlWifiConn_disableStats()`** — Public API to disable collection and stop the timer.

**New fields in `SlWifiConn_t`:**
```c
bool     bStatsEnabled;   // Statistics collection enabled
uint16_t statsInterval;   // Collection interval in seconds
```

**New types:**
```c
typedef struct {
    uint8_t  bssid[6];
    int16_t  mgMntRssi;     // Management RSSI
    int16_t  dataCtrlRssi;  // Data/Control RSSI
} SlWifiConnStats_t;
```

**State machine integration:**
- `SLWIFICONN_EVENT_TIMEOUT` in CONNECTED state triggers `CollectAndNotifyStats()` when stats are enabled
- Timer is stopped on disconnect/IP lost events
- Timer is restarted when entering CONNECTED state if stats are enabled

---

## 5. WLAN Profile Checking at Init

**Problem:** The state machine had no visibility into whether stored WLAN profiles existed before starting. This information is needed to decide whether to enter provisioning mode or wait for auto-connect.

**Changes to `SlWifiConn_init()`:**
- After NWP is initialized but before state machine starts, scans all 8 profile slots using `sl_WlanProfileGet()`
- Stores result in static `g_profileCount` (-1 = not checked, 0 = none, >0 = count)

**New function:**
- `SlWifiConn_getProfileCount()` — Returns the number of stored profiles

---

## 6. Reset Request from Connected State

**Problem:** There was no way to trigger an NWP reset while in the CONNECTED state. If the NWP becomes unreliable but doesn't trigger a fatal error (e.g., socket operations failing intermittently), the application had no mechanism to force a reset.

**Changes to `smState_CONNECTED()`:**
- Added `SLWIFICONN_EVENT_RESET_REQUEST` handler that:
  1. Stops stats timer (if active)
  2. Notifies NetIf of disconnect
  3. Calls `ResetNWP()` (with exponential backoff)
  4. Enters `WAIT_FOR_CONN` state with reconnect timeout

**New function:**
- `SlWifiConn_requestReset()` — Public API to signal a reset request via the event queue

---

## 7. Power Policy: Always Awake

**Problem:** NWP power management transitions (sleep/wake) can cause SPI timing issues between the host MCU and NWP, leading to SYNC_LOSS fatal errors.

**Changes:**
- Sets `SL_WLAN_LOW_LATENCY_POLICY` at init in `SlWifiConn_init()`
- Also reapplied after every NWP reset in `ResetNWP()`
- Trade-off: Higher power consumption for improved SPI communication reliability

---

## 8. AP Mode Reset for Profile Deletion (`ResetNWPProfileForProvisioning`)

**Problem:** Deleting WiFi profiles via `sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES)` while the NWP is in STA mode leaves the NWP in a corrupted state. Subsequent calls to `sl_WlanProvisioning()` crash the NWP, causing an infinite reboot loop that requires a full reflash to recover.

**Root cause:** The NWP's internal state is not fully cleaned when profiles are deleted in STA mode. The TI out-of-box example (`provisioning_task.c`) demonstrates that profiles must be deleted while the NWP is in AP mode.

**New function `ResetNWPProfileForProvisioning()`:**
1. `sl_Stop()` — stop NWP
2. `sl_Start()` — restart
3. `sl_WlanSetMode(ROLE_AP)` — switch to AP mode (stop/start if needed)
4. `sl_WlanProfileDel(0xFF)` — delete all profiles while in AP mode
5. `sl_WlanPolicySet()` — set connection policy to Auto
6. `sl_Stop()` — stop NWP (caller restarts in STA mode)

**Integration:**
- `SlWifiConn_init()` accepts new `deleteProfiles` parameter
- When true (button provisioning requested), calls `ResetNWPProfileForProvisioning()` before the profile count check, then restarts with `sl_Start()`
- Profile check finds 0 profiles → provisioning starts via normal path
- No `FORCE_PROVISIONING` flag needed — profiles are already cleanly deleted

---

## 9. SlWifiConn_init API Change

**Change:** `SlWifiConn_init(SlWifiConn_AppEventCB_f fWifiAppCB)` now accepts an additional parameter:

```c
int SlWifiConn_init(SlWifiConn_AppEventCB_f fWifiAppCB, uint8_t deleteProfiles);
```

When `deleteProfiles` is non-zero, `ResetNWPProfileForProvisioning()` runs before the profile check to ensure clean provisioning. The application passes `board_is_provisioning()` which returns true when the physical button (B1+B4) was pressed at boot.

---

## 10. SlWifiConn_setDeleteAllProfiles API

**New function:** Sets the `bDeleteAllProfiles` flag in the SlWifiConn context for use by the ENABLE event handler.

```c
void SlWifiConn_setDeleteAllProfiles(void);
```

This is an alternative to `SLWIFICONN_PROV_FLAG_FORCE_PROVISIONING` that can be called before `SlWifiConn_enable()`. Not currently used in production — `ResetNWPProfileForProvisioning()` in init is the preferred approach.

---

## Known Limitations

### Re-provisioning After Bad Credentials (Without Button)

If the device boots with no profiles (e.g., after a web-based profile reset) but without the physical button press, provisioning may fail. The `ResetNWPProfileForProvisioning` cycle only runs when `deleteProfiles` is true (button pressed). Without it, `sl_WlanProvisioning()` is called on a potentially dirty NWP and crashes.

**Workaround:** Always use the physical button (B1+B4) to trigger re-provisioning. The web-based "Reset WiFi" button deletes profiles but requires a subsequent button press to enter provisioning cleanly.
