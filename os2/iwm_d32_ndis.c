/* SPDX-License-Identifier: GPL-2.0-only */
/* OS/2 NDIS 2.0.1 MAC front-end for the Drv32 Intel 9560 driver.
 *
 * Drv32.lib owns the 16-bit FAR PASCAL entry stubs, characteristics tables,
 * PROTMAN registration and bind plumbing.  This file supplies the documented
 * 32-bit driver callbacks and connects them to the hardware-independent IWM
 * Ethernet data path.
 */
#include "Dev32lib.h"
#include "Dev32ndis.h"
#include "iwm_ndis.h"
#include "iwm_d32.h"

#define IWM_NDIS_FRAME_MAX 1514U
#define IWM_NDIS_TX_DEPTH  64U

extern char cDevName[];
extern struct CommChar AdapterCC;
extern struct MACSpecChar AdapterSC;
extern struct MACSpecStat AdapterSS;
extern struct MCastBuf AdapterMCB;

static struct iwm_adapter *g_adapter;
static SpinLock_t g_tx_lock;
static int g_lock_ready;
static int g_registered;
static UCHAR g_tx_frame[IWM_NDIS_FRAME_MAX];
static UCHAR g_rx_frame[IWM_NDIS_FRAME_MAX];
static USHORT g_rx_len;
static UCHAR g_rx_active;
static char g_adapter_descr[] = "Intel Wireless-AC 9560 WLAN (OS/2 native)";

/* d17 diagnostics: the log showed only 1-3 data frames sent in hours and no
 * received data, without saying whether the protocol ever opened the MAC,
 * transmitted through it or was offered a frame.  Counted here, logged by
 * iwm_d32_ndis_log(). */
static ULONG g_nd_open;
static ULONG g_nd_xmit;
static ULONG g_nd_xmit_ok;
static ULONG g_nd_xmit_unassoc;
static ULONG g_nd_xmit_err;
static int g_nd_xmit_rc;
static USHORT g_nd_xmit_type;
static USHORT g_nd_xmit_len;
static UCHAR g_nd_xmit_dst[6];
static ULONG g_nd_ind;
static ULONG g_nd_ind_busy;
static ULONG g_nd_ind_rc;
/* d24 log: "ind 2 ... fail 2" counted every non-SUCCESS return of
 * NdisRcvLkAhead.  FRAME_NOT_RECOGNIZED (3) and FRAME_REJECTED (4) are the
 * protocol declining a frame, not a MAC fault; keep them apart and keep the
 * last code. */
static ULONG g_nd_ind_ok;
static ULONG g_nd_ind_declined;
static USHORT g_nd_ind_lastrc;
static USHORT g_nd_ind_type;
static char g_message_file[] = "IWM32.MSG";

static void copy_bytes(void *dst, const void *src, USHORT len)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;

    while (len--)
        *d++ = *s++;
}

static int name_equal(const char *a, const char *b)
{
    UCHAR ca;
    UCHAR cb;

    if (!a || !b)
        return 0;
    do {
        ca = (UCHAR)*a++;
        cb = (UCHAR)*b++;
        if (ca >= 'a' && ca <= 'z')
            ca = (UCHAR)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z')
            cb = (UCHAR)(cb - ('a' - 'A'));
        if (ca != cb)
            return 0;
    } while (ca != 0);
    return 1;
}

static int is_broadcast(const UCHAR *p)
{
    unsigned i;

    for (i = 0; i < 6U; ++i)
        if (p[i] != 0xffU)
            return 0;
    return 1;
}

static void set_mac_tables(void)
{
    unsigned i;

    if (!g_adapter)
        return;
    AdapterSC.MscStnAdrSz = 6;
    for (i = 0; i < 6U; ++i) {
        AdapterSC.MscPermStnAdr[i] = g_adapter->mac[i];
        AdapterSC.MscCurrStnAdr[i] = g_adapter->mac[i];
    }
    AdapterSC.MscInterrupt = (USHORT)g_adapter->irq;
    AdapterSC.MscVenAdaptDesc = g_adapter_descr;
    AdapterSC.MscLinkSpd = 54000000UL;
    AdapterSC.MscService = BROADCAST_SUPP | MULTICAST_SUPP |
                           STAT_ALWAYS_CURR | GDT_ADDRESS_SUPP;
    AdapterSC.MscMaxFrame = IWM_NDIS_FRAME_MAX;
    AdapterSC.MscTBufCap = IWM_NDIS_FRAME_MAX * IWM_NDIS_TX_DEPTH;
    AdapterSC.MscTBlkSz = 1;
    AdapterSC.MscRBufCap = IWM_NDIS_FRAME_MAX;
    AdapterSC.MscRBlkSz = 1;
    AdapterSC.MscTxQDepth = IWM_NDIS_TX_DEPTH;
    AdapterSC.MscMaxDataBlocks = MAX_TX_DATABLK;
    AdapterMCB.McbMax = NUM_MCADDRS;
    AdapterSS.MssStatus |= HW_OK;
}

