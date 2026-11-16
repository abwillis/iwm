# M3: 9560 RFH receive queue and ALIVE

## Diagnosis from the target trace

The factory MAC address is not the failure.  The driver reads the OTP address as
`d4:3b:04:c3:04:74`, and XWLAN's `OID_802_3_CURRENT_ADDRESS` request completes
with six bytes and success status.

The trace contains 874 complete GenMAC requests.  The only protocol failures are
eight `OID_802_11_BSSID_LIST_SCAN` requests, all correctly reported as
`NDIS_STATUS_ADAPTER_NOT_READY`.  INIT firmware transfers nine sections and
548,520 bytes, but no ALIVE notification reaches host memory.

The missing prerequisite was RFH queue 0.  The 9000-family firmware reports
ALIVE as an RX packet.  Releasing the secure CPUs before programming the RFH
free ring, used ring, status block, and receive buffers leaves firmware with no
DMA destination.

## Implementation

`core/iwm_rx.c` and `include/iwm_rx.h` now implement the bounded queue needed for
boot:

- 512-entry free descriptor ring (64-bit pre-AX210 MQ descriptors);
- 512-entry used descriptor ring (32-bit buffer IDs);
- coherent `iwm_rx_status` block;
- 512 contiguous 4 KiB DMA32 receive buffers;
- queue-0 RFH programming through the PRPH gateway;
- initial producer index of 504, preserving the one-empty-entry rule and the
  hardware's eight-entry write-index granularity;
- bounded draining, buffer recycling, length validation, and ALIVE parsing;
- validation of ALIVE status `0xcafe` before the loader reports success.

The firmware loader binds and programs the queue before releasing CPU1.  The
CSR ALIVE bit remains a diagnostic signal; only a valid ALIVE packet satisfies
the boot wait.

## Target diagnostics

A target run now prints RX DMA addresses during attach:

```text
iwm: rxq free XXXXXXXX used XXXXXXXX status XXXXXXXX buffers XXXXXXXX/2097152
```

The INIT summary adds notification and queue state:

```text
iwm: init load rc N ... aliveirq N aliventf N ... rx N bad N closed N
```

Expected success is `rc 0`, `aliventf 1`, `rx` at least 1, and an ALIVE line with
status `cafe`.  `aliveirq` may be zero if the ISR acknowledged a receive cause
rather than the legacy ALIVE cause; the validated DMA packet is authoritative.

## Safety boundary

This change completes the missing receive/ALIVE boot transport, not the whole
802.11 stack.  INIT ALIVE deliberately leaves firmware in LOADING state.  A
working network interface still requires a command TX queue, INIT NVM/PHY
commands, transition to runtime firmware and runtime ALIVE, firmware scan/result
commands, association/station state, key commands, and NDIS packet TX/RX.  The
driver must continue returning ADAPTER_NOT_READY for scan until those stages are
implemented and validated on hardware.
