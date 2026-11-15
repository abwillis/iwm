# makefile.wat -- Open Watcom wmake build for the OS/2 Intel Wi-Fi driver.
#   wmake -f makefile.wat            ring-3 loader + M1 probe driver
#   wmake -f makefile.wat probe      IWM32.SYS (M1 probe build)
#   wmake -f makefile.wat ring3      IWMFWLD.EXE
#   wmake -f makefile.wat paths      show resolved paths
# Commands are indented with spaces; wmake accepts any leading whitespace.

#----------------------------------------------------------------------
# Paths (override on the command line, e.g. DRV32=E:\dev\drv32)
#----------------------------------------------------------------------
!ifndef WATCOM
WATCOM = $(%WATCOM)
!endif
!ifndef ROOT
ROOT = E:\OS2TK45
!endif
!ifndef MINIDDK
MINIDDK = $(ROOT)\miniddk
!endif
!ifndef DDK
DDK = $(ROOT)\ddk
!endif
!ifndef DRV32
DRV32 = $(ROOT)\drv32
!endif
!ifndef DRV16
DRV16 = $(ROOT)\drv16
!endif
!ifndef OS2TK
OS2TK = $(ROOT)
!endif
!ifndef ACPITK
ACPITK = $(ROOT)\acpi
!endif

MINIDDK_H = $(MINIDDK)\base\h
DDK_H     = $(DDK)\base\h
DRV32_H   = $(DRV32)\kit
DRV32_LIB = $(DRV32)\kit
DRV16_H   = $(DRV16)\kit
OS2TK_H   = $(OS2TK)\h
OS2TK_LIB = $(OS2TK)\lib
ACPITK_H  = $(ACPITK)\h
WATCOM_H  = $(WATCOM)\h;$(WATCOM)\h\os2

DRV32LIB  = $(DRV32_LIB)\Drv32.lib

#----------------------------------------------------------------------
# Output
#----------------------------------------------------------------------
DRVNAME = IWM32
DRIVER  = $(DRVNAME).SYS
LOADER  = IWMFWLD.EXE

CC   = wcc386
LINK = wlink

#----------------------------------------------------------------------
# Include order: project, Drv32, miniddk, toolkit, ACPI, full DDK.
#----------------------------------------------------------------------
INC_R0 = -i=include -i=os2 -i=$(DRV32_H) -i=$(MINIDDK_H) -i=$(OS2TK_H) &
         -i=$(ACPITK_H) -i=$(DDK_H) -i=$(WATCOM_H)
INC_R3 = -i=include -i=$(OS2TK_H) -i=$(WATCOM_H)

#----------------------------------------------------------------------
# Flags. Ring 0: flat model, no default libs, no stack checks,
# SS != DS, intrinsics for memset/memcpy. Calling conventions come
# from the Drv32 headers (__syscall / #pragma aux), so no -ec* switch.
# Confirm against the Drv32 sample build before relying on them.
#----------------------------------------------------------------------
CF_COMMON = -q -bt=os2 -zq -wx -we -ei -6s -obilmr -d__OS2__
CF_R0     = $(CF_COMMON) -mf -s -ze -zfp -zgp -ms -wx -zp4 -za99 -zl -zls -dIWM_RING0 $(INC_R0)
CF_R3     = $(CF_COMMON) -mf -ox $(INC_R3)

!ifdef DEBUG
CF_R0 += -d2 -dIWM_DEBUG
CF_R3 += -d2
!endif

#----------------------------------------------------------------------
# Objects
#  PROBE_OBJS: everything the M1 driver needs, nothing more.
#  CORE_OBJS:  full portable core, linked from M2 onward.
#----------------------------------------------------------------------
PROBE_OBJS = r0_strat.obj r0_m1.obj r0_bar.obj

CORE_OBJS  = r0_devices.obj r0_crc32.obj r0_fw_stream.obj r0_fw_image.obj &
             r0_transport.obj r0_hw.obj r0_rings.obj r0_genmac.obj &
             r0_ndis.obj

R3_OBJS    = r3_iwmfwld.obj r3_crc32.obj

#----------------------------------------------------------------------
# Targets
#----------------------------------------------------------------------
all: .SYMBOLIC check ring3 probe

