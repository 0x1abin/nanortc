/*
 * nanortc — wrap-safe uint32_t monotonic time helpers
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_TIME_H_
#define NANORTC_TIME_H_

#include <stdbool.h>
#include <stdint.h>

/* Deadlines are unambiguous when every scheduled interval is less than half
 * the uint32_t clock range (about 24.8 days). */
#define NANO_TIME_MAX_INTERVAL_MS UINT32_C(0x7FFFFFFF)

/** Return elapsed time using modulo-2^32 arithmetic. */
static inline uint32_t nano_time_elapsed(uint32_t now_ms, uint32_t since_ms)
{
    return now_ms - since_ms;
}

/** Return true when @p deadline_ms is now or in the past. */
static inline bool nano_time_is_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return nano_time_elapsed(now_ms, deadline_ms) <= NANO_TIME_MAX_INTERVAL_MS;
}

/** Return time remaining until a deadline, or zero once it is due. */
static inline uint32_t nano_time_until(uint32_t now_ms, uint32_t deadline_ms)
{
    return nano_time_is_due(now_ms, deadline_ms) ? 0u : (deadline_ms - now_ms);
}

/**
 * Schedule a positive interval while reserving zero as the "not armed"
 * sentinel used by protocol state structs. A deadline that would wrap to zero
 * is shifted by one millisecond.
 *
 * @pre delay_ms > 0 && delay_ms <= NANO_TIME_MAX_INTERVAL_MS
 */
static inline uint32_t nano_time_deadline(uint32_t now_ms, uint32_t delay_ms)
{
    uint32_t deadline_ms = now_ms + delay_ms;
    return deadline_ms == 0u ? 1u : deadline_ms;
}

#endif /* NANORTC_TIME_H_ */
