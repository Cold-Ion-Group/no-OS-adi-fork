# SYSREF Edge Experiment Log

## Summary

Rising edge **must** be the AD9144 init default. Falling edge from init permanently
bricks SYSREF alignment — the tune sweep cannot recover without a full power cycle.

## Experiment (2026-02-20)

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

### Resolution

`SYSREF_RISE` (0x04) is hardcoded in both `ad9144_setup()` and
`ad9144_setup_legacy()`. The tune sweep remains as a safety net but should
report "No alignment error - skipping tune" on every boot.
