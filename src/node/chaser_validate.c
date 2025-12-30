/**
 * chaser_validate.c — Parallel block validation chaser
 *
 * Implements concurrent block validation using a threadpool.
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

#include "chaser_validate.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "block.h"
#include "log.h"
#include "node.h"

/* Default configuration
 *
 * libbitcoin-node uses maximum_concurrency (typically hundreds) for both
 * the download window and validation backlog. This ensures the validator
 * always has work available.
 *
 * Worker count should match CPU cores for maximum parallel validation.
 * Backlog should be large enough to keep all workers busy.
 */
#define DEFAULT_WORKER_COUNT 0  /* 0 = auto-detect CPU count */
#define DEFAULT_MAX_BACKLOG 500 /* libbitcoin uses maximum_concurrency */

/* Forward declarations */
static int validate_start(chaser_t *self);
static bool validate_handle_event(chaser_t *self, chase_event_t event,
                                  chase_value_t value);
static void validate_stop(chaser_t *self);
static void validate_destroy(chaser_t *self);
static void *worker_thread(void *arg);

static const chaser_vtable_t validate_vtable = {
    .start = validate_start,
    .handle_event = validate_handle_event,
    .stop = validate_stop,
    .destroy = validate_destroy,
};

/*
 * Work Queue Implementation
 */

static int queue_init(work_queue_t *queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->shutdown = false;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return -1;
    }

    return 0;
}

static void queue_cleanup(work_queue_t *queue) {
    /* Free any remaining work items */
    validate_work_t *current = queue->head;
    while (current) {
        validate_work_t *next = current->next;
        free(current);
        current = next;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
}

static void queue_shutdown(work_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = true;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

static int queue_push(work_queue_t *queue, validate_work_t *work) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }

    work->next = NULL;

    if (queue->tail) {
        queue->tail->next = work;
    } else {
        queue->head = work;
    }
    queue->tail = work;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static validate_work_t *queue_pop(work_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);

    while (!queue->head && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->shutdown && !queue->head) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    validate_work_t *work = queue->head;
    queue->head = work->next;
    if (!queue->head) {
        queue->tail = NULL;
    }

    pthread_mutex_unlock(&queue->mutex);
    return work;
}

/*
 * Chaser Implementation
 */

chaser_validate_t *chaser_validate_create(node_t *node,
                                          chase_dispatcher_t *dispatcher,
                                          chainstate_t *chainstate,
                                          size_t worker_count,
                                          size_t max_backlog) {
    chaser_validate_t *chaser = calloc(1, sizeof(chaser_validate_t));
    if (!chaser) {
        return NULL;
    }

    /* Initialize base chaser */
    if (chaser_init(&chaser->base, &validate_vtable, node, dispatcher,
                    "validate") != 0) {
        free(chaser);
        return NULL;
    }

    /* Initialize work queue */
    if (queue_init(&chaser->queue) != 0) {
        chaser_cleanup(&chaser->base);
        free(chaser);
        return NULL;
    }

    /* Set configuration */
    chaser->chainstate = chainstate;

    /* Auto-detect CPU count if worker_count is 0 */
    if (worker_count == 0) {
        long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
        if (cpu_count <= 0) {
            cpu_count = 4; /* Fallback */
        }
        chaser->worker_count = (size_t)cpu_count;
    } else {
        chaser->worker_count = worker_count;
    }

    chaser->maximum_backlog =
        max_backlog > 0 ? max_backlog : DEFAULT_MAX_BACKLOG;
    atomic_init(&chaser->backlog, 0);
    chaser->top_checkpoint = 0; /* TODO: Get from config */
    chaser->defer_validation = false;

    log_info(LOG_COMP_SYNC, "chaser_validate: worker_count=%zu, max_backlog=%zu",
             chaser->worker_count, chaser->maximum_backlog);

    /* Create worker threads */
    chaser->workers = calloc(chaser->worker_count, sizeof(pthread_t));
    if (!chaser->workers) {
        queue_cleanup(&chaser->queue);
        chaser_cleanup(&chaser->base);
        free(chaser);
        return NULL;
    }

    for (size_t i = 0; i < chaser->worker_count; i++) {
        if (pthread_create(&chaser->workers[i], NULL, worker_thread, chaser) !=
            0) {
            /* Shutdown already-created workers */
            queue_shutdown(&chaser->queue);
            for (size_t j = 0; j < i; j++) {
                pthread_join(chaser->workers[j], NULL);
            }
            free(chaser->workers);
            queue_cleanup(&chaser->queue);
            chaser_cleanup(&chaser->base);
            free(chaser);
            return NULL;
        }
    }

    return chaser;
}

