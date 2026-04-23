# SYSREF Edge Experiment Log

## Summary

This note is now **historical evidence**, not the current policy authority.

What still holds:

1. Rising edge must remain the AD9144 init default.
2. Starting with falling edge from init was observed to brick alignment in the
   2026-02-20 experiment below.

What no longer holds as a current-project statement:

1. "Tune should always skip on every boot."
2. "Changing edge after DATA state cannot produce a clean runtime state on the
   current build."

Current boot evidence on the default `1966 MSPS, 2x interpolation` build
captured in
[`boot_repeatability_20260404T163827Z`](./capture_runs/boot_repeatability_20260404T163827Z/)
shows a different runtime behavior:

1. AD9144 still starts with rising-edge SYSREF.
2. Every observed boot entered `fmcdac_sysref_tune()`.
3. The tune converged immediately to `falling edge + offset 0`.
4. The final FPGA `SYSREF_STATUS` was clean (`0x00000001`) on all 5 cycles.
5. Two latency signatures were observed (`0x03/0x03/0x0A/0x0A` and
   `0x04/0x04/0x0A/0x0A`), so deterministic latency is still open.

Use this file as background on the "falling-from-init is bad" result only.
For current runtime policy, use the current boot-repeatability artifacts and the
live comments in `fmcdac.c`.

## Historical Experiment (2026-02-20)

### Setup
- AD9144 at 983 MSPS, JESD204B Mode 4, Subclass 1
- SYSREF at 30.72 MHz from AD9516 OUT6/7
- FPGA AXI JESD TX, 4 GTH lanes at 9.83 Gbps

### Test 1: Init with falling edge (SYSREF_ACTRL0 = 0x00)

1. AD9144 driver writes `REG_SYSREF_ACTRL0 = 0x00` (falling edge) during init
2. Link comes up to DATA state — CGS, ILAS, Frame all OK (0x0F)
3. FPGA TX `SYSREF_STATUS` (0x108) reads `0x03` — **captured=1, alignment_error=1**
4. `fmcdac_sysref_tune()` runs:
   - Phase 1: toggle to rising edge, offset=0 → still error
   - Phase 2: sweep offsets 0–31 with falling edge → all fail
   - Phase 3: sweep offsets 0–31 with rising edge → all fail
   - **Result: FAILED — could not clear alignment error**
5. Symptom: alignment error is latched and cannot be cleared by any edge/offset
   combination once the initial LMFC alignment happened on the wrong edge

### Test 2: Init with rising edge (SYSREF_ACTRL0 = 0x04)

1. Power cycle board (required — soft reset insufficient)
2. AD9144 driver writes `REG_SYSREF_ACTRL0 = SYSREF_RISE` (0x04) during init
3. Link comes up to DATA state — all OK
4. FPGA TX `SYSREF_STATUS` reads `0x01` — **captured=1, alignment_error=0**
5. No tune needed — alignment clean from init

### Root Cause

The AD9144's LMFC counter aligns to the first SYSREF edge it sees after power-up.
If that edge is falling, the LMFC phase is offset by half a SYSREF period relative
to the FPGA TX LMFC. This phase offset is latched into the link during ILAS and
cannot be corrected post-link-up — changing the capture edge after DATA state only
affects future SYSREF detection, not the already-established LMFC phase relationship.

### Historical Resolution

`SYSREF_RISE` (0x04) is hardcoded in both `ad9144_setup()` and
`ad9144_setup_legacy()`.

That part is still correct.

The older expectation that the tune sweep should always report
"No alignment error - skipping tune" on every boot is no longer supported by
the current 2026-04-04 boot-repeatability data.
