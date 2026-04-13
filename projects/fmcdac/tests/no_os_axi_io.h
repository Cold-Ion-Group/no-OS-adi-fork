/*
 * no_os_axi_io.h — stub for host-side test builds.
 */
#ifndef NO_OS_AXI_IO_H
#define NO_OS_AXI_IO_H

#include <stdint.h>

int no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t val);
int no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *val);

#endif /* NO_OS_AXI_IO_H */
