# OS/2 NDIS 2.0.1 data path

`os2/iwm_d32_ndis.c` is the 32-bit MAC side of the Drv32 NDIS support. Drv32.lib supplies the 16-bit FAR PASCAL entry stubs, the common/specific characteristics tables, PROTMAN registration, bind handling, and protocol upcall thunks.

The transmit path gathers an NDIS `TxBufDesc` into one Ethernet II frame and calls `iwm_d32_data_tx()`. Pointer type 0 is treated as physical; types 1 and 2 are accepted as GDT virtual pointers. A successful queue operation returns `SUCCESS`. Frames submitted while the station is not associated are deliberately consumed with `SUCCESS`, rather than making TCP/IP retry continuously during association.

The receive path uses synchronous ReceiveLookahead. The Ethernet frame produced by `iwm_data_rx()` is copied into the MAC lookahead buffer, indicated with `NdisRcvLkAhead()`, and followed by `NdisIndicationsComplete()`. `NdisDriverXferRx()` services any TransferData request made during that indication. No receive buffer survives the indication, so ReceiveRelease returns `INVALID_FUNCTION`.

## Declaration sources

The load-bearing NDIS 2.0.1 layouts are `CommChar`/`COMMONCHAR`, `MACSpecChar`/`SERVICECHAR`, `MACSpecStat`/`SERVICESTAT`, `MACUprDisp`/`UPPERDISP`, `ProtLwrDisp`/`PROTLWRDISP`, `TxBufDesc`/`TXBUFDESC`, `TDBufDesc`/`TDBUFDESC`, and `RxBufDesc`/`RXBUFDESC`.

The supplied OneDrive MiniDDK tree was enumerated through `miniddk/base/h`, `miniddk/base/inc`, `miniddk/base32/h`, and the remaining MiniDDK subfolders. No `ndis.h` was present in that tree at patch time. The definitions were therefore checked against the fallback `OS2stuff/ddk/DDK/ndis.h` and consumed through `OS2stuff/Drv32/Kit/Dev32ndis.h`, whose far-pointer annotations and 32-bit callback declarations are required by Drv32. The DDK and Drv32 declarations agree on all fields used here. MiniDDK remains first in the compiler include path; if a later MiniDDK copy adds `ndis.h`, it does not replace `Dev32ndis.h`, because the latter is the Drv32 ABI contract rather than a duplicate platform header.

## Installation

1. Build with Open Watcom 2.0 and the same Drv32 kit used by the current driver: `wmake -f Makefile.wat driver`.
2. Copy `IWM32.OS2` and `install/IWM32.NIF` to `C:\IBMCOM\MACS`.
3. Merge `install/PROTOCOL.INI.example` into `C:\IBMCOM\PROTOCOL.INI`. Preserve existing protocol bindings and append `IWM32_NIF` to the existing `[TCPIP_NIF] Bindings` list.
4. Add `DEVICE=C:\IBMCOM\MACS\IWM32.OS2` to `CONFIG.SYS`, after the normal MPTS/PROTMAN support drivers. Only one driver that owns `WRND32$` may be active.
5. Reboot, associate through the existing GenMAC/XWLAN path, and obtain an address with the normal TCP/IP DHCP client.

The sample files are merge fragments, not replacements for a working MPTS configuration.
