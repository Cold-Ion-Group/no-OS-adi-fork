# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdac\system_top\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdac\system_top\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {system_top}\
-hw {C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdac\system_top.xsa}\
-out {C:/Users/fpga_/Desktop/adi/no-OS/projects/fmcdac}

platform write
domain create -name {standalone_sys_mb} -display-name {standalone_sys_mb} -os {standalone} -proc {sys_mb} -runtime {cpp} -arch {32-bit} -support-app {empty_application}
platform generate -domains 
platform active {system_top}
platform generate -quick
