# Host-side unit tests for awg_sched validation layer.
#
# Usage:
#   make -C projects/fmcdac/tests
#   make -C projects/fmcdac/tests run
#   make -C projects/fmcdac/tests run-host
#   make -C projects/fmcdac/tests clean

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic \
           -include awg_sched_test_parameters.h \
           -I. \
           -I../src/app \
           -I../../../include \
           -I../../../drivers/axi_core/axi_dmac \
           -DAWG_SCHED_ENABLE_LOAD_READBACK_VERIFY=0 \
           -DFMCDAC_AWG_SCHED_USE_IRQ=1 \
           -DFMCDAC_AWG_SCHED_STREAM=1

SRC_DIR := ../src/app
TEST_BIN := awg_sched_validate_test
PROTO_TEST_BIN := awg_stream_proto_test
DMA_TEST_BIN := awg_sched_dma_test
PARAM_TEST_BIN := parameters_compile_test
NET_TEST_BIN := awg_net_test
ETH_IO_TEST_BIN := awg_eth_io_test

SRCS := awg_sched_validate_test.c \
        $(SRC_DIR)/awg_sched.c \
        $(SRC_DIR)/awg_stream_ring.c

PROTO_SRCS := awg_stream_proto_test.c \
              $(SRC_DIR)/awg_stream_proto.c

DMA_SRCS := awg_sched_dma_test.c \
            $(SRC_DIR)/awg_sched_dma.c \
            $(SRC_DIR)/awg_stream_ring.c

PARAM_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror \
                -I. -I$(SRC_DIR) \
                -DFMCDAC_AWG_SCHED=1 \
                -DFMCDAC_AWG_SCHED_STREAM=1 \
                -DFMCDAC_AWG_SCHED_DMA_REFILL=1 \
                -DFMCDAC_AWG_SCHED_ETH=1 \
                -DFMCDAC_AWG_SCHED_USE_IRQ=1 \
                -DFMCDAC_AWG_SCHED_BASEADDR=0x44AA0000U \
                -DFMCDAC_AWG_SCHED_DMA_BASEADDR=0x44AB0000U \
                -DFMCDAC_AWG_ETH_MAC_BASEADDR=0x44C00000U \
                -DFMCDAC_AWG_ETH_RX_DMA_BASEADDR=0x44AC0000U \
                -DFMCDAC_AWG_ETH_TX_DMA_BASEADDR=0x44AD0000U

NET_SRCS := awg_net_test.c $(SRC_DIR)/awg_net.c

ETH_IO_SRCS := awg_eth_io_test.c \
               $(SRC_DIR)/awg_eth_mac.c \
               $(SRC_DIR)/awg_eth_rx.c \
               $(SRC_DIR)/awg_eth_tx.c

all: $(TEST_BIN) $(PROTO_TEST_BIN) $(DMA_TEST_BIN) $(PARAM_TEST_BIN) $(NET_TEST_BIN) $(ETH_IO_TEST_BIN)

$(TEST_BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

$(PROTO_TEST_BIN): $(PROTO_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

$(DMA_TEST_BIN): $(DMA_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

$(PARAM_TEST_BIN): parameters_compile_test.c xparameters.h
	$(CC) $(PARAM_CFLAGS) -o $@ parameters_compile_test.c

$(NET_TEST_BIN): $(NET_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

$(ETH_IO_TEST_BIN): $(ETH_IO_SRCS)
	$(CC) $(CFLAGS) -Werror -o $@ $^

run: $(TEST_BIN) $(PROTO_TEST_BIN) $(DMA_TEST_BIN) $(PARAM_TEST_BIN) $(NET_TEST_BIN) $(ETH_IO_TEST_BIN)
	./$(TEST_BIN)
	./$(PROTO_TEST_BIN)
	./$(DMA_TEST_BIN)
	./$(PARAM_TEST_BIN)
	./$(NET_TEST_BIN)
	./$(ETH_IO_TEST_BIN)

run-host:
	python awg_sched_host_test.py

clean:
	rm -f $(TEST_BIN) $(PROTO_TEST_BIN) $(DMA_TEST_BIN) $(PARAM_TEST_BIN) $(NET_TEST_BIN) $(ETH_IO_TEST_BIN)

.PHONY: all run run-host clean
