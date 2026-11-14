CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Werror
CPPFLAGS += -Iinclude
CORE = core/iwm_devices.c core/iwm_crc32.c core/iwm_fw_stream.c \
       core/iwm_hw.c core/iwm_rings.c core/iwm_genmac.c core/iwm_ndis.c
OS2HOST = os2/iwm_os2_ioctl.c

.PHONY: all test clean
all: build/test_core build/test_fw_fixes

build/test_core: $(CORE) tests/test_core.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CORE) tests/test_core.c

build/test_fw_fixes: $(CORE) $(OS2HOST) tests/test_fw_fixes.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(CORE) $(OS2HOST) tests/test_fw_fixes.c

test: build/test_core build/test_fw_fixes
	./build/test_core
	./build/test_fw_fixes

clean:
	rm -rf build