/* Return a flat address for an NDIS data block.  The MiniDDK NDIS 2.0.1
 * declaration defines type 0 as physical and type 1 as GDT virtual.  Some
 * later IBM/MultiMac code uses 2 for a virtual block, so both 1 and 2 are
 * accepted here. */
static void *data_pointer(UCHAR type, UCHAR FAR16DATA *pointer, USHORT len)
{
    void *linear;
    ULONG physical;

    if (!pointer || !len)
        return 0;
    if (type == 1U || type == 2U)
        return Far16ToFlat((void FAR16DATA *)pointer);
    if (type != 0U)
        return 0;
    physical = CastFar16ToULONG((void FAR16DATA *)pointer);
    if (Dev32Help_PhysToLin(physical, len, &linear))
        return 0;
    return linear;
}

static int append_bytes(UCHAR *frame, USHORT *used, const void *src, USHORT len)
{
    if (!src || (ULONG)*used + len > IWM_NDIS_FRAME_MAX)
        return 0;
    copy_bytes(frame + *used, src, len);
    *used = (USHORT)(*used + len);
    return 1;
}

static int register_mac(void *context, struct iwm_adapter *adapter)
{
    int rc;

    (void)context;
    if (g_registered)
        return IWM_OK;
    if (!adapter)
        return IWM_EINVAL;
    g_adapter = adapter;
    if (!g_lock_ready) {
        KernAllocSpinLock(&g_tx_lock);
        g_lock_ready = 1;
    }

    rc = NdisInit(g_message_file);
    if (rc < 0) {
        dprintf(0, "iwm: NDIS PROTMAN initialization failed %d\n", rc);
        return IWM_ENODEV;
    }
    if (rc != 0) {
        dprintf(0, "iwm: NDIS PROTOCOL.INI rejected %d\n", rc);
        return IWM_EINVAL;
    }
    set_mac_tables();
    rc = NdisRegisterDriver();
    if (rc != 0) {
        dprintf(0, "iwm: NDIS MAC registration failed %d\n", rc);
        return IWM_EIO;
    }
    g_registered = 1;
    dprintf(0, "iwm: NDIS MAC registered as %.16s\n", AdapterCC.CcName);
    return IWM_OK;
}

static int indicate_receive(void *context, const void *frame, uint16_t length)
{
    UCHAR indicate;
    ULONG rc;

    (void)context;
    if (!g_registered || !frame || length < ETH_HLEN ||
        length > IWM_NDIS_FRAME_MAX)
        return IWM_EINVAL;
    if (NdisGetInfo(NDISINFO_Indications) || g_rx_active) {
        ++AdapterSS.MssRFLack;
        ++g_nd_ind_busy;
        return IWM_EBUSY;
    }
    ++g_nd_ind;
    g_nd_ind_type = (USHORT)((((const UCHAR *)frame)[12] << 8) |
                             ((const UCHAR *)frame)[13]);

    g_rx_active = 1;
    g_rx_len = length;
    copy_bytes(g_rx_frame, frame, length);
    ++AdapterSS.MssFR;
    AdapterSS.MssFRByt += length;
    if (g_rx_frame[0] & 1U) {
        if (is_broadcast(g_rx_frame)) {
            ++AdapterSS.MssFRBC;
            AdapterSS.MssFRBCByt += length;
        } else {
            ++AdapterSS.MssFRMC;
            AdapterSS.MssFRMCByt += length;
        }
    }

    indicate = 0xffU;
    NdisIndicationsOff();
    rc = NdisRcvLkAhead(&indicate, g_rx_frame, length, length);
    if (indicate)
        NdisIndicationsOn();
    NdisIndicationsComplete();
    g_nd_ind_lastrc = (USHORT)rc;
    if (rc == SUCCESS)
        ++g_nd_ind_ok;
    else if (rc == FRAME_NOT_RECOGNIZED || rc == FRAME_REJECTED)
        ++g_nd_ind_declined;
    else
        ++g_nd_ind_rc;
    g_rx_active = 0;
    g_rx_len = 0;
    return rc == SUCCESS ? IWM_OK : IWM_EIO;
}

