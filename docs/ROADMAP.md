# Porting roadmap and acceptance tests

## M0 — portable core (included)

- Device table covers the PCI IDs in the selected BSD `iwm` snapshot.
- 9560 configuration is explicit.
- Firmware IOCTL state machine rejects out-of-order, oversize and bad-CRC transfers.
- GenMAC discovery/status OIDs respond.
- Host tests pass with warnings treated as errors.

## M1 — OS/2 PCI and memory

- Detect a target adapter without touching unsupported hardware.
- Map BAR0 and read a stable hardware revision.
- Allocate/free aligned physical blocks repeatedly without leaks.
- Register and release the IRQ cleanly.
- Verify operation on uniprocessor and SMP kernels with the ISR disabled, then enabled.

## M2 — firmware ALIVE

- Parse the TLV header and every record with overflow checks.
- Load init/runtime sections using a physical bounce buffer.
- Receive ALIVE and print firmware version/API.
- Repeat cold boot, warm reset, failed transfer and firmware reload 100 times.
- Stop bus-master DMA before every reset/free path.

## M3 — NVM, scan and XWLAN discovery

- Read NVM/MAC address and regulatory channel data.
- Implement BSSID list scan and return `NDIS_802_11_BSSID_LIST_EX` records.
- Return SSID, BSSID, RSSI, media state and link speed.
- XWLAN lists the adapter and visible access points without traps or leaks.

## M4 — open association

- Implement PHY/MAC/binding contexts, station add/remove and time events.
- Associate/disassociate with an open test AP.
- Pass EAPOL EtherType frames unmodified through NDIS.
- Complete TX and recycle RX descriptors under packet loss and reconnects.

## M5 — WPA2-PSK with wpa_supplicant

- Implement authentication/encryption/privacy OIDs.
- Implement add/remove pairwise and group keys with strict length/index checks.
- Complete 4-way and group-key handshakes.
- Reassociate after AP loss and firmware recovery.
- Validate replay counters, multicast traffic and rekeying.

## M6 — performance and power

- Add additional TX queues and multi-queue RX.
- Add rate reporting, aggregation and calibrated power handling.
- Implement suspend/resume with firmware reload as required.
- Stress SMP traffic, shutdown, repeated DHCP and mixed interrupt load.

## Required diagnostics

Keep a fixed-size event ring with sequence number, timestamp/tick, state, CSR interrupt cause, FH cause, command ID, queue/index and status. Provide a read-only IOCTL to copy a bounded snapshot to ring 3. Never format long strings in the ISR.

