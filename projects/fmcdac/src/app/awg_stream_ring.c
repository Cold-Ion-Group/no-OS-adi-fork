#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "awg_stream_ring.h"

static int awg_stream_ring_valid(const awg_stream_ring_t *ring)
{
	return ring && ring->storage && ring->element_size != 0U &&
	       ring->capacity != 0U && ring->count <= ring->capacity &&
	       ring->read_index < ring->capacity &&
	       ring->write_index < ring->capacity;
}

static uint32_t awg_stream_ring_advance(const awg_stream_ring_t *ring,
		uint32_t index, uint32_t count)
{
	/* Both operands are below 2 * capacity, avoiding a 32-bit sum overflow. */
	count %= ring->capacity;
	if (count >= (ring->capacity - index))
		return count - (ring->capacity - index);

	return index + count;
}

int awg_stream_ring_init(awg_stream_ring_t *ring, void *storage,
		uint32_t capacity, size_t element_size)
{
	if (!ring || !storage || capacity == 0U || element_size == 0U)
		return -EINVAL;

	ring->storage = storage;
	ring->element_size = element_size;
	ring->capacity = capacity;
	ring->read_index = 0U;
	ring->write_index = 0U;
	ring->count = 0U;
	return 0;
}

void awg_stream_ring_reset(awg_stream_ring_t *ring)
{
	if (!ring)
		return;

	ring->read_index = 0U;
	ring->write_index = 0U;
	ring->count = 0U;
}

uint32_t awg_stream_ring_capacity(const awg_stream_ring_t *ring)
{
	return awg_stream_ring_valid(ring) ? ring->capacity : 0U;
}

uint32_t awg_stream_ring_count(const awg_stream_ring_t *ring)
{
	return awg_stream_ring_valid(ring) ? ring->count : 0U;
}

uint32_t awg_stream_ring_free(const awg_stream_ring_t *ring)
{
	return awg_stream_ring_valid(ring) ? ring->capacity - ring->count : 0U;
}

void *awg_stream_ring_producer_ptr(awg_stream_ring_t *ring)
{
	if (!awg_stream_ring_valid(ring) || ring->count == ring->capacity)
		return NULL;

	return ring->storage + ((size_t)ring->write_index * ring->element_size);
}

uint32_t awg_stream_ring_producer_contiguous(const awg_stream_ring_t *ring)
{
	uint32_t free_count;
	uint32_t to_end;

	if (!awg_stream_ring_valid(ring))
		return 0U;

	free_count = ring->capacity - ring->count;
	to_end = ring->capacity - ring->write_index;
	return (free_count < to_end) ? free_count : to_end;
}

int awg_stream_ring_produce(awg_stream_ring_t *ring, uint32_t count)
{
	if (!awg_stream_ring_valid(ring))
		return -EINVAL;
	if (count > awg_stream_ring_free(ring))
		return -ENOSPC;

	ring->write_index = awg_stream_ring_advance(ring, ring->write_index,
						    count);
	ring->count += count;
	return 0;
}

void *awg_stream_ring_consumer_ptr(awg_stream_ring_t *ring)
{
	if (!awg_stream_ring_valid(ring) || ring->count == 0U)
		return NULL;

	return ring->storage + ((size_t)ring->read_index * ring->element_size);
}

const void *awg_stream_ring_consumer_const_ptr(const awg_stream_ring_t *ring)
{
	if (!awg_stream_ring_valid(ring) || ring->count == 0U)
		return NULL;

	return ring->storage + ((size_t)ring->read_index * ring->element_size);
}

uint32_t awg_stream_ring_consumer_contiguous(const awg_stream_ring_t *ring)
{
	uint32_t to_end;

	if (!awg_stream_ring_valid(ring))
		return 0U;

	to_end = ring->capacity - ring->read_index;
	return (ring->count < to_end) ? ring->count : to_end;
}

int awg_stream_ring_consume(awg_stream_ring_t *ring, uint32_t count)
{
	if (!awg_stream_ring_valid(ring))
		return -EINVAL;
	if (count > ring->count)
		return -ENODATA;

	ring->read_index = awg_stream_ring_advance(ring, ring->read_index, count);
	ring->count -= count;
	return 0;
}

void *awg_stream_ring_at(awg_stream_ring_t *ring, uint32_t offset)
{
	uint32_t index;

	if (!awg_stream_ring_valid(ring) || offset >= ring->count)
		return NULL;

	index = awg_stream_ring_advance(ring, ring->read_index, offset);
	return ring->storage + ((size_t)index * ring->element_size);
}

const void *awg_stream_ring_const_at(const awg_stream_ring_t *ring,
		uint32_t offset)
{
	uint32_t index;

	if (!awg_stream_ring_valid(ring) || offset >= ring->count)
		return NULL;

	index = awg_stream_ring_advance(ring, ring->read_index, offset);
	return ring->storage + ((size_t)index * ring->element_size);
}

void *awg_stream_ring_last(awg_stream_ring_t *ring)
{
	if (!awg_stream_ring_valid(ring) || ring->count == 0U)
		return NULL;

	return awg_stream_ring_at(ring, ring->count - 1U);
}

int awg_stream_ring_push(awg_stream_ring_t *ring, const void *elements,
		uint32_t count)
{
	const uint8_t *source = elements;
	uint32_t remaining = count;

	if (!awg_stream_ring_valid(ring))
		return -EINVAL;
	if (count == 0U)
		return 0;
	if (!elements)
		return -EINVAL;
	if (count > awg_stream_ring_free(ring))
		return -ENOSPC;

	while (remaining > 0U) {
		uint32_t contiguous = awg_stream_ring_producer_contiguous(ring);
		uint32_t to_copy = (remaining < contiguous) ? remaining : contiguous;
		void *destination = awg_stream_ring_producer_ptr(ring);

		memcpy(destination, source, (size_t)to_copy * ring->element_size);
		(void)awg_stream_ring_produce(ring, to_copy);
		source += (size_t)to_copy * ring->element_size;
		remaining -= to_copy;
	}

	return 0;
}
