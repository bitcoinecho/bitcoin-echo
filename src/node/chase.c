/**
 * chase.c — Event-driven chaser communication system
 *
 * Thread-safe event dispatch using mutex-protected subscriber list.
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

/* Required for PTHREAD_MUTEX_RECURSIVE on Linux with -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include "chase.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/**
 * Subscription node in linked list
 */
struct chase_subscription {
    chase_handler_t handler;
    void *context;
    struct chase_subscription *next;
    bool marked_for_removal; /* Deferred removal during iteration */
};

/**
 * Chase dispatcher - manages event routing
 */
struct chase_dispatcher {
    chase_subscription_t *subscribers;
    pthread_mutex_t mutex;
    bool iterating; /* True during notify iteration */
};

chase_dispatcher_t *chase_dispatcher_create(void) {
    chase_dispatcher_t *dispatcher = calloc(1, sizeof(chase_dispatcher_t));
    if (!dispatcher) {
        return NULL;
    }

    /* Use recursive mutex to allow nested event dispatch */
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        free(dispatcher);
        return NULL;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(&dispatcher->mutex, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(dispatcher);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    dispatcher->subscribers = NULL;
    dispatcher->iterating = false;
    return dispatcher;
}

void chase_dispatcher_destroy(chase_dispatcher_t *dispatcher) {
    if (!dispatcher) {
        return;
    }

    pthread_mutex_lock(&dispatcher->mutex);

    /* Free all subscriptions */
    chase_subscription_t *current = dispatcher->subscribers;
    while (current) {
        chase_subscription_t *next = current->next;
        free(current);
        current = next;
    }

    pthread_mutex_unlock(&dispatcher->mutex);
    pthread_mutex_destroy(&dispatcher->mutex);
    free(dispatcher);
}

chase_subscription_t *chase_subscribe(chase_dispatcher_t *dispatcher,
                                      chase_handler_t handler, void *context) {
    if (!dispatcher || !handler) {
        return NULL;
    }

    chase_subscription_t *sub = calloc(1, sizeof(chase_subscription_t));
    if (!sub) {
        return NULL;
    }

    sub->handler = handler;
    sub->context = context;
    sub->marked_for_removal = false;

    pthread_mutex_lock(&dispatcher->mutex);

    /* Add to front of list */
    sub->next = dispatcher->subscribers;
    dispatcher->subscribers = sub;

    pthread_mutex_unlock(&dispatcher->mutex);

    return sub;
}

void chase_unsubscribe(chase_dispatcher_t *dispatcher,
                       chase_subscription_t *subscription) {
    if (!dispatcher || !subscription) {
        return;
    }

    pthread_mutex_lock(&dispatcher->mutex);

    if (dispatcher->iterating) {
        /* Mark for deferred removal */
        subscription->marked_for_removal = true;
    } else {
        /* Remove immediately */
        chase_subscription_t **pp = &dispatcher->subscribers;
        while (*pp) {
            if (*pp == subscription) {
                *pp = subscription->next;
                free(subscription);
                break;
            }
            pp = &(*pp)->next;
        }
    }

    pthread_mutex_unlock(&dispatcher->mutex);
}

/**
 * Remove subscriptions marked for removal
 * Must be called with mutex held
 */
static void cleanup_marked_subscriptions(chase_dispatcher_t *dispatcher) {
    chase_subscription_t **pp = &dispatcher->subscribers;
    while (*pp) {
        if ((*pp)->marked_for_removal) {
            chase_subscription_t *to_free = *pp;
            *pp = to_free->next;
            free(to_free);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void chase_notify(chase_dispatcher_t *dispatcher, chase_event_t event,
                  chase_value_t value) {
    if (!dispatcher) {
        return;
    }

    pthread_mutex_lock(&dispatcher->mutex);
    dispatcher->iterating = true;

    chase_subscription_t *current = dispatcher->subscribers;
    while (current) {
        if (!current->marked_for_removal) {
            /* Call handler - if it returns false, mark for removal */
            if (!current->handler(event, value, current->context)) {
                current->marked_for_removal = true;
            }
        }
        current = current->next;
    }

    dispatcher->iterating = false;
    cleanup_marked_subscriptions(dispatcher);

    pthread_mutex_unlock(&dispatcher->mutex);
}

const char *chase_event_name(chase_event_t event) {
    static const char *names[] = {
        [CHASE_START] = "start",
        [CHASE_BUMP] = "bump",
        [CHASE_STOP] = "stop",
        [CHASE_SUSPEND] = "suspend",
        [CHASE_RESUME] = "resume",
        [CHASE_HEADERS] = "headers",
        [CHASE_DOWNLOAD] = "download",
        [CHASE_BLOCKS] = "blocks",
        [CHASE_CHECKED] = "checked",
        [CHASE_UNCHECKED] = "unchecked",
        [CHASE_VALID] = "valid",
        [CHASE_UNVALID] = "unvalid",
        [CHASE_CONFIRMABLE] = "confirmable",
        [CHASE_UNCONFIRMABLE] = "unconfirmable",
        [CHASE_ORGANIZED] = "organized",
        [CHASE_REORGANIZED] = "reorganized",
        [CHASE_STARVED] = "starved",
        [CHASE_SPLIT] = "split",
        [CHASE_STALL] = "stall",
        [CHASE_PURGE] = "purge",
        [CHASE_REGRESSED] = "regressed",
        [CHASE_DISORGANIZED] = "disorganized",
    };

    if (event >= 0 && event < CHASE_EVENT_COUNT) {
        return names[event] ? names[event] : "unknown";
    }
    return "invalid";
}
