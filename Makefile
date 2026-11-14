# Recipes use '>' instead of TAB so the file survives copy/paste.
.RECIPEPREFIX := >
CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude -Itests
CORE = core/iwm_devices.c core/iwm_crc32.c core/iwm_fw_stream.c \
       core/iwm_fw_image.c core/iwm_transport.c \
       core/iwm_hw.c core/iwm_rings.c core/iwm_genmac.c core/iwm_ndis.c
OS2HOST = os2/iwm_os2_ioctl.c os2/iwm_os2_adapter.c
TESTS = test_core test_fw_fixes test_fw_image test_transport test_oid \
        test_os2_adapter

.PHONY: all test clean
all: $(TESTS:%=build/%)

build/%: tests/%.c $(CORE) $(OS2HOST)
> @mkdir -p build
> $(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CORE) $(OS2HOST) $<

test: all
> @for t in $(TESTS); do ./build/$$t || exit 1; done

clean:
> rm -rf build