probe: .SYMBOLIC check $(DRIVER)

ring3: .SYMBOLIC $(LOADER)

check: .SYMBOLIC
!ifeq WATCOM
    @echo WATCOM is not set.
    @%abort
!endif
    @if not exist $(DRV32_H)\Dev32lib.h echo Missing $(DRV32_H)\Dev32lib.h
    @if not exist $(DRV32LIB) echo Missing $(DRV32LIB)
    @if not exist $(MINIDDK_H)\dhcalls.h echo Missing $(MINIDDK_H)\dhcalls.h
    @if not exist $(OS2TK_H)\os2.h echo Missing $(OS2TK_H)\os2.h

#----------------------------------------------------------------------
# Ring-0 compile rules (explicit, so ring-0 and ring-3 copies of the
# same source cannot collide)
#----------------------------------------------------------------------
r0_strat.obj: os2\iwm_d32_strat.c os2\iwm_d32.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_strat.c

r0_m1.obj: os2\iwm_d32_m1.c os2\iwm_d32.h include\iwm_bar.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_m1.c

r0_bar.obj: core\iwm_bar.c include\iwm_bar.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_bar.c

r0_devices.obj: core\iwm_devices.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_devices.c
r0_crc32.obj: core\iwm_crc32.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_crc32.c
r0_fw_stream.obj: core\iwm_fw_stream.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_fw_stream.c
r0_fw_image.obj: core\iwm_fw_image.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_fw_image.c
r0_transport.obj: core\iwm_transport.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_transport.c
r0_hw.obj: core\iwm_hw.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_hw.c
r0_rings.obj: core\iwm_rings.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_rings.c
r0_genmac.obj: core\iwm_genmac.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_genmac.c
r0_ndis.obj: core\iwm_ndis.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_ndis.c

core: .SYMBOLIC $(CORE_OBJS)

#----------------------------------------------------------------------
# Ring-3 compile rules
#----------------------------------------------------------------------
r3_iwmfwld.obj: tools\iwmfwld.c .AUTODEPEND
    $(CC) $(CF_R3) -fo=$@ tools\iwmfwld.c

r3_crc32.obj: core\iwm_crc32.c .AUTODEPEND
    $(CC) $(CF_R3) -fo=$@ core\iwm_crc32.c

#----------------------------------------------------------------------
# Link
#----------------------------------------------------------------------
$(LOADER): $(R3_OBJS)
    $(LINK) name $@ system os2v2 option quiet &
        file { $(R3_OBJS) } libpath $(OS2TK_LIB)

$(DRIVER): $(PROBE_OBJS) $(DRVNAME).lnk
    $(LINK) @$(DRVNAME).lnk

$(DRVNAME).lnk: makefile.wat
    @%create $@
    @%append $@ name $(DRIVER)
    @%append $@ format os2 lx physdevice
    @%append $@ option mixed1632,nostub,align=4
    @%append $@ option verbose,caseexact,eliminate sort global
    @%append $@ option stack=0
    @%append $@ option quiet, map=$(DRVNAME).map, internalrelocs
    @%append $@ option description 'Intel Wireless-AC 9560 OS/2 driver (M1 probe)'
!ifdef DEBUG
    @%append $@ debug all
!endif
    @for %f in ($(PROBE_OBJS)) do @%append $@ file %f
    @%append $@ library $(DRV32LIB)
    @%append $@ library $(MINIDDK)\base32\lib\kee.lib
    @%append $@ libpath $(OS2TK_LIB)

#----------------------------------------------------------------------
# Housekeeping
#----------------------------------------------------------------------
paths: .SYMBOLIC
    @echo WATCOM  = $(WATCOM)
    @echo MINIDDK = $(MINIDDK)
    @echo DDK     = $(DDK)
    @echo DRV32   = $(DRV32)
    @echo DRV16   = $(DRV16)
    @echo OS2TK   = $(OS2TK)
    @echo ACPITK  = $(ACPITK)

clean: .SYMBOLIC
    @if exist *.obj del *.obj
    @if exist *.err del *.err
    @if exist *.map del *.map
    @if exist $(DRVNAME).lnk del $(DRVNAME).lnk
    @if exist $(DRIVER) del $(DRIVER)
    @if exist $(LOADER) del $(LOADER)
