# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\baare_metal_ex\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\baare_metal_ex\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {baare_metal_ex}\
-hw {C:\Users\fpga_\Desktop\baremetal_basic_peri\mb_preset_wrapper.xsa}\
-proc {microblaze_0} -os {standalone} -out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
platform generate -domains 
platform active {baare_metal_ex}
platform generate