void iwm_d32_ndis_log_rx(const char *why)
{
    dprintf(0,
        "iwm: ndis rx %s ind %lu ok %lu declined %lu busy %lu fail %lu lastrc %04x type %04x\n",
        why, g_nd_ind, g_nd_ind_ok, g_nd_ind_declined, g_nd_ind_busy,
        g_nd_ind_rc, (unsigned)g_nd_ind_lastrc, (unsigned)g_nd_ind_type);
}

static void transmit_complete(void *context, void *packet, int status)
{
    (void)context;
    (void)packet;
    (void)status;
}

static void status_change(void *context, uint32_t generation)
{
    (void)context;
    (void)generation;
    if (g_adapter)
        AdapterSS.MssStatus |= HW_OK;
}

static const struct iwm_ndis_callbacks g_callbacks = {
    register_mac, indicate_receive, transmit_complete, status_change
};

void iwm_d32_ndis_bind(struct iwm_ndis_context *ndis,
                       struct iwm_adapter *adapter)
{
    if (!ndis)
        return;
    ndis->callbacks = &g_callbacks;
    ndis->callback_context = 0;
    ndis->adapter = adapter;
    g_adapter = adapter;
}

int __syscall NdisDriverInitAdapter(void)
{
    set_mac_tables();
    return g_adapter ? 0 : 1;
}

int __syscall NdisDriverStartIrq(void)
{
    /* iwm_d32_m1() has already allocated MSI and installed the Drv32 ISR. */
    return g_adapter ? 0 : 1;
}

int __syscall NdisDriverOpen(void)
{
    ++g_nd_open;
    AdapterSS.MssStatus |= HW_OK | MS_OPEN;
    return g_adapter ? 0 : 1;
}

int __syscall NdisDriverProcessParms(struct ModCfg *config)
{
    struct KeywordEntry *entry;
    char *driver_name;
    unsigned i;

    if (!config)
        return -4;
    entry = FindKey(config, "DRIVERNAME");
    driver_name = entry ? GetConfigString(entry) : 0;
    if (!name_equal(driver_name, cDevName))
        return -4;
    for (i = 0; i < NAME_LEN; ++i)
        AdapterCC.CcName[i] = config->ModName[i];
    return 0;
}

void __syscall NdisDriverSetMcast(ULONG flags)
{
    (void)flags;
    /* The associated station receives directed, group and broadcast frames;
     * the NDIS packet filter is maintained by Drv32.lib. */
}

void __syscall NdisDriverGetHwStats(void)
{
    /* Counters are updated synchronously in the transmit/receive callbacks. */
}

void __syscall NdisDriverSetMac(UCHAR *mac, ULONG length)
{
    unsigned i;

    if (!g_adapter || !mac || length != 6UL || (mac[0] & 1U))
        return;
    for (i = 0; i < 6U; ++i) {
        g_adapter->mac[i] = mac[i];
        AdapterSC.MscCurrStnAdr[i] = mac[i];
    }
}

