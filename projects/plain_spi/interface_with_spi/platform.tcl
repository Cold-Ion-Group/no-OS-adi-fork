# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\interface_with_spi\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\interface_with_spi\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {interface_with_spi}\
-hw {C:\Users\fpga_\Desktop\interface_testing\interface_with_spi.xsa}\
-out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
domain create -name {standalone_microblaze_0} -display-name {standalone_microblaze_0} -os {standalone} -proc {microblaze_0} -runtime {cpp} -arch {32-bit} -support-app {peripheral_tests}
platform generate -domains 
platform active {interface_with_spi}
platform generate -quick
platform generate
platform clean
platform generate
platform write
