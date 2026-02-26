# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\emb_linux\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\plain_spi\emb_linux\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {emb_linux}\
-hw {C:\Users\fpga_\Desktop\basic_design\mb_preset_wrapper.xsa}\
-proc {microblaze_0} -os {linux} -out {C:/Users/fpga_/Desktop/adi/no-OS/projects/plain_spi}

platform write
platform active {emb_linux}
domain config -bif {}
domain config -boot {}
domain config -image {}
platform write
platform active {emb_linux}
