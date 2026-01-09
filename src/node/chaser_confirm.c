/**
 * chaser_confirm.c — Sequential block confirmation chaser
 *
 * Confirms validated blocks to chainstate in height order.
 *
 * Copyright (c) 2024 Bitcoin Echo
 * SPDX-License-Identifier: MIT
 */

#include "chaser_confirm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "block.h"
#include "block_index_db.h"
#include "chainstate.h"
#include "log.h"
#include "node.h"

/*
 * WAL checkpoint interval during IBD.
 * Checkpoint every N blocks to prevent SQLite WAL from growing unbounded.
 * Without this, WAL grows to GB and reads slow to a crawl.
 */
#define CHECKPOINT_INTERVAL 10000

/* Forward declarations */
static int confirm_start(chaser_t *self);
static bool confirm_handle_event(chaser_t *self, chase_event_t event,
                                 chase_value_t value);
static void confirm_stop(chaser_t *self);
static void confirm_destroy(chaser_t *self);

static const chaser_vtable_t confirm_vtable = {
    .start = confirm_start,
    .handle_event = confirm_handle_event,
    .stop = confirm_stop,
    .destroy = confirm_destroy,
};

/*
 * Chaser Implementation
 */

chaser_confirm_t *chaser_confirm_create(node_t *node,
                                        chase_dispatcher_t *dispatcher,
                                        chainstate_t *chainstate) {
    chaser_confirm_t *chaser = calloc(1, sizeof(chaser_confirm_t));
    if (!chaser) {
        return NULL;
    }

    /* Initialize base chaser */
    if (chaser_init(&chaser->base, &confirm_vtable, node, dispatcher,
                    "confirm") != 0) {
        free(chaser);
        return NULL;
    }

    chaser->chainstate = chainstate;
    chaser->confirmed_height = 0;
    chaser->fork_point = 0;
    chaser->top_checkpoint = 0; /* TODO: Get from config */

    return chaser;
}

void chaser_confirm_destroy(chaser_confirm_t *chaser) {
    if (!chaser) {
        return;
    }

    chaser_stop(&chaser->base);
    chaser_destroy(&chaser->base);
}

uint32_t chaser_confirm_height(chaser_confirm_t *chaser) {
    if (!chaser) {
        return 0;
    }

    chaser_lock(&chaser->base);
    uint32_t height = chaser->confirmed_height;
    chaser_unlock(&chaser->base);
    return height;
}

/*
 * Internal confirmation with optional preloaded block.
 * If preloaded_block is NULL, loads from storage. Otherwise uses the provided block.
 * Caller retains ownership of preloaded_block (we don't free it).
 */
static confirm_result_t confirm_block_internal(chaser_confirm_t *chaser,
                                               uint32_t height,
                                               const uint8_t block_hash[32],
                                               const block_t *preloaded_block,
                                               const hash256_t *preloaded_hash) {
    if (!chaser) {
        return CONFIRM_ERROR_INTERNAL;
    }

    (void)block_hash; /* Hash passed for interface compatibility */

    node_t *node = chaser->base.node;
    if (!node) {
        return CONFIRM_ERROR_INTERNAL;
    }

    chaser_lock(&chaser->base);

    /* Must be next block in sequence */
    if (height != chaser->confirmed_height + 1) {
        chaser_unlock(&chaser->base);
        return CONFIRM_ERROR_INTERNAL;
    }

    chaser_unlock(&chaser->base);

    /* Use preloaded block or load from storage */
    block_t local_block;
    hash256_t local_hash;
    const block_t *block;
    const hash256_t *hash;

    if (preloaded_block != NULL) {
        block = preloaded_block;
        hash = preloaded_hash;
    } else {
        echo_result_t result =
            node_load_block_at_height(node, height, &local_block, &local_hash);
        if (result != ECHO_OK) {
            log_error(LOG_COMP_SYNC,
                      "chaser_confirm: failed to load block %u: %d", height,
                      result);
            return CONFIRM_ERROR_LOOKUP;
        }
        block = &local_block;
        hash = &local_hash;
    }

    /* Apply block to chainstate (validation already done by chaser_validate) */
    echo_result_t result = node_apply_block(node, block);

    /* Free local block if we loaded it (preloaded block is caller's responsibility) */
    if (preloaded_block == NULL) {
        block_free(&local_block);
    }

    if (result != ECHO_OK) {
        log_error(LOG_COMP_SYNC, "chaser_confirm: block %u apply failed: %d",
                  height, result);
        return CONFIRM_ERROR_APPLY;
    }

    /* Update confirmed height */
    chaser_lock(&chaser->base);
    chaser->confirmed_height = height;
    chaser_unlock(&chaser->base);

    /* Notify that block is organized */
    chaser_notify_height(&chaser->base, CHASE_ORGANIZED, height);

    /*
     * Announce valid block to peers (skip during IBD - Core behavior).
     * This enables unified validation path: both IBD and post-IBD blocks
     * flow through the chase system and get announced here.
     */
    node_announce_block_to_peers(node, hash);

    return CONFIRM_SUCCESS;
}

confirm_result_t chaser_confirm_block(chaser_confirm_t *chaser, uint32_t height,
                                      const uint8_t block_hash[32]) {
    /* Public API: load block from storage */
    return confirm_block_internal(chaser, height, block_hash, NULL, NULL);
}

bool chaser_confirm_is_bypass(chaser_confirm_t *chaser, uint32_t height) {
    if (!chaser) {
        return false;
    }

    /* Bypass confirmation for blocks at or below checkpoint */
    return height <= chaser->top_checkpoint;
}

