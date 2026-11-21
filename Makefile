# Recipes use '>' instead of TAB so the file survives copy/paste.
.RECIPEPREFIX := >
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Itests
CORE = core/iwm_devices.c core/iwm_crc32.c core/iwm_fw_stream.c \
       core/iwm_fw_image.c core/iwm_fw_loader.c core/iwm_transport.c core/iwm_bar.c \
       core/iwm_hw.c core/iwm_rings.c core/iwm_rx.c core/iwm_tx.c \
       core/iwm_genmac.c core/iwm_ndis.c core/iwm_init_fw.c core/iwm_rt_fw.c core/iwm_rt_seq.c core/iwm_scan.c core/iwm_assoc.c core/iwm_sta.c core/iwm_sta_seq.c core/iwm_data.c
OS2HOST = os2/iwm_os2_ioctl.c os2/iwm_os2_adapter.c
TESTS = test_core test_fw_fixes test_fw_image test_transport test_oid \
         test_os2_adapter test_bar test_fw_loader test_rx test_tx test_init_fw test_rt_fw test_scan test_assoc test_sta test_data

.PHONY: all test clean
all: $(TESTS:%=build/%)

build/%: tests/%.c $(CORE) $(OS2HOST)
> @mkdir -p build
> $(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CORE) $(OS2HOST) $<

test: all
> @for t in $(TESTS); do ./build/$$t || exit 1; done

clean:
> rm -rf build


