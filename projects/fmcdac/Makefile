TINYIIOD ?= n
HARDWARE := system_top.xsa
include ../../tools/scripts/generic_variables.mk

# On Windows, default to copying sources into the generated Xilinx app tree.
# The generic build defaults to LINK_SRCS=y, which uses mklink and fails on
# shells without symlink privilege, leaving build/app sources missing.
LINK_SRCS := n

include src.mk

include ../../tools/scripts/generic.mk

# --- S0 reproducibility targets ---

# Verify manifest before building (opt out with SKIP_MANIFEST=1)
ifneq ($(SKIP_MANIFEST),1)
  ifneq ($(wildcard manifest.json),)
.PHONY: manifest-verify
manifest-verify:
	@echo "[MANIFEST] Verifying build inputs..."
	@powershell -ExecutionPolicy Bypass -File gen_manifest.ps1 -Verify
build: manifest-verify
  endif
endif

# Generate / refresh the manifest
.PHONY: manifest-gen
manifest-gen:
	@powershell -ExecutionPolicy Bypass -File gen_manifest.ps1

# Verify a captured UART log against golden
.PHONY: verify-uart
verify-uart:
	@powershell -ExecutionPolicy Bypass -File verify_uart.ps1 \
		-LogFile $(or $(UART_LOG),captured_uart.txt) \
		-GoldenFile golden/uart_boot_log.txt
