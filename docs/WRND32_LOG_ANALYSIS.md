# WRND32$ log analysis and implemented corrections

The supplied `iwm.txt` contains 935 lines, including 875 complete GenMAC
requests (866 GET and 9 SET requests).  Hardware discovery, BAR mapping,
Resource Manager registration, MSI allocation, firmware transfer and every FH
section DMA completed.  The decisive failure is the final firmware hand-off:

```text
iwm: init load rc 0 stage 5 sections 9 bytes 548520 dst 80454000 aliveirq 0
```

The old loader returned success after an ALIVE timeout.  It therefore left the
firmware session in `LOADING`, while XWLAN repeatedly attempted requests that
require a running firmware image.

## GenMAC failures in the trace

| OID | Operation | Count | Old result | Meaning |
|---|---:|---:|---|---|
| `0x0d01011b` | GET encryption status | 169 | `IWM_ENOTSUP` / `NDIS_STATUS_NOT_SUPPORTED` | XWLAN's status query stops before reading the BSSID. |
| `0x0d01011a` | SET BSSID-list scan | 9 | `IWM_ESTATE` / `NDIS_STATUS_ADAPTER_NOT_READY` | Correct while firmware has not reported ALIVE. |
| `0x01010102` | GET current 802.3 address | 1 | `IWM_ENOTSUP` / `NDIS_STATUS_NOT_SUPPORTED` | This is `OID_802_3_CURRENT_ADDRESS`, not `OID_GEN_HARDWARE_STATUS`; its six-byte buffer confirms the interpretation. |

## Corrections in this tree

- A missing early ALIVE now returns `IWM_ETIMEDOUT`, moves the loader and
  firmware session to `FAILED`, masks interrupts, and records CSR interrupt,
  FH interrupt and ucode-load-status snapshots.  Firmware COMMIT can no longer
  look successful after the timeout.
- The target log prints those three register snapshots on the INIT-load line.
- The MSI counter increments only for a real, non-`0xffffffff` interrupt
  cause.
- Authentication, encryption, privacy and power OIDs validate and retain the
  requested policy.  GET encryption status now returns the retained state, so
  XWLAN's normal status sequence can continue.  This policy state does not
  claim that a firmware key command was completed.
- `OID_802_3_CURRENT_ADDRESS` and `OID_802_3_PERMANENT_ADDRESS` are implemented.
  They return `NDIS_STATUS_ADAPTER_NOT_READY` until a validated NVM MAC address
  is installed, rather than returning an all-zero address.
- A bounded 32-entry scan cache and exact `NDIS_802_11_BSSID_LIST_EX`
  serializer are implemented.  Firmware notification code can add validated
  results with `iwm_genmac_scan_add()` and complete a scan with
  `iwm_genmac_scan_complete()`.

## Remaining target-only boundary

The supplied trace proves that the card never reaches firmware ALIVE.  No
source-only change can certify the missing target sequence without another
hardware run.  The next log must include the extended INIT-load line.  Its
`csr`, `fh`, and `load` values distinguish an interrupt-delivery problem from
a secure-CPU/load-status problem.

Until ALIVE succeeds, the driver must not fabricate a MAC address, scan
results, association, key installation, or packet traffic.  After ALIVE, the
remaining target path is: program the RX notification queue; parse ALIVE; read
NVM and install the MAC address; load and validate runtime firmware; issue and
parse scan commands; then implement association, key commands and NDIS TX/RX.
