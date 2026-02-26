# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\basic_mb\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\basic_mb\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {basic_mb}\
-hw {C:\Users\fpga_\Desktop\basic_design\mb_preset_wrapper.xsa}\
-proc {microblaze_0} -os {standalone} -out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
platform generate -domains 
platform active {basic_mb}
platform active {basic_mb}
domain create -name {standalone_microblaze_0} -display-name {standalone_microblaze_0} -os {standalone} -proc {microblaze_0} -runtime {cpp} -arch {32-bit} -support-app {hello_world}
platform generate -domains 
platform write
domain active {standalone_domain}
domain active {standalone_microblaze_0}
platform generate -quick
platform generate
