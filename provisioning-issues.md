# WiFi Provisioning — NWP State Corruption Issues

**SDK Version:** SimpleLink CC32xx SDK 7.10.00.13
**MCU:** CC3220SF (P/N: CC3220SF23ARGKT)

## Summary

The CC3220SF NWP enters an unrecoverable state when `sl_WlanProvisioning()` is called after certain sequences of failed WiFi connection attempts and profile deletions. The NWP crashes during the provisioning startup, and the device cannot recover without a physical button press that triggers a specific NWP reset sequence.

## Issue 1: sl_WlanProvisioning Crashes After Failed Credentials

### Reproduction

1. Device provisions successfully (AP mode, user enters WiFi credentials)
2. Credentials are wrong — NWP tries to connect, times out repeatedly
3. User triggers re-provisioning (button press or firmware command)
4. `sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES)` is called in STA mode
5. `sl_WlanProvisioning(SL_WLAN_PROVISIONING_CMD_START_MODE_APSC, ...)` is called
6. **NWP crashes** — `Fatal Error detected!`
7. Device reboots — NWP crashes again on provisioning attempt
8. **Infinite crash loop** — device is bricked until manual intervention

### Root Cause

The NWP's internal state becomes corrupted after the failed connection cycle. Calling `sl_WlanProfileDel` while the NWP is in STA mode does not clean up all internal state. Subsequent `sl_WlanProvisioning` attempts fail because the NWP cannot transition cleanly to AP provisioning mode.

### Workaround: AP Mode Reset Cycle

Based on the TI out-of-box example (`provisioning_task.c`), profiles must be deleted while the NWP is in **AP mode**, not STA mode. The working sequence is:

```c
// 1. Stop NWP
sl_Stop(SL_STOP_TIMEOUT);
sleep(2);

// 2. Start in any mode
sl_Start(0, 0, 0);

// 3. Switch to AP mode
sl_WlanSetMode(ROLE_AP);
sl_Stop(SL_STOP_TIMEOUT);
sleep(1);
sl_Start(0, 0, 0);
// Verify ROLE_AP

// 4. Delete profiles while in AP mode
sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES);

// 5. Set connection policy
sl_WlanPolicySet(SL_WLAN_POLICY_CONNECTION,
                 SL_WLAN_CONNECTION_POLICY(1, 0, 0, 0), NULL, 0);

// 6. Stop NWP
sl_Stop(SL_STOP_TIMEOUT);
sleep(1);

// 7. Restart — NWP is now clean, provisioning will work
sl_Start(0, 0, 0);
```

This sequence must be performed **before** the SlWifiConn state machine starts. We implement it in `SlWifiConn_init()` when a button-triggered provisioning request is detected.

### Key Finding

The TI out-of-box example (`provisioning_task.c` lines 746-828) follows this exact pattern — switch to AP, delete profiles, configure policies, stop. This is not documented as a requirement in the SDK documentation.

## Issue 2: SlWifiConn State Machine Not Designed for Re-Provisioning

### Problem

The `SlWifiConn` connection manager in the SDK has no clean path from "connected with bad credentials" to "provisioning mode". The state machine:

1. After connection timeout, calls `g_fEnterPROVISIONING()` which calls `SetApParams()` then `sl_WlanProvisioning()`
2. `SetApParams()` uses `g_settings` which may have NULL SSID (not configured by application)
3. If `g_ctx->role == ROLE_AP`, it calls `ResetNWP()` which does `sl_Stop`/`sl_Start` — this can crash if the NWP is in a bad state
4. The `ENABLE`/`DISABLE` event sequence during provisioning startup conflicts with the provisioning events

### Specific Code Location

```c
// slwificonn.c — WAIT_FOR_CONN state timeout handler
case SLWIFICONN_EVENT_TIMEOUT:
    if(g_ctx->provMode != WifiProvMode_OFF) {
        retVal = g_fEnterPROVISIONING();  // ← crashes here
    }
```

```c
// slwificonn.c — EnterPROVISIONING
static int EnterPROVISIONING(void) {
    retVal = SetApParams();              // ← may call ResetNWP()
    retVal = sl_WlanProvisioning(...);   // ← crashes on dirty NWP
}
```

### Recommendation

The SlWifiConn state machine should:
1. Always perform the AP mode reset cycle before starting provisioning
2. Clear `provMode` after successful provisioning to prevent re-entry on timeout
3. Provide a clean API for "delete profiles and prepare for provisioning" that handles the NWP state transitions internally
4. Not call `sl_WlanProvisioning` without first ensuring the NWP is in a known-good state

## Issue 3: FORCE_PROVISIONING Flag

### Problem

`SLWIFICONN_PROV_FLAG_FORCE_PROVISIONING` sets `bDeleteAllProfiles = true`, which causes `sl_WlanProfileDel(SL_WLAN_DEL_ALL_PROFILES)` during the `SLWIFICONN_EVENT_ENABLE` handler. However, this deletion happens in STA mode (not AP mode), which leaves the NWP in a corrupt state.

### Recommendation

The `FORCE_PROVISIONING` flag should perform the AP mode reset cycle (Issue 1 workaround) instead of a simple `sl_WlanProfileDel` in STA mode.

## Environment

```
Concurrent RTOS Tasks: HTTP client, MQTT, OTA, Engine, Reefer, RTP
WiFi Mode: STA with DHCP
Provisioning Mode: AP+SC (SL_WLAN_PROVISIONING_CMD_START_MODE_APSC)
Provisioning Trigger: Physical button (B1+B4) at boot
```

## Files

- `slwificonn.c` — Modified SlWifiConn with `ResetNWPProfileForProvisioning()` function
- `slwificonn.h` — Updated `SlWifiConn_init` signature with `deleteProfiles` parameter
