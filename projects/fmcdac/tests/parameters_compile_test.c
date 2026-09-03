#include "parameters.h"

_Static_assert(AWG_SCHED_BASEADDR == 0x44AA0000U, "scheduler XPAR alias");
_Static_assert(AWG_SCHED_DMA_BASEADDR == 0x44AB0000U, "scheduler DMA alias");
_Static_assert(AWG_ETH_MAC_BASEADDR == 0x44C00000U, "MAC XPAR alias");
_Static_assert(AWG_ETH_RX_DMA_BASEADDR == 0x44AC0000U, "RX DMA alias");
_Static_assert(AWG_ETH_TX_DMA_BASEADDR == 0x44AD0000U, "TX DMA alias");
_Static_assert(AWG_EXTENSION_BASEADDR == 0x44AE0000U, "AWGX alias");
_Static_assert(AWG_ETH_DMA_BUFFER_BYTES == 9216U, "jumbo DMA buffer size");
_Static_assert(AWG_SCHED_IRQ_ID == 14U, "scheduler IRQ alias");
_Static_assert(AWG_SCHED_DMA_IRQ_ID == 12U, "scheduler DMA IRQ alias");
_Static_assert(AWG_ETH_RX_DMA_IRQ_ID == 10U, "RX DMA IRQ alias");
_Static_assert(AWG_ETH_TX_DMA_IRQ_ID == 9U, "TX DMA IRQ alias");
_Static_assert((AWG_STREAM_DDR_SIZE_BYTES % 32U) == 0U,
	       "ring contains whole events");
_Static_assert(AWG_STREAM_DDR_SIZE_BYTES >=
	       ((AWG_C1_MAX_COMMAND_RECORDS + 2U) * AWG_C1_COMMAND_BYTES),
	       "ring stages one complete maximum-size C1 program");
_Static_assert(FMCDAC_AWG_RELEASE_PROFILE == 1,
	       "release profile compile gate");
_Static_assert(FMCDAC_AWG_LEGACY_INGRESS_MAINTENANCE == 0,
	       "release profile defaults legacy ingress off");

int main(void)
{
	return 0;
}
