# Open Watcom 2.0 / wmake build for the Drv32 OS/2 driver.
# Override paths on the command line, for example:
#   wmake -f makefile.wat OS2TK=D:\OS2TK45 WATCOM=E:\compilers\watcom

!ifndef WATCOM
WATCOM = $(%WATCOM)
!endif
!ifndef OS2TK
OS2TK = E:\OS2TK45
!endif
!ifndef MINIDDK
MINIDDK = $(OS2TK)\miniddk
!endif
!ifndef DDK
DDK = $(OS2TK)\ddk
!endif
!ifndef DRV32
DRV32 = $(OS2TK)\drv32\kit
!endif
!ifndef DRV16
DRV16 = $(OS2TK)\drv16
!endif
!ifndef ACPITK
ACPITK = $(OS2TK)\acpi
!endif

MINIDDK_H = $(MINIDDK)\base\h
DDK_H     = $(DDK)\base\h
DRV32_H   = $(DRV32)
OS2TK_H   = $(OS2TK)\h
OS2TK_LIB = $(OS2TK)\lib
ACPITK_H  = $(ACPITK)\h
WATCOM_H  = $(WATCOM)\h;$(WATCOM)\h\os2
DRV32LIB  = $(DRV32)\Drv32.lib
KEELIB    = $(MINIDDK)\base32\lib\kee.lib

CC = wcc386
LINK = wlink
DRIVER = IWM32.OS2
LOADER = IWMFWLD.EXE

INC_R0 = -i=include -i=os2 -i=$(DRV32_H) -i=$(MINIDDK_H) -i=$(OS2TK_H) &
         -i=$(ACPITK_H) -i=$(DDK_H) -i=$(WATCOM_H)
INC_R3 = -i=include -i=$(OS2TK_H) -i=$(WATCOM_H)

# Do not add -zu: Drv32 interfaces intentionally accept addresses of locals
# through near pointers on the 32-bit stack.
CF_R0 = -q -bt=os2 -zq -wx -6s -zp4 -d__OS2__ -d1 -6 -obilmr -s &
        -zp=1 -zl -ei -zff -zgf -dIWM_RING0 -fi=Dev32lib.h $(INC_R0)
CF_R3 = -q -bt=os2 -zq -wx -6s -zp4 -d__OS2__ -bm -obilmr -we -ei &
        -zm -ox $(INC_R3)

R0_OBJS = r0_strat.obj r0_m1.obj r0_rm.obj r0_bar.obj r0_fw.obj r0_ioctl.obj &
          r0_devices.obj r0_crc32.obj r0_fw_stream.obj r0_fw_image.obj &
          r0_fw_loader.obj r0_transport.obj r0_hw.obj r0_rings.obj &
          r0_genmac.obj
R3_OBJS = r3_iwmfwld.obj r3_crc32.obj

all: .SYMBOLIC check ring3 driver
ring3: .SYMBOLIC $(LOADER)
driver: .SYMBOLIC check $(DRIVER)

check: .SYMBOLIC
!ifeq WATCOM
    @echo WATCOM is not set.
    @%abort
!endif
    @if not exist $(DRV32_H)\Dev32lib.h echo Missing $(DRV32_H)\Dev32lib.h
    @if not exist $(DRV32LIB) echo Missing $(DRV32LIB)
    @if not exist $(MINIDDK_H)\strategy.h echo Missing $(MINIDDK_H)\strategy.h
    @if not exist $(KEELIB) echo Missing $(KEELIB)

r0_strat.obj: os2\iwm_d32_strat.c os2\iwm_d32.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_strat.c
r0_m1.obj: os2\iwm_d32_m1.c os2\iwm_d32.h include\iwm_bar.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_m1.c
r0_rm.obj: os2\iwm_d32_rm.c os2\iwm_d32.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_rm.c
r0_bar.obj: core\iwm_bar.c include\iwm_bar.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_bar.c
r0_fw.obj: os2\iwm_d32_fw.c os2\iwm_d32.h include\iwm_fw_loader.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_d32_fw.c
r0_ioctl.obj: os2\iwm_os2_ioctl.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ os2\iwm_os2_ioctl.c
r0_devices.obj: core\iwm_devices.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_devices.c
r0_crc32.obj: core\iwm_crc32.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_crc32.c
r0_fw_stream.obj: core\iwm_fw_stream.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_fw_stream.c
r0_fw_image.obj: core\iwm_fw_image.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_fw_image.c
r0_fw_loader.obj: core\iwm_fw_loader.c include\iwm_fw_loader.h .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_fw_loader.c
r0_transport.obj: core\iwm_transport.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_transport.c
r0_hw.obj: core\iwm_hw.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_hw.c
r0_rings.obj: core\iwm_rings.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_rings.c
r0_genmac.obj: core\iwm_genmac.c .AUTODEPEND
    $(CC) $(CF_R0) -fo=$@ core\iwm_genmac.c

r3_iwmfwld.obj: tools\iwmfwld.c .AUTODEPEND
    $(CC) $(CF_R3) -fo=$@ tools\iwmfwld.c
r3_crc32.obj: core\iwm_crc32.c .AUTODEPEND
    $(CC) $(CF_R3) -fo=$@ core\iwm_crc32.c

$(LOADER): $(R3_OBJS)
    $(LINK) name $@ system os2v2 option quiet file { $(R3_OBJS) } &
        libpath $(OS2TK_LIB)

$(DRIVER): $(R0_OBJS) IWM32.lnk
    $(LINK) @IWM32.lnk

IWM32.lnk: makefile.wat
    @%create $@
    @%append $@ name $(DRIVER)
    @%append $@ format os2 lx physdevice
    @%append $@ option mixed1632,nostub,align=4
    @%append $@ option verbose,caseexact,eliminate,nodefaultlibs
    @%append $@ option quiet,map=IWM32.map,internalrelocs
    @%append $@ option description 'Intel Wireless-AC 9560 OS/2 M2 loader'
    @%append $@ segment _TEXT PRELOAD CONFORMING IOPL DYNAMIC
    @%append $@ segment TYPE DATA SHARED
    @%append $@ file $(DRV32LIB)(header)
    @for %f in ($(R0_OBJS)) do @%append $@ file %f
    @%append $@ library $(DRV32LIB)
    @%append $@ library $(KEELIB)
    @%append $@ libpath $(OS2TK_LIB)

paths: .SYMBOLIC
    @echo WATCOM  = $(WATCOM)
    @echo OS2TK   = $(OS2TK)
    @echo MINIDDK = $(MINIDDK)
    @echo DDK     = $(DDK)
    @echo DRV32   = $(DRV32)
    @echo DRV16   = $(DRV16)
    @echo ACPITK  = $(ACPITK)

clean: .SYMBOLIC
    @if exist *.obj del *.obj
    @if exist *.err del *.err
    @if exist *.map del *.map
    @if exist $(DRIVER) del $(DRIVER)
    @if exist $(LOADER) del $(LOADER)