void chaser_confirm_set_checkpoint(chaser_confirm_t *chaser, uint32_t height) {
    if (!chaser) {
        return;
    }

    chaser->top_checkpoint = height;
    log_info(LOG_COMP_SYNC,
             "chaser_confirm: checkpoint set to %u (blocks <= this bypass confirmation)",
             height);
}

bool chaser_confirm_reorganize(chaser_confirm_t *chaser, uint32_t fork_point) {
    if (!chaser) {
        return false;
    }

    chaser_lock(&chaser->base);

    /* Can't reorganize below confirmed height if it's below fork point */
    if (fork_point > chaser->confirmed_height) {
        chaser_unlock(&chaser->base);
        return false;
    }

    /* Roll back to fork point */
    /* TODO: Actually undo chainstate changes */
    uint32_t old_height = chaser->confirmed_height;

    /* Notify reorganization for each block rolled back */
    for (uint32_t h = old_height; h > fork_point; h--) {
        chaser_notify_height(&chaser->base, CHASE_REORGANIZED, h);
    }

    chaser->confirmed_height = fork_point;
    chaser->fork_point = fork_point;

    chaser_unlock(&chaser->base);

    return true;
}

/*
 * Chaser Virtual Methods
 */

static int confirm_start(chaser_t *self) {
    chaser_confirm_t *chaser = (chaser_confirm_t *)self;

    /* Get confirmed height from chainstate */
    if (chaser->chainstate != NULL) {
        chaser->confirmed_height = chainstate_get_height(chaser->chainstate);
        log_info(LOG_COMP_SYNC, "chaser_confirm: starting at height %u",
                 chaser->confirmed_height);
    } else {
        chaser->confirmed_height = 0;
    }

    return 0;
}

static bool confirm_handle_event(chaser_t *self, chase_event_t event,
                                 chase_value_t value) {
    chaser_confirm_t *chaser = (chaser_confirm_t *)self;

    if (chaser_is_closed(self)) {
        return false;
    }

    /* Stop processing during suspension */
    if (chaser_is_suspended(self)) {
        return true;
    }

    switch (event) {
    case CHASE_RESUME:
    case CHASE_START:
    case CHASE_BUMP:
        /* Check for blocks to confirm in sequence */
        {
            node_t *node = chaser->base.node;
            uint32_t confirmed = chaser_confirm_height(chaser);

            /* Try to confirm blocks in sequence */
            while (1) {
                uint32_t next_height = confirmed + 1;

                /* Try to load block at next height */
                block_t block;
                hash256_t hash;
                echo_result_t result =
                    node_load_block_at_height(node, next_height, &block, &hash);

                if (result != ECHO_OK) {
                    break; /* Block not stored/validated yet */
                }

                /* Confirm the block */
                bool bypass = chaser_confirm_is_bypass(chaser, next_height);

                if (bypass) {
                    /* Just update height for checkpoint blocks */
                    block_free(&block);
                    chaser_lock(&chaser->base);
                    chaser->confirmed_height = next_height;
                    chaser_unlock(&chaser->base);
                    chaser_notify_height(&chaser->base, CHASE_ORGANIZED,
                                         next_height);
                } else {
                    /* Pass preloaded block to avoid double-loading */
                    confirm_result_t conf_result = confirm_block_internal(
                        chaser, next_height, hash.bytes, &block, &hash);
                    block_free(&block);
                    if (conf_result != CONFIRM_SUCCESS) {
                        break; /* Confirmation failed */
                    }
                }

                chaser_set_position(self, next_height);
                confirmed = next_height;

                /*
                 * Checkpoint WAL periodically to prevent unbounded growth.
                 * During IBD, SQLite WAL grows because auto-checkpoint uses
                 * PASSIVE mode which fails when readers are present. By
                 * checkpointing inside the confirmation loop (with mutex
                 * held by block_index_db_checkpoint), we guarantee no readers.
                 */
                if (confirmed % CHECKPOINT_INTERVAL == 0) {
                    block_index_db_t *bdb = node_get_block_index_db(node);
                    if (bdb) {
                        block_index_db_checkpoint(bdb);
                        log_info(LOG_COMP_SYNC,
                                 "chaser_confirm: WAL checkpoint at height %u",
                                 confirmed);
                    }
                }
            }
        }
        break;

    case CHASE_VALID:
        /* Block has been validated */
        /* value.height is the validated block height */
        {
            uint32_t height = value.height;
            uint32_t confirmed = chaser_confirm_height(chaser);

            /* Can only confirm if all previous blocks are confirmed */
            if (height == confirmed + 1) {
                /* TODO: Get block hash from database */
                uint8_t hash[32] = {0};
                bool bypass = chaser_confirm_is_bypass(chaser, height);

                if (bypass) {
                    /* Just update height for checkpoint blocks */
                    chaser_lock(&chaser->base);
                    chaser->confirmed_height = height;
                    chaser_unlock(&chaser->base);
                    chaser_notify_height(&chaser->base, CHASE_ORGANIZED, height);
                } else {
                    chaser_confirm_block(chaser, height, hash);
                }

                chaser_set_position(self, height);

                /* Bump to check for more blocks */
                chaser_notify_height(&chaser->base, CHASE_BUMP, 0);
            }
        }
        break;

    case CHASE_REGRESSED:
    case CHASE_DISORGANIZED:
        /* Regression - may need to reorganize */
        {
            uint32_t branch_point = value.height;
            if (branch_point < chaser_confirm_height(chaser)) {
                chaser_confirm_reorganize(chaser, branch_point);
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

static void confirm_stop(chaser_t *self) {
    (void)self;
    /* Nothing special to do on stop */
}

static void confirm_destroy(chaser_t *self) {
    chaser_confirm_t *chaser = (chaser_confirm_t *)self;
    free(chaser);
}
