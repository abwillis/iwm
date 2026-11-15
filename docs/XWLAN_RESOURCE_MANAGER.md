# XWLAN and Resource Manager integration

## Evidence from the WRND32$ trace

The supplied XWLAN run contains 880 GenMAC requests.  The IOCTL bridge is
working, but three request families expose the next boundaries:

- `OID_GEN_HARDWARE_STATUS` (`0x00010102`) was queried once and returned
  `NDIS_STATUS_NOT_SUPPORTED`.
- `OID_802_11_INFRASTRUCTURE_MODE` (`0x0d010108`) was queried 282 times and
  returned `NDIS_STATUS_NOT_SUPPORTED`.
- `OID_802_11_BSSID_LIST_SCAN` (`0x0d01011a`) was set 15 times and returned
  `IWM_ESTATE` without an NDIS status because firmware is still `LOADING`.

The first two OIDs are now implemented.  Infrastructure mode is persisted,
validated as IBSS/infrastructure/auto, and returned to XWLAN.  A scan attempted
before a validated firmware ALIVE now returns
`IWM_NDIS_STATUS_ADAPTER_NOT_READY` in the GenMAC parameter packet while the
OS/2 request packet completes normally.  This preserves the fail-closed state
without hiding the protocol result behind a generic `DosDevIOCtl` failure.

## Resource Manager registration

`os2/iwm_d32_rm.c` uses the Drv32 `Rm1` bridge for the MiniDDK Resource Manager
structures and registers:

1. `IWM32.OS2` as a static network driver.
2. The Intel Wireless-AC 9560 as a PCI network adapter.
3. BAR0 as the adapter memory resource.
4. The MSI vector returned by `PsdMsiAlloc` as an exclusive IRQ resource.
5. The PCI bus/device/function value as an adapter adjunct.

Registration happens before the MSI handler is installed.  A failed claim
unwinds the Resource Manager entries and MSI allocation instead of continuing
with an unowned resource.

Successful target initialization adds these diagnostics:

```text
iwm: RM driver registered
iwm: RM adapter registered bdf ... BAR0 ... MSI ... handles ... rc 0
```

## Remaining hardware boundary

These changes complete the observed XWLAN discovery/status compatibility and
Resource Manager ownership, but they do not manufacture scan results.  The
firmware trace still ends after INIT section loading with `aliveirq 0`.  The
driver must program the RX notification queue, parse the firmware ALIVE packet,
initialize the command queue, read NVM/MAC data, load runtime firmware, and
implement scan commands/results before XWLAN can list access points.  Returning
success before those operations would expose uninitialized DMA and invalid
wireless state.
