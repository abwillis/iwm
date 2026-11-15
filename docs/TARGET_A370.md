# Target adapter: Intel 8086:a370 (Wireless-AC 9560, CNVi)

Captured under Linux (lspci), subsystem 8086:0030, PCI revision 10:

    00:14.3 Network controller [0280]: Intel 300/C240 Series CNVi Wi-Fi
    Memory at 604b10c000 (64-bit, non-prefetchable) [size=16K]
    MSI: Enable+ Count=1/1 64bit+    MSI-X: Enable- Count=16
    Capabilities: PM at c8, MSI at d0, PCIe at 40, MSI-X at 80, LTR at 14c

## Consequences for the OS/2 driver

1. BAR0 is a 64-bit address above 4 GiB. The miniddk DevHelp services take
   a 32-bit physical address (PhysToGDTSelector, PhysToVirt, VMAlloc with
   VMDHA_PHYS). Decode both BAR dwords with iwm_bar_decode(); it returns
   IWM_ERANGE for this placement. Never pass the low dword alone.
2. "IRQ 164" is a Linux MSI vector number, not a legacy IRQ line. The
   miniddk DevHelp set has SetIRQ/UnSetIRQ/EOI only. Read the PCI
   Interrupt Line/Pin registers under OS/2 before relying on INTx.
3. The device table is keyed on device ID only. Linux selects the 9560
   configuration by device and subsystem ID (here 0x0030); confirm the
   entry in the indexed pcie/drv.c before applying per-variant values.
4. The PM capability at 0xC8 means D0 must be confirmed before CSR access.

