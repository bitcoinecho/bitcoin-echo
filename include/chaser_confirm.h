/**
 * chaser_confirm.h — Sequential block confirmation chaser
 *
 * Confirms validated blocks to the chainstate in height order.
 *
 * Key features:
 * - Sequential confirmation (single-threaded)
 * - Handles chain reorganizations
 * - Updates chainstate with UTXOs
 * - Emits CHASE_ORGANIZED on success
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

#ifndef ECHO_CHASER_CONFIRM_H
#define ECHO_CHASER_CONFIRM_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chaser.h"
#include "chase.h"

/* Forward declarations */
typedef struct chainstate chainstate_t;

/**
 * Confirmation result
 */
typedef enum {
    CONFIRM_SUCCESS = 0,         /* Block confirmed successfully */
    CONFIRM_ERROR_LOOKUP,        /* Failed to look up block */
    CONFIRM_ERROR_APPLY,         /* Failed to apply to chainstate */
    CONFIRM_ERROR_REORG,         /* Reorg failed */
    CONFIRM_ERROR_INTERNAL,      /* Internal error */
} confirm_result_t;

/**
 * Chaser confirm - sequential block confirmation
 *
 * Applies validated blocks to chainstate in height order.
 * Handles reorganizations when a stronger chain is found.
 *
 * Uses a dedicated worker thread to process blocks asynchronously,
 * preventing the dispatcher mutex from being held during I/O-heavy
 * block confirmation operations.
 */
typedef struct {
    chaser_t base;               /* Base chaser (must be first) */

    /* Dependencies */
    chainstate_t *chainstate;    /* Chainstate to update (not owned) */

    /* State */
    uint32_t confirmed_height;   /* Highest confirmed block */
    uint32_t fork_point;         /* Fork point during reorg */

    /* Configuration */
    uint32_t top_checkpoint;     /* Height of top checkpoint */

    /* Worker thread */
    pthread_t worker;            /* Worker thread handle */
    pthread_mutex_t worker_mutex;/* Protects work_pending and shutdown */
    pthread_cond_t worker_cond;  /* Signals worker when work available */
    bool work_pending;           /* Work available for worker */
    bool worker_shutdown;        /* Signal worker to exit */
    bool worker_started;         /* Worker thread has been started */
} chaser_confirm_t;

/**
 * Create a new confirmation chaser
 *
 * @param node       Parent node
 * @param dispatcher Event dispatcher
 * @param chainstate Chainstate to update
 * @return New chaser, or NULL on failure
 */
chaser_confirm_t *chaser_confirm_create(node_t *node,
                                        chase_dispatcher_t *dispatcher,
                                        chainstate_t *chainstate);

/**
 * Destroy a confirmation chaser
 *
 * @param chaser Chaser to destroy
 */
void chaser_confirm_destroy(chaser_confirm_t *chaser);

/**
 * Get the confirmed chain height
 *
 * @param chaser Chaser to query
 * @return Highest confirmed block height
 */
uint32_t chaser_confirm_height(chaser_confirm_t *chaser);

/**
 * Confirm a validated block
 *
 * Applies the block to chainstate if it's the next in sequence.
 * Must be called in height order.
 *
 * @param chaser     Chaser to use
 * @param height     Block height
 * @param block_hash Block hash for lookup
 * @return Confirmation result
 */
confirm_result_t chaser_confirm_block(chaser_confirm_t *chaser, uint32_t height,
                                      const uint8_t block_hash[32]);

/**
 * Check if confirmation is bypassed for a height
 *
 * Checkpoints blocks are trusted and skip full confirmation.
 *
 * @param chaser Chaser to query
 * @param height Block height
 * @return true if confirmation should be bypassed
 */
bool chaser_confirm_is_bypass(chaser_confirm_t *chaser, uint32_t height);

/**
 * Reorganize to a new chain
 *
 * Rolls back to fork point and applies new chain.
 *
 * @param chaser     Chaser to use
 * @param fork_point Height of common ancestor
 * @return true on success
 */
bool chaser_confirm_reorganize(chaser_confirm_t *chaser, uint32_t fork_point);

/**
 * Set the checkpoint height for confirmation bypass
 *
 * Blocks at or below this height bypass full confirmation and just
 * update the height counter. Used during IBD to skip confirmation
 * of historically-validated blocks.
 *
 * @param chaser     Chaser to configure
 * @param height     Height of the checkpoint (blocks <= this are bypassed)
 */
void chaser_confirm_set_checkpoint(chaser_confirm_t *chaser, uint32_t height);

#endif /* ECHO_CHASER_CONFIRM_H */
