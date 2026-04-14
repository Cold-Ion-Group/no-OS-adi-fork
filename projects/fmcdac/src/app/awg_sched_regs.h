/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AWG Timed-Control IP register map.
 *
 * This header is the software-side counterpart of the HDL localparams.
 * It MUST be kept in sync with the HDL peripheral register file.
 * When a register-generator toolflow is adopted, delete this hand-written
 * file and replace it with the generated artifact (same filename).
 *
 * All offsets are byte addresses relative to the peripheral base address.
 * All registers are 32-bit, little-endian at the CPU AXI4-Lite interface.
 *
 * Register map layout:
 *   0x00–0x10  Core control/status
 *   0x14–0x1C  IP identification (read-only)
 *   0x20–0x2C  Time snapshot (read-only)
 *   0x30–0x3C  Counters / IRQ (read-only)
 *   0x40–0x60  Event write window (write-only during load)
 */

#ifndef AWG_SCHED_REGS_H
#define AWG_SCHED_REGS_H

/* -----------------------------------------------------------------------
 * Core control / status
 * --------------------------------------------------------------------- */

/** CTRL — control register (W) */
#define AWG_SCHED_REG_CTRL              0x0000U
/** STATUS — status register (R) */
#define AWG_SCHED_REG_STATUS            0x0004U
/** EVT_COUNT — number of loaded events register (RW) */
#define AWG_SCHED_REG_EVT_COUNT         0x0008U
/** CUR_EVT — index of event currently being executed (R) */
#define AWG_SCHED_REG_CUR_EVT           0x000CU
/** ERR_REG — latched hardware error code (R, cleared by soft-reset) */
#define AWG_SCHED_REG_ERR               0x0010U

/* -----------------------------------------------------------------------
 * IP identification (read-only)
 * --------------------------------------------------------------------- */

/** IP_ID — magic identifier word for this IP core (R) */
#define AWG_SCHED_REG_IP_ID             0x0014U
/** IP_VERSION — packed major[31:16] / minor[15:0] (R) */
#define AWG_SCHED_REG_IP_VERSION        0x0018U
/**
 * IP_CAPS — capability register (R):
 *   [31:24]  log2 of maximum event depth (1 << field gives depth)
 *   [23:16]  payload width in bits
 *   [15:8]   timestamp width in bits
 *   [7:0]    reserved
 */
#define AWG_SCHED_REG_IP_CAPS           0x001CU

/* -----------------------------------------------------------------------
 * Time snapshot (read-only, latched on STATUS read)
 * --------------------------------------------------------------------- */

/** TIME_LO — current scheduler tick counter, bits [31:0] (R) */
#define AWG_SCHED_REG_TIME_LO           0x0020U
/** TIME_HI — current scheduler tick counter, bits [63:32] (R) */
#define AWG_SCHED_REG_TIME_HI           0x0024U
/** LAST_EXEC_LO — tick count of last dispatched event, bits [31:0] (R) */
#define AWG_SCHED_REG_LAST_EXEC_LO      0x0028U
/** LAST_EXEC_HI — tick count of last dispatched event, bits [63:32] (R) */
#define AWG_SCHED_REG_LAST_EXEC_HI      0x002CU

/* -----------------------------------------------------------------------
 * Counters / IRQ status (read-only)
 * --------------------------------------------------------------------- */

/** COMMIT_COUNT — total number of events dispatched since last reset (R) */
#define AWG_SCHED_REG_COMMIT_COUNT      0x0030U
/** REINIT_COUNT — number of sequence re-starts (e.g. loop) since reset (R) */
#define AWG_SCHED_REG_REINIT_COUNT      0x0034U
/** REINIT_REJECT — count of re-init requests rejected (timing violation) (R) */
#define AWG_SCHED_REG_REINIT_REJECT     0x0038U
/** IRQ_ENABLE — interrupt enable mask (RW) */
#define AWG_SCHED_REG_IRQ_ENABLE        0x0074U
/**
 * IRQ_STATUS — latched interrupt flags (R/W1C):
 *   [0]  done interrupt
 *   [1]  error interrupt
 *   [2]  spacing-violation interrupt
 */
#define AWG_SCHED_REG_IRQ_STATUS        0x003CU

/* -----------------------------------------------------------------------
 * Event write window
 *
 * Write sequence:
 *   1. Write EVT_WADDR = target event index.
 *   2. Write EVT_WDATA0..6 (order does not matter before strobe).
 *   3. Write EVT_WCTRL = 1 to commit (strobe).
 *
 * Event word mapping:
 *   WDATA0  timestamp[31:0]
 *   WDATA1  timestamp[63:32]
 *   WDATA2  channel[31:16] | flags[15:0]
 *   WDATA3  payload.word0
 *   WDATA4  payload.word1
 *   WDATA5  payload.word2
 *   WDATA6  payload.word3
 * --------------------------------------------------------------------- */