void chaser_validate_destroy(chaser_validate_t *chaser) {
    if (!chaser) {
        return;
    }

    /* Stop workers first - signal queue shutdown */
    chaser_stop(&chaser->base);

    /* Now destroy which will join workers */
    chaser_destroy(&chaser->base);
}

size_t chaser_validate_backlog(chaser_validate_t *chaser) {
    if (!chaser) {
        return 0;
    }
    return atomic_load(&chaser->backlog);
}

bool chaser_validate_is_bypass(chaser_validate_t *chaser, uint32_t height) {
    if (!chaser) {
        return false;
    }

    /* Bypass validation for blocks at or below checkpoint */
    if (height <= chaser->top_checkpoint) {
        return true;
    }

    /* Bypass if validation is deferred */
    if (chaser->defer_validation) {
        return true;
    }

    return false;
}

void chaser_validate_set_checkpoint(chaser_validate_t *chaser, uint32_t height) {
    if (!chaser) {
        return;
    }

    chaser->top_checkpoint = height;
    log_info(LOG_COMP_SYNC,
             "chaser_validate: checkpoint set to %u (blocks <= this bypass validation)",
             height);
}

int chaser_validate_submit(chaser_validate_t *chaser, uint32_t height,
                           const uint8_t block_hash[32], bool bypass) {
    if (!chaser) {
        return -1;
    }

    /* Check backlog limit */
    size_t current = atomic_load(&chaser->backlog);
    if (current >= chaser->maximum_backlog) {
        return -1; /* Backlog full */
    }

    /* Create work item */
    validate_work_t *work = malloc(sizeof(validate_work_t));
    if (!work) {
        return -1;
    }

    work->height = height;
    memcpy(work->block_hash, block_hash, 32);
    work->bypass = bypass;
    work->next = NULL;

    /* Increment backlog before queuing */
    atomic_fetch_add(&chaser->backlog, 1);

    /* Queue the work */
    if (queue_push(&chaser->queue, work) != 0) {
        atomic_fetch_sub(&chaser->backlog, 1);
        free(work);
        return -1;
    }

    return 0;
}

/*
 * Worker Thread
 */

static void *worker_thread(void *arg) {
    chaser_validate_t *chaser = (chaser_validate_t *)arg;

    while (1) {
        validate_work_t *work = queue_pop(&chaser->queue);
        if (!work) {
            break; /* Queue shutdown */
        }

        /* Check if we should stop */
        if (chaser_is_closed(&chaser->base)) {
            free(work);
            break;
        }

        /* Perform validation */
        int result = 0; /* 0 = success */
        size_t prev;

        if (!work->bypass) {
            node_t *node = chaser->base.node;

            /* Skip validation if block already confirmed.
             * This can happen due to race: block was queued for validation
             * but confirmed before worker got to it. UTXO state has moved
             * past this height, so validation would fail with UTXO errors. */
            uint32_t confirmed_height = node_get_validated_height(node);
            if (work->height <= confirmed_height) {
                /* Already confirmed - treat as valid */
                log_debug(LOG_COMP_SYNC,
                          "chaser_validate: block %u already confirmed (height=%u), skipping",
                          work->height, confirmed_height);
                goto notify_valid;
            }

            /* Load block from storage */
            block_t block;
            hash256_t hash;
            echo_result_t load_result =
                node_load_block_at_height(node, work->height, &block, &hash);

            if (load_result != ECHO_OK) {
                log_error(LOG_COMP_SYNC,
                          "chaser_validate: failed to load block %u: %d",
                          work->height, load_result);
                result = -1;
            } else {
                /* Re-check confirmation state right before validation.
                 * There's a race window between the first check and here where
                 * chaser_confirm may have applied this block via CHASE_BUMP. */
                confirmed_height = node_get_validated_height(node);
                if (work->height <= confirmed_height) {
                    log_debug(LOG_COMP_SYNC,
                              "chaser_validate: block %u confirmed during load (height=%u), skipping",
                              work->height, confirmed_height);
                    block_free(&block);
                    goto notify_valid;
                }

                /* Validate block (read-only, no chainstate modification) */
                if (!node_validate_block(node, &block)) {
                    /* Check if block was confirmed DURING validation (race condition).
                     * If so, the "failure" is spurious - block was applied correctly. */
                    uint32_t post_confirmed = node_get_validated_height(node);
                    if (work->height <= post_confirmed) {
                        log_debug(LOG_COMP_SYNC,
                                  "chaser_validate: block %u validation failed but already "
                                  "confirmed (race condition, height=%u), treating as valid",
                                  work->height, post_confirmed);
                        block_free(&block);
                        goto notify_valid;
                    }
                    log_error(LOG_COMP_SYNC,
                              "chaser_validate: block %u validation failed (confirmed=%u)",
                              work->height, post_confirmed);
                    result = -1;
                }
                block_free(&block);
            }
        }

notify_valid:
        /* Decrement backlog */
        prev = atomic_fetch_sub(&chaser->backlog, 1);

        /* Notify completion */
        if (result == 0) {
            /* VALID BLOCK */
            chaser_notify_height(&chaser->base, CHASE_VALID, work->height);
        } else {
            /* INVALID BLOCK */
            chase_value_t value = {.height = work->height};
            chaser_notify(&chaser->base, CHASE_UNVALID, value);
        }

        /* If backlog was at 1 (now 0), bump to check for more work */
        if (prev == 1) {
            chaser_notify_height(&chaser->base, CHASE_BUMP, 0);
        }

        free(work);
    }

    return NULL;
}

