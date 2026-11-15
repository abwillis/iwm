# 9560 refactor: boundaries and status

## Authorities
- OS/2 behavior: `Microsoft Copilot Chat Files/OS2Stuff`, starting with
  `COPILOT_OS2_START_HERE`; miniddk takes precedence over ddk.
- Intel hardware behavior: current Linux iwlwifi and BSD iwm sources routed
  by `COPILOT_OS2_IWM_LINUX_BSD_INDEX`.
- MultiMac and MultiMac1: historical OS/2 pattern references only. They are
  not includes, link inputs or build prerequisites. `MMROOT` is not used and
  `os2/iwm_multimac_glue.c` is removed.

## Layers
- `core/` (portable, host-tested, no OS/2/Linux/BSD/MultiMac headers):
  device table, firmware stream, firmware store + TLV catalog
  (`iwm_fw_image`), CSR helpers, interrupt-cause decode, 32-bit DMA and ring
  arithmetic (`iwm_transport`), GenMAC OIDs, NDIS-neutral hooks.
- `os2/iwm_os2_adapter.c`: the only target bridge. Attach order is PCI ->
  BAR0 -> firmware store -> IRQ, with reverse-order rollback and idempotent
  detach. Missing callbacks return `IWM_ENOTSUP`.
- Ring 3: XWLAN, wpa_supplicant and `IWMFWLD.EXE` are unchanged in role.

## Firmware path
BEGIN sizes the bound store; each ordered DATA record is copied into the
store before the CRC advances; COMMIT verifies the CRC, parses the TLV
container and requires at least one runtime section; `iwm_fw_start_load`
refuses to run without a store. No ring-3 pointer is retained. ABORT and
failures reset the store.

## 9560 configuration (current Linux cfg/9000.c, pcie/drv.c)
- 9DF0, A370: integrated, MQ-capable, 650 us crystal latency.
- 30DC: long-latency variant, 2820 us.
- 31DC: shared-clock variant, 670 us plus FW_PHY_CFG_SHARED_CLK.
- Family 9000: firmware API 30..46, NVM HW section 10, MAC CSR base 0x380.

## Still target/hardware-only (must not report success until done)
FH section DMA, RX status/free rings, command queue, interrupt enable before
CPU reset release, ALIVE parsing, NVM/MAC, PHY config, scan, station,
association, key commands, TX/RX data path, recovery.

## Target callbacks to implement from miniddk
`pci_attach/pci_detach`, `map_bar/unmap_bar`,
`bind_fw_store/release_fw_store` (fixed physical memory, GDT selector
windows, then `iwm_adapter_set_fw_store`), `install_irq/remove_irq`
(SetIRQ, EOI via DevHelp, context hook for deferred work).

