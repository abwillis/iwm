# IWM-OS2 starter

Experimental source scaffold for a native OS/2/ArcaOS NDIS Wi-Fi driver for the Intel `iwm` device families, with the Intel Dual Band Wireless-AC 9560 as the primary target.

**This remains a development driver, not a complete Wi-Fi driver.** It identifies the OS/2 boundaries, implements a tested device table, firmware-transfer protocol, core state machines, the XWLAN/GenMAC discovery and status surface observed in the supplied trace, OS/2 Resource Manager registration, the 8000/9000 FH service-channel section loader, and the 9560 RFH queue-0 receive transport needed for ALIVE. The loader DMA-copies INIT sections and now requires a validated ALIVE packet in host memory; it keeps INIT firmware in `LOADING` until the command/NVM/runtime sequence is implemented. Command TX, NVM/PHY commands, runtime transition, firmware scan commands, association, key installation and NDIS packet I/O remain target milestones.

## Primary 9560 target

The reference driver maps the 9560 to Intel PCI IDs `8086:9df0`, `8086:a370` and `8086:31dc`, family 9000, firmware alias `iwm9000fw`, multi-queue RX, integrated transport and a 650 µs crystal latency. The related 9260 uses `8086:2526` and alias `iwm9260fw`.

The project also includes IDs/configuration aliases for the 3160, 3165, 3168, 7260, 7265, 8260 and 8265 devices covered by the same BSD `iwm` snapshot.

## Architecture

- `core/` — portable device selection, firmware stream validation, CSR bring-up subset, DMA ring planning, GenMAC OIDs and NDIS-neutral packet hooks.
- `os2/` — OS/2-only boundary for DevHelp, PCI, selectors, interrupt registration, request packets and MultiMac NDIS glue.
- `tools/iwmfwld.c` — ring-3 firmware loader. It computes CRC32 and transfers the selected Intel `.ucode` file with BEGIN/DATA/COMMIT IOCTLs instead of embedding a large image in a 16-bit data segment.
- `tools/ssidconnect.c` — open-network test CLI and SSID scanner. It accepts OS/2 `/OPTION` or `-OPTION` forms, can refresh/list the GenMAC scan cache, or set an SSID, wait for the driver link acknowledgement, optionally reset stale classic DHCP state, and start DHCP on the requested `lanN` interface.
- `install/` — CONFIG.SYS, PROTOCOL.INI, NIF and STARTUP.CMD templates.
- `tests/` — host-side tests for device matching, the firmware protocol, ring sizing, RFH RX/ALIVE and GenMAC discovery.

## XWLAN and wpa_supplicant contract

The device exposes `WRND32$`, matching the GenMAC DIM used by XWLAN. Category `0x99`, functions `0x40/0x41`, carry OID GET/SET requests. The driver implements the private wrapper discovery OIDs plus hardware status, current/permanent address, infrastructure mode, SSID/BSSID/RSSI, staged authentication/encryption/privacy policy, power mode, media state, link speed and `NDIS_802_11_BSSID_LIST_EX` serialization. A validated MAC address, hardware scan initiation/results, disassociation, association and key OIDs remain gated by their hardware milestones.

XWLAN starts and controls the OS/2 `wpa_supplicant`. EAPOL frames must pass through the normal NDIS transmit/receive path. WPA2 cannot work until `OID_802_11_ADD_KEY`/`REMOVE_KEY`, station commands and hardware crypto setup are completed.

Only one driver exposing `WRND32$` may be loaded at a time.

## OS/2 memory and interrupt rules

- DMA descriptors and buffers come from physical, DMA-suitable allocations, never the C heap.
- Ring-3 pointers are mapped and copied through OS/2 request-packet/DevHelp services; hardware never receives a ring-3 address.
- 16-bit code accesses physical blocks through bounded GDT selector mappings or a verified flat helper.
- The ISR only reads/acknowledges causes, masks as needed and schedules deferred work. Firmware notifications, RX indication and TX completion run outside hard-interrupt context.
- Descriptor alignment is preserved: TX/RX descriptor rings use 256-byte alignment and the ICT uses 4096-byte alignment.

## Host verification

```sh
make test
```

This does not build an OS/2 driver. It verifies the portable core before target integration.

## OS/2 build start

1. Install Open Watcom 2.0 and NASM.
2. Use `Microsoft Copilot Chat Files/OS2Stuff`, starting with `COPILOT_OS2_START_HERE`, as the OS/2 reference; miniddk takes precedence over ddk. `MMROOT` is not used.
3. Run `build-ring3.cmd` to build `IWMFWLD.EXE`.
4. Implement the callbacks in `include/iwm_os2_adapter.h` from verified miniddk declarations. MultiMac and MultiMac1 are historical OS/2 pattern references only, not includes, link inputs or build prerequisites. See `docs/REFACTOR.md`.
5. Build the mixed 16/32-bit Drv32 driver with `wmake -f makefile.wat driver`; the supplied link file keeps the 16-bit strategy header and 32-bit core segments separate.

## First useful hardware milestone

1. Enumerate a supported PCI function and map BAR0.
2. Disable PCI retry timeout byte `0x41`, enable bus mastering, register legacy INTx first.
3. Read `CSR_HW_REV`, prepare the card and verify RF-kill state.
4. Allocate the RFH RX free/used/status rings, 4 KiB receive buffers, and firmware DMA bounce buffer. **Implemented for queue 0.**
5. Make `IWMFWLD.EXE` transfer a real firmware file and parse its TLVs. **Done.**
6. Load INIT sections through FH DMA. **Implemented; the supplied target log transfers all nine sections.**
7. Program RFH queue 0 before CPU release and validate the ALIVE command. **Implemented; target validation is required.** Then add the command queue, read NVM and send PHY configuration.
8. Stop INIT firmware, load runtime firmware, and validate runtime ALIVE.
9. Add scan commands/results, then association and WPA2 keys, then packet I/O.

See `docs/ROADMAP.md` for acceptance tests and `docs/REFERENCE_MAP.md` for exact source paths used.
The supplied XWLAN trace analysis and Resource Manager lifecycle are documented
in `docs/XWLAN_RESOURCE_MANAGER.md`.
The current WRND32$ trace, exact OID counts and timeout corrections are documented
in `docs/WRND32_LOG_ANALYSIS.md`.
The receive-queue diagnosis, register sequence and new target diagnostics are in
`docs/M3_RX_ALIVE.md`.