int __syscall NdisDriverStartXmit(struct TxBufDesc *descriptor,
                                  ULONG protocol_request)
{
    USHORT used;
    USHORT i;
    void *source;
    int rc;

    (void)protocol_request;
    if (!descriptor || descriptor->TxDataCount > MAX_TX_DATABLK ||
        descriptor->TxImmedLen > MAX_IMMED_LEN)
        return INVALID_PARAMETER;

    KernAcquireSpinLock(&g_tx_lock);
    used = 0;
    if (descriptor->TxImmedLen) {
        source = Far16ToFlat((void FAR16DATA *)descriptor->TxImmedPtr);
        if (!append_bytes(g_tx_frame, &used, source,
                          descriptor->TxImmedLen)) {
            KernReleaseSpinLock(&g_tx_lock);
            return INVALID_PARAMETER;
        }
    }
    for (i = 0; i < descriptor->TxDataCount; ++i) {
        if (!descriptor->TxDataBlk[i].TxDataLen)
            continue;
        source = data_pointer(descriptor->TxDataBlk[i].TxPtrType,
                              descriptor->TxDataBlk[i].TxDataPtr,
                              descriptor->TxDataBlk[i].TxDataLen);
        if (!append_bytes(g_tx_frame, &used, source,
                          descriptor->TxDataBlk[i].TxDataLen)) {
            KernReleaseSpinLock(&g_tx_lock);
            return INVALID_PARAMETER;
        }
    }
    if (used < ETH_HLEN) {
        KernReleaseSpinLock(&g_tx_lock);
        return INVALID_PARAMETER;
    }

    rc = iwm_d32_data_tx(g_tx_frame, used);
    ++g_nd_xmit;
    g_nd_xmit_rc = rc;
    g_nd_xmit_len = used;
    g_nd_xmit_type = (USHORT)((g_tx_frame[12] << 8) | g_tx_frame[13]);
    copy_bytes(g_nd_xmit_dst, g_tx_frame, 6);
    if (rc == IWM_OK)
        ++g_nd_xmit_ok;
    else if (rc == IWM_ESTATE)
        ++g_nd_xmit_unassoc;
    else
        ++g_nd_xmit_err;
    if (rc == IWM_OK) {
        ++AdapterSS.MssFS;
        AdapterSS.MssFSByt += used;
        if (g_tx_frame[0] & 1U) {
            if (is_broadcast(g_tx_frame)) {
                ++AdapterSS.MssFSBC;
                AdapterSS.MssFSBCByt += used;
            } else {
                ++AdapterSS.MssFSMC;
                AdapterSS.MssFSMCByt += used;
            }
        }
    } else {
        ++AdapterSS.MssSFHW;
    }
    KernReleaseSpinLock(&g_tx_lock);

    if (rc == IWM_OK || rc == IWM_ESTATE)
        return SUCCESS;
    if (rc == IWM_EBUSY || rc == IWM_ENOMEM)
        return OUT_OF_RESOURCE;
    if (rc == IWM_EINVAL || rc == IWM_EOVERFLOW)
        return INVALID_PARAMETER;
    return TRANSMIT_ERROR;
}

int __syscall NdisDriverXferRx(USHORT *copied, USHORT frame_offset,
                               struct TDBufDesc *descriptor)
{
    USHORT available;
    USHORT total;
    USHORT i;
    USHORT amount;
    void *destination;

    if (copied)
        *copied = 0;
    if (!copied || !descriptor || !g_rx_active || frame_offset > g_rx_len ||
        descriptor->TDDataCount > MAX_TD_DATABLK)
        return INVALID_PARAMETER;

    available = (USHORT)(g_rx_len - frame_offset);
    total = 0;
    for (i = 0; i < descriptor->TDDataCount && available; ++i) {
        amount = descriptor->TDDataBlk[i].TDDataLen;
        if (!amount)
            continue;
        if (amount > available)
            amount = available;
        destination = data_pointer(descriptor->TDDataBlk[i].TDPtrType,
                                   descriptor->TDDataBlk[i].TDDataPtr,
                                   amount);
        if (!destination)
            return INVALID_PARAMETER;
        copy_bytes(destination, g_rx_frame + frame_offset + total, amount);
        total = (USHORT)(total + amount);
        available = (USHORT)(available - amount);
    }
    *copied = total;
    return SUCCESS;
}

int __syscall NdisDriverReleaseRx(USHORT receive_handle)
{
    (void)receive_handle;
    /* ReceiveLookahead is synchronous; no receive buffer is retained. */
    return INVALID_FUNCTION;
}

void iwm_d32_ndis_log(const char *why)
{
    dprintf(0,
        "iwm: ndis %s reg %d open %lu xmit %lu ok %lu unassoc %lu err %lu rc %d last %02x:%02x:%02x:%02x:%02x:%02x type %04x len %u ind %lu busy %lu fail %lu type %04x\n",
        why, g_registered, g_nd_open, g_nd_xmit, g_nd_xmit_ok,
        g_nd_xmit_unassoc, g_nd_xmit_err, g_nd_xmit_rc, g_nd_xmit_dst[0],
        g_nd_xmit_dst[1], g_nd_xmit_dst[2], g_nd_xmit_dst[3], g_nd_xmit_dst[4],
        g_nd_xmit_dst[5], (unsigned)g_nd_xmit_type, (unsigned)g_nd_xmit_len,
        g_nd_ind, g_nd_ind_busy, g_nd_ind_rc, (unsigned)g_nd_ind_type);
}
