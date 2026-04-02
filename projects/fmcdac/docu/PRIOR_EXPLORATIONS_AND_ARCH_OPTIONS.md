# Prior Explorations And Architectural Options

Date: 2026-04-02

This note captures earlier exploration threads that are still relevant context,
even though they are not all part of the current primary DDS/SFDR benchmark
flow.

## Why This Exists

The current summary set is centered on:

1. DDS-band validation
2. SFDR baseline capture
3. throughput baseline capture
4. UART RTT baseline capture

That leaves several earlier investigations only partially represented unless
they are called out explicitly. This note preserves those threads.

## 1. 500 MHz And Above

### What was explored

The original question was how to support or justify output at `500 MHz` and
beyond when the effective FPGA-side DDS Nyquist limit in the current mode is
`491.52 MHz`.

### Existing documentation

Primary detailed notes:

1. [AUTOMATION_AND_IMPLEMENTATION_STATUS.md](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
2. [MODE9_HDL_REQUIREMENTS.md](./MODE9_HDL_REQUIREMENTS.md)

### Main architectural options explored

#### Option A: Higher-rate architecture / JESD mode change

Documented in:

1. [AUTOMATION_AND_IMPLEMENTATION_STATUS.md](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)
2. [MODE9_HDL_REQUIREMENTS.md](./MODE9_HDL_REQUIREMENTS.md)

Exploration themes:

1. raise the effective FPGA-generated bandwidth
2. consider JESD mode changes and higher-rate operation
3. assess whether a mode-change path is justified relative to current evidence

Important current conclusion:

1. the latest FSH8 results do not support using the earlier `230-330 MHz`
   amplitude issue as the justification for Mode 9 work
2. Mode 9 remains an architectural option for genuine FPGA-generated content
   above first Nyquist, but it is not currently the primary corrective action

#### Option B: Image-mode / RF filtering path

Also documented in [MODE9_HDL_REQUIREMENTS.md](./MODE9_HDL_REQUIREMENTS.md).

Exploration themes:

1. use image-zone synthesis
2. select the desired image with external RF filtering

Important current conclusion:

1. this remains an RF/architecture option, not part of the current automated
   DDS-band or SFDR bench flow

## 2. Batched SYNC

### What was explored

The DDS update path used to incur too many synchronization events. This led to a
batched-SYNC exploration so multi-register DDS updates could be applied with a
single commit.

### Existing documentation

Primary references:

1. [AUTOMATION_AND_IMPLEMENTATION_STATUS.md](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)

### Code locations

Relevant implementation pieces:

1. [axi_dac_core.c](../../../drivers/axi_core/axi_dac_core/axi_dac_core.c)
2. [axi_dac_core.h](../../../drivers/axi_core/axi_dac_core/axi_dac_core.h)
3. [fmcdac.c](../src/app/fmcdac.c)

Current status:

1. this is implemented
2. it is still part of the current firmware behavior
3. it is not just historical context; it remains important for efficient DDS
   update sequencing

### What changed

Implemented themes:

1. shadow cache for DDS state
2. `sync_hold()` / `sync_commit()` flow
3. reduction of repeated SYNC pulses during:
   - initial DDS setup
   - sweep stepping
   - STPL setup
   - other grouped DDS writes

## 3. 32-bit DDS Phase Word

### What was explored

The DDS resolution was widened from a narrower control path to a full 32-bit
phase-word flow.

### Existing documentation

Primary reference:

1. [AUTOMATION_AND_IMPLEMENTATION_STATUS.md](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)

### Why it matters

This earlier exploration is directly relevant to current work because:

1. it affects FTW precision
2. it affects any future SFDR or dynamic-retune exploration
3. it is part of the current working firmware and not just a dead branch

## 4. CORDIC Mode With Memory

### What was explored

This topic is only lightly documented today.

Evidence in the current tree:

1. [fmcdac.c](../src/app/fmcdac.c) contains the comment:
   - `DDS tone test - validates data path for NCO/CORDIC development`
2. the system still contains a DMA playback path and AXI DAC source-mux support
3. there are references to arbitrary waveform / DMA playback in:
   - [SYSTEM_OVERVIEW.md](./SYSTEM_OVERVIEW.md)
   - [system_block_diagram.md](./system_block_diagram.md)

### What is currently documented

Only partially:

1. DMA playback path exists in the hardware
2. firmware can select DMA in principle
3. the current main tests do not exercise that path

### Current status

1. there is not yet a clean standalone note that fully explains the earlier
   "CORDIC mode with memory" exploration
2. the closest currently documented concept is the DMA/arbitrary-waveform path
3. if that exploration needs to be preserved more formally, it should become its
   own design note tied to:
   - source-mux modes
   - DMA playback
   - waveform-memory ownership
   - any intended CORDIC-generated or table-driven waveform modes

## 5. DMA Path

### What was explored

DMA waveform playback has been recognized as a future discriminator for whether
observed behavior is DDS-specific or downstream of DDS.

### Existing references

1. [fmcdac.c](../src/app/fmcdac.c)
2. [SYSTEM_OVERVIEW.md](./SYSTEM_OVERVIEW.md)
3. [system_block_diagram.md](./system_block_diagram.md)

### Current status

1. DMA path exists in hardware
2. it is not part of the current automated benchmark flow
3. it remains a valuable next-step experiment if DDS-band or SFDR data suggests
   a source-path-specific issue

## 6. EXT_SYNC Hooks

### What was explored

Whether the transport layer can support a cleaner external commit mechanism for
grouped DDS updates.

### Existing references

1. [AUTOMATION_AND_IMPLEMENTATION_STATUS.md](./AUTOMATION_AND_IMPLEMENTATION_STATUS.md)

### Current status

1. this remains a documented but not fully closed thread
2. batched SYNC already improved the practical situation
3. EXT_SYNC closure is still an architectural cleanup item, not a current blocker

## 7. Multi-Mode And Sample-Rate Variants

### What was explored

The firmware includes multiple rate selections such as:

1. `983 MSPS`
2. `1966 MSPS (2x interpolation)`
3. `500 MSPS`
4. `600 MSPS`

### Existing references

1. [fmcdac.c](../src/app/fmcdac.c)
2. [SYSTEM_OVERVIEW.md](./SYSTEM_OVERVIEW.md)

### Current status

1. the recent automated comparisons focused on `1x` vs `2x`
2. the broader multi-mode path is present in the codebase context but is not the
   current primary benchmark flow

## What Is Covered In The New Summary Set

Covered directly:

1. full DDS-band validation
2. SFDR baseline status
3. throughput baseline status
4. UART RTT baseline status
5. current tool/firmware implementation state

Only partially covered until this note:

1. 500 MHz path exploration
2. batched SYNC history
3. EXT_SYNC closure
4. DMA path as a future discriminator
5. CORDIC-with-memory context

## Current Recommendation

Treat these earlier threads as preserved architectural context, but keep the
main evaluation focus on:

1. DDS-band
2. SFDR
3. throughput
4. UART RTT

Only pull the older architectural threads back into the active critical path if
new evidence requires them.
