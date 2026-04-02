# FMCDAC System Block Diagram

```mermaid
block-beta
  columns 4

  block:EXT["External Clock"]
    columns 1
    SRC["122.88 MHz Source"]
    AD9516["AD9516-1 Clock Distributor\nOUT1: DAC CLK 122.88 MHz\nOUT6: DAC SYSREF 30.72 MHz\nOUT7: FPGA SYSREF 30.72 MHz\nOUT9: FPGA REFCLK 122.88 MHz"]
    SRC --> AD9516
  end

  block:FPGA["KCU116 FPGA (XCKU5P)"]
    columns 2

    MB["MicroBlaze\nno-OS bare-metal\nfmcdac.c"]
    SPI["AXI SPI\n0x44A70000\nCS0=AD9516\nCS1=AD9144"]

    AXI["AXI Peripherals\nAXI GPIO 0x40000000\nAXI IIC 0x41600000"]
    DDR["DDR Controller\n0x80000000"]

    DMAC["axi_dmac\nMM-to-AXIS\n0x7C420000"]
    FIFO["util_dacfifo\n(inferred from HDL)"]

    UPACK["util_upack2\n(inferred from HDL)"]
    TPL["ad_ip_jesd204_tpl_dac\n0x44A04000\nSource mux: DDS SED\nDMA PN7 PN15 ZERO\n2 ch, DPW=4, 16-bit"]

    JESD["axi_jesd204_tx\n0x44A90000\nSC1, K=32, F=1\nSYSREF capture internal"]
    XCVR["axi_adxcvr + util_adxcvr\n0x44A60000\nGTH4 TX, QPLL0\n4 lanes, 9.83 Gbps"]

    MB --> SPI
    MB --> AXI
    AXI --> DMAC
    AXI --> TPL
    AXI --> JESD
    AXI --> XCVR
    DDR --> DMAC
    DMAC --> FIFO
    FIFO --> UPACK
    UPACK --> TPL
    TPL --> JESD
    JESD --> XCVR
  end

  DAC["AD9144\nJESD204B Receiver\nDual 16-bit DAC\nXBAR: {4,5,6,7}→{0,1,2,3}\nLane 2 polarity invert"]
  RF["Analog Output\nDAC0 (I) + DAC1 (Q)"]

  SPI --> AD9516
  SPI --> DAC
  AD9516 --> XCVR
  AD9516 --> JESD
  AD9516 --> DAC
  XCVR --> DAC
  DAC --> RF
```

## GPIO Pin Mapping

```
AXI GPIO (0x40000000) bit assignments:
  Bit 40  — DAC_RESET      (active-low reset to AD9144)
  Bit 41  — DAC_TXEN        (DAC transmit enable)
  Bit 38  — CLKD_SYNC       (AD9516 SYNC pulse)
  Bit 22  — dac_ctrl[1]     (DAC analog output control)
  Bit 21  — dac_ctrl[0]     (DAC analog output control)
```

## Clock and SYSREF Routing

```mermaid
flowchart LR
  SRC["122.88 MHz\nExternal Ref"] --> AD9516

  AD9516 -->|"OUT1\n122.88 MHz\nLVPECL"| DAC_CLK["AD9144\nPLL Ref → 983 MHz"]
  AD9516 -->|"OUT6\n30.72 MHz\nLVDS"| DAC_SYSREF["AD9144\nSYSREF"]
  AD9516 -->|"OUT7\n30.72 MHz\nLVDS"| FPGA_SYSREF["FPGA\nSYSREF\n(→ axi_jesd204_tx)"]
  AD9516 -->|"OUT9\n122.88 MHz\nLVDS bypass"| FPGA_REFCLK["FPGA\nGTH QPLL0\nRefclk"]
```

## Data Path Detail

```mermaid
flowchart LR
  subgraph TPL["ad_ip_jesd204_tpl_dac (0x44A04000)"]
    MUX{"Source\nMux"}
    DDS["DDS NCO\n16-bit FTW\n16-bit POW\n16-bit ASF"]
    SED["SED Pattern\n32-bit per ch"]
    PN["PN7 / PN15\nPRBS Gen"]
    DMA_IN["DMA Input\n(from upack2)"]
    ZERO["Zero Source"]

    DDS --> MUX
    SED --> MUX
    PN --> MUX
    DMA_IN --> MUX
    ZERO --> MUX
  end

  MUX --> JESD["axi_jesd204_tx\nLink Layer\n8B/10B + Scramble"]
  JESD --> GTH["GTH4 TX\n4 lanes\n9.83 Gbps"]
  GTH --> AD9144["AD9144\nJESD RX\nXBAR → DAC0/DAC1"]
```

## Notes

- **GTH4** (not GTY): XCKU5P on KCU116 has GTH transceivers
- **util_dacfifo** and **util_upack2** are standard ADI HDL IPs inferred from the reference design; firmware does not interact with them directly
- **SYSREF capture** is handled inside `axi_jesd204_tx` (registers `0x100`–`0x108`), not a separate IP
- **DMA playback** path (DDR → DMAC → FIFO → UPACK → TPL) exists in hardware but is **not currently exercised** by firmware — only DDS, SED, and PN paths are tested
- **Si5328** (via AXI IIC at `0x41600000`) is present in hardware but **bypassed** (`SKIP_SI5328`) — GTH REFCLK comes directly from AD9516 OUT9
