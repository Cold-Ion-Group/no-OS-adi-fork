# Golden Reference Archive

Captured: 2026-02-14 (firmware commit `973d79cc2`)

## Contents

| File | Description |
|------|-------------|
| `uart_boot_log.txt` | Full UART output: boot → STPL → PRBS → DDS sweep |
| `register_dump.txt` | Key register values extracted from the boot log |
| `scope_capture.md` | Placeholder — drop in your scope screenshot |

## How to Compare

1. Boot the board and capture UART output via TeraTerm
2. Compare against `uart_boot_log.txt` — key fields to match:
   - `[JESD] Sanity:` line (all 0x0F)
   - `STPL PASS` for both DAC0 and DAC1 (8 lines)
   - `PRBS7 test PASSED` and `PRBS15 test PASSED`
   - DDS sweep from 10–200 MHz
3. Any deviation from golden indicates a config drift

## When to Update

Re-capture golden files whenever:
- `manifest.json` firmware commit changes
- `system_top.xsa` is rebuilt
- JESD parameters (lane rate, subclass, lane map) change

Run `gen_manifest.ps1 -Verify` to check if current state matches the manifest.
