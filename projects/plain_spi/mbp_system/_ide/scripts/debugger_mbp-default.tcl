# Usage with Vitis IDE:
# In Vitis IDE create a Single Application Debug launch configuration,
# change the debug type to 'Attach to running target' and provide this 
# tcl script in 'Execute Script' option.
# Path of this script: C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\mbp_system\_ide\scripts\debugger_mbp-default.tcl
# 
# 
# Usage with xsct:
# To debug using xsct, launch xsct and run below command
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\mbp_system\_ide\scripts\debugger_mbp-default.tcl
# 
connect -url tcp:127.0.0.1:3121
targets -set -filter {jtag_cable_name =~ "Digilent JTAG-SMT2NC 210308B3AF60" && level==0 && jtag_device_ctx=="jsn-JTAG-SMT2NC-210308B3AF60-04a62093-0"}
fpga -file C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi/mbp/_ide/bitstream/mb_preset_wrapper.bit
targets -set -nocase -filter {name =~ "*microblaze*#0" && bscan=="USER2" }
loadhw -hw C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi/mb_preset_wrapper/export/mb_preset_wrapper/hw/mb_preset_wrapper.xsa -regs
configparams mdm-detect-bscan-mask 2
targets -set -nocase -filter {name =~ "*microblaze*#0" && bscan=="USER2" }
rst -system
after 3000
targets -set -nocase -filter {name =~ "*microblaze*#0" && bscan=="USER2" }
dow C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi/mbp/Debug/mbp.elf
bpadd -addr &main
