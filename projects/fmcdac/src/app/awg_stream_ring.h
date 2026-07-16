#ifndef AWG_STREAM_RING_H
#define AWG_STREAM_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Single-producer/single-consumer element ring.  The object does not allocate
 * storage and does not provide inter-context synchronization; callers that
 * share it with an ISR must protect index updates themselves.  Producer and
 * consumer indices are deliberately independent so DMA users can reserve a
 * contiguous extent and consume it only after EOT.
 */
typedef struct {
	uint8_t *storage;
	size_t element_size;
	uint32_t capacity;
	uint32_t read_index;
	uint32_t write_index;
	uint32_t count;
} awg_stream_ring_t;

int awg_stream_ring_init(awg_stream_ring_t *ring, void *storage,
		uint32_t capacity, size_t element_size);
void awg_stream_ring_reset(awg_stream_ring_t *ring);

uint32_t awg_stream_ring_capacity(const awg_stream_ring_t *ring);
uint32_t awg_stream_ring_count(const awg_stream_ring_t *ring);
uint32_t awg_stream_ring_free(const awg_stream_ring_t *ring);

void *awg_stream_ring_producer_ptr(awg_stream_ring_t *ring);
uint32_t awg_stream_ring_producer_contiguous(const awg_stream_ring_t *ring);
int awg_stream_ring_produce(awg_stream_ring_t *ring, uint32_t count);

void *awg_stream_ring_consumer_ptr(awg_stream_ring_t *ring);
const void *awg_stream_ring_consumer_const_ptr(const awg_stream_ring_t *ring);
uint32_t awg_stream_ring_consumer_contiguous(const awg_stream_ring_t *ring);
int awg_stream_ring_consume(awg_stream_ring_t *ring, uint32_t count);

void *awg_stream_ring_at(awg_stream_ring_t *ring, uint32_t offset);
const void *awg_stream_ring_const_at(const awg_stream_ring_t *ring,
		uint32_t offset);
void *awg_stream_ring_last(awg_stream_ring_t *ring);

int awg_stream_ring_push(awg_stream_ring_t *ring, const void *elements,
		uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* AWG_STREAM_RING_H */
