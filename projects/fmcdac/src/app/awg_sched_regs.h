#ifndef AWG_SCHED_REGS_H
#define AWG_SCHED_REGS_H

/*
 * Firmware ABI for awg_timed_ctrl.
 *
 * Keep this header synchronized with projects/awg/common/awg_timed_ctrl.v.
 * Phase A freezes the register offsets below for both legacy fixed-length
 * mode and stream mode. Firmware should treat AWG_SCHED_REG_STREAM_DEPTH as
 * the authoritative stream FIFO capacity; do not hardcode the HDL parameter.
 */

#define AWG_SCHED_IP_ID                 0x41574753u
#define AWG_SCHED_IP_VERSION            0x00010000u

#define AWG_SCHED_REG_CTRL              0x00u
#define AWG_SCHED_REG_STATUS            0x04u
#define AWG_SCHED_REG_EVENT_COUNT       0x08u
#define AWG_SCHED_REG_CUR_EVENT         0x0Cu
#define AWG_SCHED_REG_ERR_REG           0x10u
#define AWG_SCHED_REG_IP_ID             0x14u
#define AWG_SCHED_REG_IP_VERSION        0x18u
#define AWG_SCHED_REG_IP_CAPS           0x1Cu
#define AWG_SCHED_REG_TIME_NOW_LO       0x20u
#define AWG_SCHED_REG_TIME_NOW_HI       0x24u
#define AWG_SCHED_REG_LAST_EXEC_LO      0x28u
#define AWG_SCHED_REG_LAST_EXEC_HI      0x2Cu
#define AWG_SCHED_REG_COMMIT_COUNT      0x30u
#define AWG_SCHED_REG_REINIT_COUNT      0x34u
#define AWG_SCHED_REG_REINIT_REJECT     0x38u
#define AWG_SCHED_REG_IRQ_STATUS        0x3Cu
#define AWG_SCHED_REG_EVT_WADDR         0x40u
#define AWG_SCHED_REG_EVT_WDATA0        0x44u
#define AWG_SCHED_REG_EVT_WDATA1        0x48u
#define AWG_SCHED_REG_EVT_WDATA2        0x4Cu
#define AWG_SCHED_REG_EVT_WDATA3        0x50u
#define AWG_SCHED_REG_EVT_WDATA4        0x54u
#define AWG_SCHED_REG_EVT_WDATA5        0x58u
#define AWG_SCHED_REG_EVT_WDATA6        0x5Cu
#define AWG_SCHED_REG_EVT_WCTRL         0x60u
#define AWG_SCHED_REG_IRQ_ENABLE        0x64u
#define AWG_SCHED_REG_IP_SCRATCH        0x68u
#define AWG_SCHED_REG_TIME_RELOAD_LO    0x6Cu
#define AWG_SCHED_REG_TIME_RELOAD_HI    0x70u
#define AWG_SCHED_REG_TIME_RELOAD_CTRL  0x74u
#define AWG_SCHED_REG_STREAM_CTRL       0x78u
#define AWG_SCHED_REG_OCCUPANCY         0x7Cu
#define AWG_SCHED_REG_FREE_SPACE        0x80u
#define AWG_SCHED_REG_LOW_WMARK         0x84u
#define AWG_SCHED_REG_STREAM_DEPTH      0x88u
#define AWG_SCHED_REG_STREAM_PUSHES     0x8Cu
#define AWG_SCHED_REG_STREAM_STALLS     0x90u

/* CTRL register bits */
#define AWG_SCHED_CTRL_RUN              (1u << 0)
#define AWG_SCHED_CTRL_ARM              (1u << 1)
#define AWG_SCHED_CTRL_STOP             (1u << 2)
#define AWG_SCHED_CTRL_RESET_SOFT       (1u << 3)
#define AWG_SCHED_CTRL_IRQ_ENABLE       (1u << 8)

/* TIME_RELOAD_CTRL register bits */
#define AWG_SCHED_TIME_RELOAD_ARM_ON_SYSREF (1u << 0)
#define AWG_SCHED_TIME_RELOAD_LOAD_NOW      (1u << 1)

/* STATUS low-byte bits */
#define AWG_SCHED_STATUS_IDLE           (1u << 0)
#define AWG_SCHED_STATUS_ARMED          (1u << 1)
#define AWG_SCHED_STATUS_RUNNING        (1u << 2)
#define AWG_SCHED_STATUS_DONE           (1u << 3)
#define AWG_SCHED_STATUS_ERROR          (1u << 4)

/* STATUS[15:8] / ERR_REG error codes */
#define AWG_SCHED_ERR_NONE              0x00u
#define AWG_SCHED_ERR_MISSED_DEADLINE   0x01u
#define AWG_SCHED_ERR_SPACING_VIOLATION 0x02u
#define AWG_SCHED_ERR_REINIT_SPACING    0x03u

/* IRQ_STATUS / IRQ_ENABLE bits. IRQ_STATUS is write-one-to-clear. */
#define AWG_SCHED_IRQ_DONE              (1u << 0)
#define AWG_SCHED_IRQ_ERROR             (1u << 1)
#define AWG_SCHED_IRQ_SPACING_VIOLATION (1u << 2)
#define AWG_SCHED_IRQ_UNDERRUN          (1u << 3)
#define AWG_SCHED_IRQ_LOW_WATERMARK     (1u << 4)
#define AWG_SCHED_IRQ_EMPTY_STALL       (1u << 5)
#define AWG_SCHED_IRQ_ALL               (AWG_SCHED_IRQ_DONE | \
                                         AWG_SCHED_IRQ_ERROR | \
                                         AWG_SCHED_IRQ_SPACING_VIOLATION | \
                                         AWG_SCHED_IRQ_UNDERRUN | \
                                         AWG_SCHED_IRQ_LOW_WATERMARK | \
                                         AWG_SCHED_IRQ_EMPTY_STALL)

/* STREAM_CTRL bits. WRITE_OVERFLOW is W1C; EOF_SEEN is read-only. */
#define AWG_SCHED_STREAM_CTRL_MODE      (1u << 0)
#define AWG_SCHED_STREAM_CTRL_OVERFLOW  (1u << 1)
#define AWG_SCHED_STREAM_CTRL_EOF_SEEN  (1u << 2)

/* EVT_WCTRL bits */
#define AWG_SCHED_EVT_WCTRL_PUSH        (1u << 0)

/* Event flags in EVT_WDATA2[15:0]. */
#define AWG_SCHED_EVENT_FLAG_PHASE_REINIT (1u << 0)
#define AWG_SCHED_EVENT_FLAG_EOF          (1u << 1)

/* Event packing for the 7-word MMIO write path. */
#define AWG_SCHED_EVENT_WORDS           7u
#define AWG_SCHED_EVENT_BYTES           32u

#endif
