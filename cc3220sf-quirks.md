# List of Firmware Quirks
Document all weird behaviour of the firmware and undocumented quirks

## Access OCP Spare Registers
Do not use these as a place to store data to survive a restart. During testing we tried all SPARE register to store NWP statistcs for fatal errors, recovered errors and disconnections.
We used it as below:
```
#define NWP_STATS_REG_FATAL    (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_8)
#define NWP_STATS_REG_RECOVER  (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_7)
#define NWP_STATS_REG_DISCON   (OCP_SHARED_BASE + OCP_SHARED_O_SPARE_REG_6)
```
### WARNING: 
Writing to OCP_SHARED registers during NWP operation causes
NWP power-down/up cycles and instability. Registers 0-5 are reserved
by NWP/ITM. Registers 6-8 can be read after sl_Start but must NOT be
written while the NWP is running.