/*
 * Chaser Virtual Methods
 */

static int validate_start(chaser_t *self) {
    chaser_validate_t *chaser = (chaser_validate_t *)self;
    (void)chaser;

    /* Position is set by the node based on database state */
    /* Workers are already started in create() */

    return 0;
}

static bool validate_handle_event(chaser_t *self, chase_event_t event,
                                  chase_value_t value) {
    chaser_validate_t *chaser = (chaser_validate_t *)self;

    if (chaser_is_closed(self)) {
        return false;
    }

    /* Stop generating work during suspension */
    if (chaser_is_suspended(self)) {
        return true;
    }

    switch (event) {
    case CHASE_RESUME:
    case CHASE_START:
    case CHASE_BUMP:
        /* Check for stored blocks ready to validate */
        {
            node_t *node = chaser->base.node;
            uint32_t position = chaser_position(self);

            /* Try to find and submit stored blocks */
            while (1) {
                uint32_t next_height = position + 1;

                /* Check if we can accept more work */
                if (atomic_load(&chaser->backlog) >= chaser->maximum_backlog) {
                    break;
                }

                /* Try to load block at next height */
                block_t block;
                hash256_t hash;
                echo_result_t result =
                    node_load_block_at_height(node, next_height, &block, &hash);

                if (result != ECHO_OK) {
                    break; /* Block not stored yet */
                }

                block_free(&block); /* We just needed to check it exists */

                /* Submit for validation */
                bool bypass = chaser_validate_is_bypass(chaser, next_height);
                if (chaser_validate_submit(chaser, next_height, hash.bytes,
                                           bypass) == 0) {
                    chaser_set_position(self, next_height);
                    position = next_height;
                } else {
                    break; /* Queue full or error */
                }
            }
        }
        break;

    case CHASE_CHECKED:
        /* Block has been downloaded and checked */
        /* value.height is the checked block height */
        {
            uint32_t height = value.height;
            uint32_t position = chaser_position(self);

            /* Can only validate if all previous blocks are done */
            if (height == position + 1) {
                /* TODO: Get block hash from database */
                /* For now, submit with empty hash */
                uint8_t hash[32] = {0};
                bool bypass = chaser_validate_is_bypass(chaser, height);

                if (chaser_validate_submit(chaser, height, hash, bypass) == 0) {
                    chaser_set_position(self, height);
                }
            }
        }
        break;

    case CHASE_REGRESSED:
    case CHASE_DISORGANIZED:
        /* Regression - reset position */
        {
            uint32_t branch_point = value.height;
            if (branch_point < chaser_position(self)) {
                chaser_set_position(self, branch_point);
            }
        }
        break;

    case CHASE_STOP:
        return false;

    default:
        break;
    }

    return true;
}

static void validate_stop(chaser_t *self) {
    chaser_validate_t *chaser = (chaser_validate_t *)self;

    /* Shutdown work queue - workers will exit */
    queue_shutdown(&chaser->queue);
}

static void validate_destroy(chaser_t *self) {
    chaser_validate_t *chaser = (chaser_validate_t *)self;

    /* Wait for all workers to finish */
    if (chaser->workers) {
        for (size_t i = 0; i < chaser->worker_count; i++) {
            pthread_join(chaser->workers[i], NULL);
        }
        free(chaser->workers);
        chaser->workers = NULL;
    }

    /* Cleanup queue */
    queue_cleanup(&chaser->queue);

    /* Free the chaser itself */
    free(chaser);
}
