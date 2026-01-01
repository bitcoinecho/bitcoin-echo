/**
 * chase.h — Event-driven chaser communication system
 *
 * Modeled after libbitcoin-node's chase event system.
 * Chasers subscribe to events and react asynchronously.
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

#ifndef ECHO_CHASE_H
#define ECHO_CHASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Chase events - signals between chasers
 *
 * Events flow through the system to coordinate:
 * - Block downloading (headers → download → checked)
 * - Validation (checked → valid/unvalid)
 * - Confirmation (valid → confirmable → organized)
 * - Channel management (starved, split, stall)
 */
typedef enum {
    /* Work shuffling */
    CHASE_START,          /* Chasers directed to start operating */
    CHASE_BUMP,           /* Attempt start from current position */
    CHASE_STOP,           /* Service stopping */
    CHASE_SUSPEND,        /* Channels directed to stop */
    CHASE_RESUME,         /* Chasers directed to resume after suspend */

    /* Candidate chain */
    CHASE_HEADERS,        /* New candidate headers available */
    CHASE_DOWNLOAD,       /* New blocks ready for download */
    CHASE_BLOCKS,         /* New candidate branch from branch point */

    /* Check/Download */
    CHASE_CHECKED,        /* Block downloaded and checked */
    CHASE_UNCHECKED,      /* Downloaded block failed check */

    /* Validation */
    CHASE_VALID,          /* Block validated successfully */
    CHASE_UNVALID,        /* Block failed validation */

    /* Confirmation */
    CHASE_CONFIRMABLE,    /* Block ready for confirmation */
    CHASE_UNCONFIRMABLE,  /* Block failed confirmability */
    CHASE_ORGANIZED,      /* Block confirmed to chain */
    CHASE_REORGANIZED,    /* Block unconfirmed (reorg) */

    /* Channel management */
    CHASE_STARVED,        /* Channel needs work */
    CHASE_STALL,          /* All peers stalled */
    CHASE_PURGE,          /* Drop work and stop */

    /* Regression handling */
    CHASE_REGRESSED,      /* Candidate chain reorganized */
    CHASE_DISORGANIZED,   /* Unchecked/unvalid/unconfirmable handled */

    CHASE_EVENT_COUNT     /* Number of event types */
} chase_event_t;

/**
 * Event value - polymorphic payload
 *
 * Different events carry different value types:
 * - height_t: block height (CHASE_VALID, CHASE_ORGANIZED, etc.)
 * - link_t: block link/hash reference (CHASE_UNCHECKED, CHASE_UNVALID)
 * - count_t: count value (CHASE_DOWNLOAD)
 * - object_t: object pointer (CHASE_STARVED)
 */
typedef union {
    uint32_t height;      /* Block height */
    uint64_t link;        /* Block link/reference */
    size_t count;         /* Count value */
    void *object;         /* Object pointer */
} chase_value_t;

/**
 * Event handler callback
 *
 * @param event   The event type
 * @param value   Event-specific value
 * @param context User-provided context pointer
 * @return true to continue receiving events, false to unsubscribe
 */
typedef bool (*chase_handler_t)(chase_event_t event, chase_value_t value,
                                void *context);

/**
 * Subscription handle - opaque type
 */
typedef struct chase_subscription chase_subscription_t;

/**
 * Chase dispatcher - manages event routing
 */
typedef struct chase_dispatcher chase_dispatcher_t;

/**
 * Create a new chase dispatcher
 *
 * @return New dispatcher, or NULL on allocation failure
 */
chase_dispatcher_t *chase_dispatcher_create(void);

/**
 * Destroy a chase dispatcher
 *
 * Unsubscribes all handlers and frees resources.
 *
 * @param dispatcher Dispatcher to destroy
 */
void chase_dispatcher_destroy(chase_dispatcher_t *dispatcher);

/**
 * Subscribe to chase events
 *
 * The handler will be called for all events. Filter in the handler.
 *
 * @param dispatcher Event dispatcher
 * @param handler    Callback function
 * @param context    User context passed to handler
 * @return Subscription handle, or NULL on failure
 */
chase_subscription_t *chase_subscribe(chase_dispatcher_t *dispatcher,
                                      chase_handler_t handler,
                                      void *context);

/**
 * Unsubscribe from chase events
 *
 * @param dispatcher   Event dispatcher
 * @param subscription Subscription to cancel
 */
void chase_unsubscribe(chase_dispatcher_t *dispatcher,
                       chase_subscription_t *subscription);

/**
 * Notify all subscribers of an event
 *
 * Subscribers returning false are automatically unsubscribed.
 *
 * @param dispatcher Event dispatcher
 * @param event      Event type
 * @param value      Event value
 */
void chase_notify(chase_dispatcher_t *dispatcher, chase_event_t event,
                  chase_value_t value);

/**
 * Convenience: notify with height value
 */
static inline void chase_notify_height(chase_dispatcher_t *dispatcher,
                                       chase_event_t event, uint32_t height) {
    chase_value_t value = {.height = height};
    chase_notify(dispatcher, event, value);
}

/**
 * Convenience: notify with link value
 */
static inline void chase_notify_link(chase_dispatcher_t *dispatcher,
                                     chase_event_t event, uint64_t link) {
    chase_value_t value = {.link = link};
    chase_notify(dispatcher, event, value);
}

/**
 * Convenience: notify with count value
 */
static inline void chase_notify_count(chase_dispatcher_t *dispatcher,
                                      chase_event_t event, size_t count) {
    chase_value_t value = {.count = count};
    chase_notify(dispatcher, event, value);
}

/**
 * Convenience: notify with no value (default event)
 */
static inline void chase_notify_default(chase_dispatcher_t *dispatcher,
                                        chase_event_t event) {
    chase_value_t value = {0};
    chase_notify(dispatcher, event, value);
}

/**
 * Get event name for logging
 *
 * @param event Event type
 * @return Static string name
 */
const char *chase_event_name(chase_event_t event);

#endif /* ECHO_CHASE_H */
