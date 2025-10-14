# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\interface_with_spi_1\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\interface_with_spi_1\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {interface_with_spi_1}\
-hw {C:\Users\fpga_\Desktop\interface_testing\interface_with_spi.xsa}\
-out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
domain create -name {standalone_microblaze_mcs_0_microblaze_I} -display-name {standalone_microblaze_mcs_0_microblaze_I} -os {standalone} -proc {microblaze_mcs_0_microblaze_I} -runtime {cpp} -arch {32-bit} -support-app {hello_world}
platform generate -domains 
platform active {interface_with_spi_1}
platform generate -quick
