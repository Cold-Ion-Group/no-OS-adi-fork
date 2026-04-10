#ifndef AWG_SCHED_H
#define AWG_SCHED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload format version 1, packed as four 32-bit words. */
typedef struct {
	uint32_t word0;
	uint32_t word1;
	uint32_t word2;
	uint32_t word3;
} awg_payload_v1_t;

/* Event format version 1. */
typedef struct {
	uint32_t timestamp_ticks;
	uint16_t channel;
	uint16_t flags;
	awg_payload_v1_t payload;
} awg_event_v1_t;

/* Static scheduler configuration. */
typedef struct {
	uint32_t base_addr;
	uint32_t max_events;
	uint32_t tick_hz;
	uint32_t done_timeout_ms;
} awg_sched_cfg_t;

/* Runtime status snapshot. */
typedef struct {
	bool configured;
	bool armed;
	bool running;
	bool done;
	bool error;
	uint32_t loaded_events;
	uint32_t current_event;
	uint32_t hw_status_word;
} awg_sched_status_t;

int awg_sched_reset(void);
int awg_sched_config(const awg_sched_cfg_t *cfg);
int awg_sched_load_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_verify_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_arm(void);
int awg_sched_start(void);
int awg_sched_stop(void);
int awg_sched_get_status(awg_sched_status_t *status);
int awg_sched_wait_done(uint32_t timeout_ms, awg_sched_status_t *final_status);

#ifdef __cplusplus
}
#endif

#endif /* AWG_SCHED_H */
