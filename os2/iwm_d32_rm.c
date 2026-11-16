/* os2/iwm_d32_rm.c -- OS/2 Resource Manager registration for IWM32.
 *
 * The structures and values follow MiniDDK rmbase.h.  Drv32's Rm1 helpers
 * adapt those 16-bit Resource Manager contracts to this 32-bit ring-0 code
 * and retain the allocated handles for cleanup. */
#include "Dev32lib.h"
#include "Dev32rmbase.h"
#include "iwm_types.h"
#include "iwm_d32.h"

static char g_rm_name[] = "IWM32.OS2";
static char g_rm_description[] = "Intel Wireless-AC 9560 Wi-Fi adapter";
static char g_rm_vendor[] = "Intel Corporation";
static char g_rm_adapter_name[] = "Intel Wireless-AC 9560 [IWM32]";

static DRIVERSTRUCT g_rm_driver;
static ADJUNCT g_rm_pci_adjunct;
static ADAPTERSTRUCT g_rm_adapter;
static int g_rm_driver_created;
static int g_rm_adapter_created;

int iwm_d32_rm_create_driver(void)
{
    int rc;

    if (g_rm_driver_created)
        return IWM_OK;

    memset(&g_rm_driver, 0, sizeof(g_rm_driver));
    g_rm_driver.DrvrName = FlatToFar16(g_rm_name);
    g_rm_driver.DrvrDescript = FlatToFar16(g_rm_description);
    g_rm_driver.VendorName = FlatToFar16(g_rm_vendor);
    g_rm_driver.MajorVer = 0;
    g_rm_driver.MinorVer = 3;
    g_rm_driver.Date.Year = 2026;
    g_rm_driver.Date.Month = 11;
    g_rm_driver.Date.Day = 15;
    g_rm_driver.DrvrFlags = DRF_STATIC;
    g_rm_driver.DrvrType = DRT_NETWORK;
    g_rm_driver.DrvrSubType = 0;
    g_rm_driver.DrvrCallback = FAR16NULL;

    rc = Rm1CreateDriver(&g_rm_driver);
    if (rc) {
        dprintf(0, "iwm: RM create driver failed rc %d\n", rc);
        return IWM_EIO;
    }
    g_rm_driver_created = 1;
    dprintf(0, "iwm: RM driver registered\n");
    return IWM_OK;
}

int iwm_d32_rm_create_adapter(unsigned long bar0, unsigned long bar_size,
                              unsigned short irq, unsigned short pin,
                              unsigned short bdf)
{
    HDRIVER driver_handle;
    HADAPTER adapter_handle;
    int rc;

    if (!g_rm_driver_created)
        return IWM_ESTATE;
    if (g_rm_adapter_created)
        return IWM_EBUSY;

    rc = Rm1AddMem((ULONG)bar0, (ULONG)bar_size);
    if (rc) {
        dprintf(0, "iwm: RM BAR0 claim failed rc %d\n", rc);
        return IWM_EBUSY;
    }
    rc = Rm1AddIrq((USHORT)irq, (USHORT)pin, RS_IRQ_EXCLUSIVE);
    if (rc) {
        dprintf(0, "iwm: RM MSI claim failed irq %u rc %d\n", irq, rc);
        Rm1Destroy(0);
        return IWM_EBUSY;
    }

    memset(&g_rm_pci_adjunct, 0, sizeof(g_rm_pci_adjunct));
    g_rm_pci_adjunct.pNextAdj = FAR16NULL;
    g_rm_pci_adjunct.AdjLength = sizeof(g_rm_pci_adjunct);
    g_rm_pci_adjunct.AdjType = ADJ_PCI_DEVFUNC;
    g_rm_pci_adjunct.PCI_DevFunc = bdf;

    memset(&g_rm_adapter, 0, sizeof(g_rm_adapter));
    g_rm_adapter.AdaptDescriptName = FlatToFar16(g_rm_adapter_name);
    g_rm_adapter.AdaptFlags = AS_NO16MB_ADDRESS_LIMIT;
    g_rm_adapter.BaseType = AS_BASE_NETWORK;
    g_rm_adapter.SubType = AS_SUB_OTHER;
    g_rm_adapter.InterfaceType = AS_INTF_GENERIC;
    g_rm_adapter.HostBusType = AS_HOSTBUS_PCI;
    g_rm_adapter.HostBusWidth = AS_BUSWIDTH_32BIT;
    g_rm_adapter.pAdjunctList = FlatToFar16(&g_rm_pci_adjunct);

    rc = Rm1CreateAdapter(&g_rm_adapter);
    if (rc) {
        dprintf(0, "iwm: RM create adapter failed rc %d\n", rc);
        Rm1Destroy(0);
        return IWM_EIO;
    }
    g_rm_adapter_created = 1;
    driver_handle = 0;
    adapter_handle = 0;
    rc = Rm1GetHandles(&driver_handle, &adapter_handle);
    dprintf(0,
        "iwm: RM adapter registered bdf %x BAR0 %08lx/%lu MSI %u handles %08lx:%08lx rc %d\n",
        bdf, bar0, bar_size, irq, (ULONG)driver_handle,
        (ULONG)adapter_handle, rc);
    return IWM_OK;
}

void iwm_d32_rm_destroy(int destroy_driver)
{
    if (!g_rm_driver_created)
        return;
    Rm1Destroy(destroy_driver ? 1 : 0);
    g_rm_adapter_created = 0;
    if (destroy_driver)
        g_rm_driver_created = 0;
}

