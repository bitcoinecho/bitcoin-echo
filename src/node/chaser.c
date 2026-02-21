/**
 * chaser.c — Base chaser implementation
 *
 * Provides common functionality for all chaser types.
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

#include "chaser.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

/**
 * Internal event handler that dispatches to derived class
 */
static bool chaser_event_handler(chase_event_t event, chase_value_t value,
                                 void *context) {
    chaser_t *self = (chaser_t *)context;

    /* Check if closed */
    pthread_mutex_lock(&self->mutex);
    bool closed = self->closed;
    pthread_mutex_unlock(&self->mutex);

    if (closed) {
        return false; /* Unsubscribe */
    }

    /* Handle stop event */
    if (event == CHASE_STOP) {
        chaser_stop(self);
        return false; /* Unsubscribe */
    }

    /* Delegate to derived class */
    if (self->vtable && self->vtable->handle_event) {
        return self->vtable->handle_event(self, event, value);
    }

    return true; /* Keep subscription */
}

int chaser_init(chaser_t *self, const chaser_vtable_t *vtable, node_t *node,
                chase_dispatcher_t *dispatcher, const char *name) {
    if (!self || !dispatcher) {
        return -1;
    }

    memset(self, 0, sizeof(chaser_t));

    self->vtable = vtable;
    self->node = node;
    self->dispatcher = dispatcher;
    self->name = name ? name : "chaser";
    self->position = 0;
    self->closed = false;
    self->suspended = false;
    self->subscription = NULL;

    if (pthread_mutex_init(&self->mutex, NULL) != 0) {
        return -1;
    }

    return 0;
}

void chaser_cleanup(chaser_t *self) {
    if (!self) {
        return;
    }

    /* Unsubscribe from events */
    if (self->subscription && self->dispatcher) {
        chase_unsubscribe(self->dispatcher, self->subscription);
        self->subscription = NULL;
    }

    pthread_mutex_destroy(&self->mutex);
}

int chaser_start(chaser_t *self) {
    if (!self) {
        return -1;
    }

    /* Subscribe to events */
    self->subscription =
        chase_subscribe(self->dispatcher, chaser_event_handler, self);
    if (!self->subscription) {
        return -1;
    }

    /* Call derived start method */
    if (self->vtable && self->vtable->start) {
        return self->vtable->start(self);
    }

    return 0;
}

void chaser_stop(chaser_t *self) {
    if (!self) {
        return;
    }

    pthread_mutex_lock(&self->mutex);
    self->closed = true;
    pthread_mutex_unlock(&self->mutex);

    /* Call derived stop method */
    if (self->vtable && self->vtable->stop) {
        self->vtable->stop(self);
    }
}

void chaser_destroy(chaser_t *self) {
    if (!self) {
        return;
    }

    /* Clean up base resources first (before derived destroy may free self) */
    chaser_cleanup(self);

    /* Call derived destroy method (may free self) */
    if (self->vtable && self->vtable->destroy) {
        self->vtable->destroy(self);
    }
}

bool chaser_is_closed(chaser_t *self) {
    if (!self) {
        return true;
    }

    pthread_mutex_lock(&self->mutex);
    bool closed = self->closed;
    pthread_mutex_unlock(&self->mutex);
    return closed;
}

bool chaser_is_suspended(chaser_t *self) {
    if (!self) {
        return false;
    }

    pthread_mutex_lock(&self->mutex);
    bool suspended = self->suspended;
    pthread_mutex_unlock(&self->mutex);
    return suspended;
}

void chaser_suspend(chaser_t *self) {
    if (!self) {
        return;
    }

    pthread_mutex_lock(&self->mutex);
    self->suspended = true;
    pthread_mutex_unlock(&self->mutex);
}

void chaser_resume(chaser_t *self) {
    if (!self) {
        return;
    }

    pthread_mutex_lock(&self->mutex);
    self->suspended = false;
    pthread_mutex_unlock(&self->mutex);

    /* Notify resume event so chasers can restart */
    chaser_notify_height(self, CHASE_RESUME, 0);
}

void chaser_fault(chaser_t *self, int error) {
    if (!self) {
        return;
    }

    /* Log the fault before dispatching stop — flush is not guaranteed after
     * CHASE_STOP is sent, so the log call must precede the notify. */
    log_error(LOG_COMP_SYNC,
              "chaser '%s' fault: error=%d — initiating shutdown",
              self->name, error);

    /* Notify system of fault - this triggers shutdown */
    chase_value_t value = {.count = (size_t)error};
    chase_notify(self->dispatcher, CHASE_STOP, value);
}

uint32_t chaser_position(chaser_t *self) {
    if (!self) {
        return 0;
    }

    pthread_mutex_lock(&self->mutex);
    uint32_t pos = self->position;
    pthread_mutex_unlock(&self->mutex);
    return pos;
}

void chaser_set_position(chaser_t *self, uint32_t position) {
    if (!self) {
        return;
    }

    pthread_mutex_lock(&self->mutex);
    self->position = position;
    pthread_mutex_unlock(&self->mutex);
}

void chaser_notify(chaser_t *self, chase_event_t event, chase_value_t value) {
    if (!self || !self->dispatcher) {
        return;
    }

    chase_notify(self->dispatcher, event, value);
}

void chaser_lock(chaser_t *self) {
    if (self) {
        pthread_mutex_lock(&self->mutex);
    }
}

void chaser_unlock(chaser_t *self) {
    if (self) {
        pthread_mutex_unlock(&self->mutex);
    }
}
