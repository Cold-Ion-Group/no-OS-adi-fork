# 
# Usage: To re-create this platform project launch xsct with below options.
# xsct C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdaq2\kcu105\platform.tcl
# 
# OR launch xsct and run below command.
# source C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdaq2\kcu105\platform.tcl
# 
# To create the platform in a different location, modify the -out option of "platform create" command.
# -out option specifies the output directory of the platform project.

platform create -name {kcu105}\
-hw {C:\Users\fpga_\Desktop\adi\no-OS\projects\fmcdaq2\system_top.xsa}\
-out {C:/Users/fpga_/Desktop/adi/no-OS/projects/fmcdaq2}

platform write
domain create -name {standalone_sys_mb} -display-name {standalone_sys_mb} -os {standalone} -proc {sys_mb} -runtime {cpp} -arch {32-bit} -support-app {empty_application}
platform generate -domains 
platform active {kcu105}
platform generate -quick
bsp reload
platform active {kcu105}
catch {platform remove kcu105}
