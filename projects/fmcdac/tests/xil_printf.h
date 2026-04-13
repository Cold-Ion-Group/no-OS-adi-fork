/*
 * xil_printf.h — stub for host-side test builds.
 * The real xil_printf is a Xilinx BSP function; in test builds we route
 * all output through the log_fn set on the awg_sched_cfg_t.
 */
#ifndef XIL_PRINTF_H
#define XIL_PRINTF_H

/* xil_printf is never called when log_fn is configured; provide a no-op
 * declaration so the translation unit compiles on host without the BSP. */
#include <stdio.h>
#define xil_printf printf

#endif /* XIL_PRINTF_H */