/** EVT_WADDR — event BRAM write address (W) */
#define AWG_SCHED_REG_EVT_WADDR         0x0040U
/** EVT_WDATA0 — timestamp bits [31:0] (W) */
#define AWG_SCHED_REG_EVT_WDATA0        0x0044U
/** EVT_WDATA1 — timestamp bits [63:32] (W) */
#define AWG_SCHED_REG_EVT_WDATA1        0x0048U
/** EVT_WDATA2 — channel[31:16] | flags[15:0] (W) */
#define AWG_SCHED_REG_EVT_WDATA2        0x004CU
/** EVT_WDATA3 — payload word 0 (W) */
#define AWG_SCHED_REG_EVT_WDATA3        0x0050U
/** EVT_WDATA4 — payload word 1 (W) */
#define AWG_SCHED_REG_EVT_WDATA4        0x0054U
/** EVT_WDATA5 — payload word 2 (W) */
#define AWG_SCHED_REG_EVT_WDATA5        0x0058U
/** EVT_WDATA6 — payload word 3 (W) */
#define AWG_SCHED_REG_EVT_WDATA6        0x005CU
/** EVT_WCTRL — write 1 to commit event data to BRAM (W, self-clearing) */
#define AWG_SCHED_REG_EVT_WCTRL         0x0060U

/** TIME_RELOAD_LO — next SYSREF epoch reload low word (W) */
#define AWG_SCHED_REG_TIME_RELOAD_LO    0x0078U
/** TIME_RELOAD_HI — next SYSREF epoch reload high word (W) */
#define AWG_SCHED_REG_TIME_RELOAD_HI    0x007CU
/** TIME_RELOAD_CTRL — write 1 to reload TIME_NOW on next SYSREF (W) */
#define AWG_SCHED_REG_TIME_RELOAD_CTRL  0x0080U

/* -----------------------------------------------------------------------
 * CTRL register bit fields
 * --------------------------------------------------------------------- */

/** CTRL[0]: RUN — assert to start sequence execution */
#define AWG_SCHED_CTRL_RUN              (1U << 0)
/** CTRL[1]: ARM — arm the hardware trigger gate */
#define AWG_SCHED_CTRL_ARM              (1U << 1)
/** CTRL[2]: STOP_REQ — request graceful stop after current event */
#define AWG_SCHED_CTRL_STOP_REQ         (1U << 2)
/** CTRL[3]: RESET_SOFT — pulse to clear state (does NOT fire commit) */
#define AWG_SCHED_CTRL_RESET_SOFT       (1U << 3)

/* -----------------------------------------------------------------------
 * STATUS register bit fields
 * --------------------------------------------------------------------- */

/** STATUS[0]: armed — hardware trigger gate is armed */
#define AWG_SCHED_STATUS_ARMED          (1U << 0)
/** STATUS[1]: running — sequence is currently executing */
#define AWG_SCHED_STATUS_RUNNING        (1U << 1)
/** STATUS[2]: done — sequence has completed */
#define AWG_SCHED_STATUS_DONE           (1U << 2)
/** STATUS[3]: error — hardware error latched; see ERR_REG */
#define AWG_SCHED_STATUS_ERROR          (1U << 3)
/** STATUS[15:8]: err_code — latched error code (same as ERR_REG[7:0]) */
#define AWG_SCHED_STATUS_ERR_CODE_SHIFT 8U
#define AWG_SCHED_STATUS_ERR_CODE_MASK  0xFFU

/* -----------------------------------------------------------------------
 * IP identification constants
 * --------------------------------------------------------------------- */

/** Expected value of IP_ID register ('A','W','G','S' in ASCII) */
#define AWG_TIMED_CTRL_IP_ID            0x41574753U

/** Expected major version (IP_VERSION[31:16]) */
#define AWG_TIMED_CTRL_MAJOR_EXPECTED   1U

/* -----------------------------------------------------------------------
 * IP_CAPS field extraction macros
 * --------------------------------------------------------------------- */

/** Extract log2(event_depth) from IP_CAPS; depth = 1 << this value */
#define AWG_SCHED_CAPS_EVT_DEPTH_LOG2(caps) \
	(((caps) >> 24) & 0xFFU)

/** Extract payload width in bits from IP_CAPS */
#define AWG_SCHED_CAPS_PAYLOAD_BITS(caps) \
	(((caps) >> 16) & 0xFFU)

/** Extract timestamp width in bits from IP_CAPS */
#define AWG_SCHED_CAPS_TS_BITS(caps) \
	(((caps) >> 8) & 0xFFU)

#endif /* AWG_SCHED_REGS_H */
