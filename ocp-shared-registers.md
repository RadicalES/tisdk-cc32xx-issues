# OCP Shared Registers — Application Processor Access Restrictions

**SDK Version:** SimpleLink CC32xx SDK 7.10.00.13
**MCU:** CC3220SF (P/N: CC3220SF23ARGKT)

## Summary

The OCP Shared spare registers (`OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_x`) cannot be safely used by the application processor for persistent storage across warm reboots. Both reading and writing these registers from application code causes NWP instability.

## Registers Tested

| Register | Address | Result |
|----------|---------|--------|
| SPARE_REG_0 | 0x4402E068 | **Bus fault on read** (Hard-fault: PRECISERR) |
| SPARE_REG_1 | 0x4402E06C | **Bus fault on read** |
| SPARE_REG_2 | 0x4402E070 | **Bus fault on read** |
| SPARE_REG_4 | — | Used by ITM (SWV trace) |
| SPARE_REG_5 | — | Used by NWP (`NWP_SPARE_REG_5` in cc_pal.c for SLSTOP signaling) |
| SPARE_REG_6 | — | Read works, **write causes NWP POWERED DOWN/UP cycles** |
| SPARE_REG_7 | — | Read works, **write causes NWP POWERED DOWN/UP cycles** |
| SPARE_REG_8 | — | Read works, **write causes NWP POWERED DOWN/UP cycles** |

## Observed Behaviour

### Registers 0-2: Bus Fault on Read

Reading SPARE_REG_0/1/2 from `probe()` or `sm_start()` (before or after `sl_Start`) causes an immediate bus fault:

```
Error raised: Bus-fault: PRECISERR: Immediate Bus Fault, exact addr known, address: 0x4402e068
```

These registers appear to be in a power domain that is not accessible from the application processor, regardless of NWP state.

### Registers 6-8: Write Causes NWP Instability

Reading SPARE_REG_6/7/8 works correctly after `sl_Start()`. However, **writing** to these registers during NWP operation causes the NWP to power-cycle (POWERED DOWN → POWERED UP). In testing, writing on every disconnect and fatal error event produced 747 NWP fatal errors overnight (approximately one every 2 minutes).

Removing the writes immediately resolved the instability — the device ran 24+ hours with only 2 fatal errors.

### TI's Own Usage

TI's WiFi driver (`cc_pal.c`) uses SPARE_REG_5 for NWP stop signaling:

```c
#define NWP_SPARE_REG_5  (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_5)
#define NWP_SPARE_REG_5_SLSTOP  (0x00000002)

void NwpPowerOn(void) {
    HWREG(NWP_SPARE_REG_5) &= ~NWP_SPARE_REG_5_SLSTOP;
    // ...
}
```

This access occurs during `NwpPowerOn`/`NwpPowerOff` — tightly controlled sequences. General-purpose application access is not safe.

## Recommendation

Do not use OCP Shared spare registers for application data storage on the CC3220SF. Use external SPI flash or NWP filesystem (`sl_FsWrite`) for persistent storage instead.

## Workaround Implemented

We replaced OCP register usage with external SPI flash (Macronix 32MB) for NWP connection statistics. A 32-byte struct is read on boot and flushed every 30 seconds with anti-wear protection (dirty flag, binary semaphore locking).
