# Reference map

Repository root: `/Microsoft Copilot Chat Files/OS2stuff`

## Routing and precedence

- `COPILOT_OS2_START_HERE.md` — establishes miniddk-first precedence, OS/2 driver routing and project-source status.
- `COPILOT_OS2_IWM_LINUX_BSD_START_HERE.md` — establishes the Intel hardware-reference workflow and BSD/Linux lineage.
- `COPILOT_OS2_IWM_LINUX_BSD_INDEX.csv` — machine-readable routing for the 73 Intel reference files.

## Intel hardware/firmware references

- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm.c` — end-to-end flow, PCI IDs, DMA allocation patterns, interrupt path and attach sequence.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_config.h` — device-family and configuration contract.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_7000.c` — 3160/3165/3168/7260/7265 aliases and family flags.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_8000.c` — 8260/8265 aliases and family flags.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_9000.c` — 9560 configuration.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_9260.c` — 9260 configuration.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwmreg.h` — registers, firmware TLVs and descriptor formats.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwmvar.h` — ring counts and transport state structures.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_pcie_trans.c` — ownership, reset, interrupt and PCIe transport sequencing.
- `iwm-reference/freebsd-src/sys/dev/iwm/if_iwm_fw.c` — firmware paging and DMA behavior.

These sources describe hardware behavior. Their BSD/Linux OS services are not copied into the OS/2 layer.

## OS/2 NDIS and memory patterns

- `multimac/common16/driver.c` — physical ring allocation, selector mapping, Resource Manager registration, PCI enumeration and NDIS registration.
- `multimac/common16/strat.c` — strategy dispatch and category `0x99` GenMAC IOCTL routing.
- `multimac/common16/ioctl.h` — GenMAC private OIDs and functions.
- `multimac/iwl/iwl-core.c` — older Intel Wi-Fi flow guidance.
- `multimac/iwl/iwl_hw.cpp` — confirms that the repository's IWL MultiMac hardware methods are mostly placeholders and cannot serve as a complete driver.

## XWLAN and WPA contract

- `xwlan/xwlan_repo_bundle.txt`, virtual file `src/lib/dim/genmac/genmac.c` — exact OID GET/SET parameter convention, scan path, SSID/security operations and capability expectations.
- `xwlan/xwlan_repo_bundle.txt`, virtual files `src/lib/dim/genmac/wrapper/ioctl.hpp`, `poid.hpp` and `ndis/ntndis.h` — category/function/OID values.
- `xwlan/xwlan_repo_bundle.txt`, virtual files under `src/lib/wpacli` — XWLAN control of `wpa_supplicant`.

## Ring-3 firmware pattern

- `rtl/README.md` — `WRND32$`, XWLAN/NDIS dual exposure and per-boot firmware loading.
- `rtl/rtl8821au/install/STARTUP.CMD.example` — ring-3 copy of firmware to the driver before XWLAN use.
- `rtl/rtl8821au/src/rtl8821_hal.c` — physical firmware buffer and selector-window pattern for an image that does not fit the 16-bit data segment.
- `rtl/rtl8821au/src/rt_strat.c` — OS/2 strategy/open/read/write/IOCTL lifecycle.

## Licensing rule

No Linux/BSD/RTL/MultiMac source file is vendored in this starter. New code is GPL-2.0-only so later integration with the GPL MultiMac and RTL implementation material remains possible. Preserve original notices for any imported code and review firmware redistribution separately.

