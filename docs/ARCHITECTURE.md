# Architecture notes

## Layering

### OS/2 platform layer

Owns PCI enumeration, BAR mapping, physical memory, selectors, interrupt registration, timers, locking and NDIS calls. All declarations must come from `miniddk` first, with full `ddk` only as fallback. Linux/BSD memory and interrupt helpers must not cross this boundary.

### Intel transport layer

Owns CSR/FH/PRPH registers, NIC-access arbitration, reset, command and notification queues, firmware DMA and error recovery. Start with legacy INTx and one RX queue even though the 9560 advertises multi-queue RX; add MSI/MSI-X only after the OS/2 interrupt path is stable.

### Wi-Fi state layer

Owns scan, PHY/MAC contexts, station state, time events, rates, regulatory/NVM data and key commands. The BSD file split is a useful decomposition, but the calls into `net80211`, `bus_dma`, taskqueues and mutexes require native OS/2 replacements.

### NDIS/XWLAN layer

Presents an Ethernet-like NDIS MAC to TCP/IP and the GenMAC OID interface to XWLAN. 802.11 management remains in the driver/firmware; XWLAN and `wpa_supplicant` provide policy, credentials and EAPOL exchange.

## Firmware flow

`IWMFWLD.EXE` opens `\\DEV\\WRND32$`, queries protocol version and device ID, computes CRC32, sends `FW_BEGIN`, ordered `FW_DATA` records and `FW_COMMIT`. Each IOCTL payload is at most 16 KiB, below XWLAN's observed 32767-byte IOCTL ceiling.

The ring-0 implementation must evolve from the current validator to:

1. parse the Intel TLV header (`zero == 0`, magic `0x0a4c5749`);
2. retain metadata and stream section payloads into a physical bounce buffer;
3. reset/prepare the NIC and load init firmware sections;
4. wait for ALIVE, read NVM and calibration data;
5. load runtime sections, paging blocks where required, and wait for runtime ALIVE;
6. zero transient buffers and report a stable running state.

A complete image should not be placed in the 16-bit driver's data segment. If full-image retention proves necessary, allocate physical memory and map small selector windows on demand.

## Descriptor plan

- 256 TX entries per hardware queue; Intel TFD size is 128 bytes and the queue base is 256-byte aligned.
- 512 RX free descriptors for 9560/9260 multi-queue hardware; 256 for older legacy RX.
- 4096-byte RX buffers.
- 4096-byte ICT aligned to 4096 bytes.
- A separate command buffer array and scheduler byte-count table still need to be added.

## Concurrency

Use a short register/ring lock and a separate state lock. Never sleep while holding the register lock. The hard ISR records causes and wakes a deferred worker. Firmware waits use event words/ProcBlock-style waits with bounded timeouts. Shutdown first prevents new work, masks interrupts, drains the worker, stops bus-master DMA and then frees physical memory.

## Security boundary

The firmware loader accepts only ordered chunks after a valid BEGIN, rejects oversize images, verifies CRC32 and exposes explicit status. Production code should also validate TLV bounds, accepted section types, section count, firmware API compatibility and device-family compatibility before any DMA to the device.

