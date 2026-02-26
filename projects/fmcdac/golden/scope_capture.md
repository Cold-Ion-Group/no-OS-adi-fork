# Scope Capture Placeholder

This file is a placeholder for the golden scope capture.

## Expected Capture

- **Instrument**: Spectrum analyzer or oscilloscope
- **Signal**: DAC analog output (DAC0 or DAC1)
- **Tone**: 10 MHz DDS, scale=0x3333, DAC gain=0x01FF
- **DAC rate**: 983.04 MSPS
- **Expected**: Clean single tone at 10 MHz, no spurs above -50 dBc

## How to Add

1. Capture a screenshot or CSV export from your scope/spectrum analyzer
2. Save as `scope_10mhz_tone.png` (or `.csv`) in this directory
3. Update this file with the actual capture details (date, instrument, settings)

## Capture Checklist

- [ ] Frequency marker on 10 MHz peak
- [ ] Visible noise floor (at least 50 MHz span)
- [ ] RBW and span noted in filename or caption
- [ ] Captured with DAC gain = 0x01FF (full scale)
