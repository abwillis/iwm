/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Target-only glue template. Build this file after pointing MMROOT and DDKBASE
 * at the local MultiMac and miniddk trees. The request-packet declarations,
 * DevHelp wrappers and NDIS entry points must come from those trees rather than
 * Linux/BSD compatibility headers.
 */
#if defined(__OS2__) && defined(IWM_USE_MULTIMAC_HEADERS)
#include "Dev16lib.h"
#include "base.h"
#include "ndis.h"
#include "driver.h"
#include "ioctl.h"
#include "iwm_os2.h"

extern struct iwm_adapter g_iwm_adapter;

static unsigned short iwm_ioctl_packet(PREQPACKET p)
{
    struct iwm_os2_ioctl request;
    fptr parm = 0, data = 0;
    int rc;

    if (p->ioctl.usParmLen &&
        DevHelp_PhysToVirt(p->ioctl.ulParm, p->ioctl.usParmLen, &parm, 0))
        return RPDONE | RPERR_PARAMETER;
    if (p->ioctl.usDataLen &&
        DevHelp_PhysToVirt(p->ioctl.ulData, p->ioctl.usDataLen, &data, 0))
        return RPDONE | RPERR_PARAMETER;

    memset(&request, 0, sizeof(request));
    request.category = p->ioctl.bCategory;
    request.function = p->ioctl.bFunction;
    if (parm && p->ioctl.usParmLen >= sizeof(request.parameters))
        _fmemcpy(request.parameters, parm, sizeof(request.parameters));
    request.data = data;
    request.data_length = p->ioctl.usDataLen;
    rc = iwm_os2_ioctl_dispatch(&g_iwm_adapter, &request);
    if (parm && p->ioctl.usParmLen >= sizeof(request.parameters))
        _fmemcpy(parm, request.parameters, sizeof(request.parameters));
    p->ioctl.usDataLen = (unsigned short)request.data_actual;
    return rc == IWM_OK ? RPDONE : (RPDONE | RPERR_GENERAL);
}

/* Merge these cases into common16/strat.c's StrategyHandler. */
unsigned short iwm_strategy_ioctl(PREQPACKET p)
{
    return iwm_ioctl_packet(p);
}

/* DriverCheckDevice, DriverInitAdapter, IRQHandler and packet methods remain
 * the intended MultiMac attachment points. Keep interrupt acknowledgement in
 * the ISR and schedule RX/TX/notification work outside hard-interrupt context.
 */
#endif

