# WRND32$ log analysis and implemented corrections

The supplied `iwm.txt` contains 940 lines, including 874 complete GenMAC
requests (865 GET and 9 SET requests). Hardware discovery, BAR mapping,
Resource Manager registration, MSI allocation, firmware transfer and every FH
section DMA completed.  The decisive failure is the final firmware hand-off:

```text
iwm: init load rc -6 stage 7 sections 9 bytes 548520 dst 80454000 aliveirq 0 csr 08000000 fh 00000001 load ffffffff ints 1
```

The corrected loader now fails closed after the ALIVE timeout. The status value
`ffffffff` shows that both CPU load-status halves were committed. The missing
piece is the receive queue through which 9000-family firmware DMA-delivers the
ALIVE notification.

## GenMAC failures in the trace

| OID | Operation | Count | Old result | Meaning |
|---|---:|---:|---|---|
| `0x0d01011a` | SET BSSID-list scan | 8 | `IWM_ESTATE` / `NDIS_STATUS_ADAPTER_NOT_READY` | Correct while firmware has not reported ALIVE. |

All other 866 requests succeed. In particular, the one six-byte
`OID_802_3_CURRENT_ADDRESS` query succeeds after attach.

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
- The 9000-family RFH queue-0 free/used/status rings and 512 aligned receive
  buffers are allocated and programmed before secure CPU release. The loader
  now accepts only a well-formed ALIVE packet with status `0xcafe`.

## Remaining target-only boundary

The supplied trace proves that the old tree never gave firmware an RX DMA
destination. No source-only validation can certify the RFH register sequence;
the next hardware log must show `aliventf 1`, an RX packet count, and ALIVE
status `cafe`.

Until ALIVE succeeds, the driver must not fabricate scan results, association,
key installation, or packet traffic. The factory MAC is already obtained from
the OTP CSR registers. After INIT ALIVE, the remaining target path is: build
the command queue; read NVM and send PHY configuration; load and validate
runtime firmware; issue and parse scan commands; then implement association,
key commands and NDIS TX/RX.
