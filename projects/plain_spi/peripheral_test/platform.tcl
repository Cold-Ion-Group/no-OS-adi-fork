# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\peripheral_test\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\peripheral_test\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {peripheral_test}\
-hw {C:\Users\fpga_\Desktop\interface_testing\basic_interfaces_wrapper.xsa}\
-proc {microblaze_0} -os {standalone} -out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
platform generate -domains 
platform active {peripheral_test}
platform active {peripheral_test}
domain create -name {standalone_microblaze_0} -display-name {standalone_microblaze_0} -os {standalone} -proc {microblaze_0} -runtime {cpp} -arch {32-bit} -support-app {peripheral_tests}
platform generate -domains 
platform write
domain active {standalone_domain}
domain active {standalone_microblaze_0}
platform generate -quick
platform generate
catch {platform remove peripheral_test}
platform write
