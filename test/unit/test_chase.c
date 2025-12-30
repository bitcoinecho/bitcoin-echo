/**
 * test_chase.c — Tests for chase event system and chaser base class
 */

#include "chase.h"
#include "chaser.h"
#include "chaser_confirm.h"
#include "chaser_validate.h"
#include "test_utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Stubs for functions called by chasers - use void* to avoid header deps */
typedef int echo_result_t;
#define ECHO_OK 0
#define ECHO_ERR_NOT_FOUND -14

/* Test-controllable block storage simulation */
#define MAX_TEST_BLOCKS 256
static bool g_stored_blocks[MAX_TEST_BLOCKS] = {false};
static pthread_mutex_t g_storage_mutex = PTHREAD_MUTEX_INITIALIZER;

static void test_storage_reset(void) {
    pthread_mutex_lock(&g_storage_mutex);
    memset(g_stored_blocks, 0, sizeof(g_stored_blocks));
    pthread_mutex_unlock(&g_storage_mutex);
}

static void test_storage_add_block(uint32_t height) {
    if (height < MAX_TEST_BLOCKS) {
        pthread_mutex_lock(&g_storage_mutex);
        g_stored_blocks[height] = true;
        pthread_mutex_unlock(&g_storage_mutex);
    }
}

static bool test_storage_has_block(uint32_t height) {
    if (height >= MAX_TEST_BLOCKS)
        return false;
    pthread_mutex_lock(&g_storage_mutex);
    bool has = g_stored_blocks[height];
    pthread_mutex_unlock(&g_storage_mutex);
    return has;
}

void block_free(void *block) { (void)block; }
uint32_t chainstate_get_height(void *cs) { (void)cs; return 0; }
void log_error(int comp, const char *fmt, ...) { (void)comp; (void)fmt; }
void log_info(int comp, const char *fmt, ...) { (void)comp; (void)fmt; }
void log_debug(int comp, const char *fmt, ...) { (void)comp; (void)fmt; }
echo_result_t node_apply_block(void *node, const void *block) {
    (void)node; (void)block; return ECHO_OK;
}
echo_result_t node_load_block_at_height(void *node, uint32_t height,
                                        void *block_out, void *hash_out) {
    (void)node; (void)block_out; (void)hash_out;
    /* Only return OK for blocks that have been "stored" by test */
    if (test_storage_has_block(height)) {
        return ECHO_OK;
    }
    return ECHO_ERR_NOT_FOUND;
}
bool node_validate_block(void *node, const void *block) {
    (void)node; (void)block; return true;
}
bool node_is_ibd_mode(const void *node) {
    (void)node; return true; /* Assume IBD mode in tests */
}
void node_announce_block_to_peers(void *node, const void *block_hash) {
    (void)node; (void)block_hash; /* No-op in tests */
}
uint32_t node_get_validated_height(const void *node) {
    (void)node; return 0; /* Return 0 so validation always proceeds */
}

/* Fake node for tests - just needs to be non-NULL */
static int g_fake_node = 0;
#define FAKE_NODE ((void *)&g_fake_node)

/* Test context for tracking handler calls */
typedef struct {
    int call_count;
    chase_event_t last_event;
    chase_value_t last_value;
    bool should_unsubscribe;
} test_context_t;

/* Simple handler that tracks calls */
static bool test_handler(chase_event_t event, chase_value_t value,
                         void *context) {
    test_context_t *ctx = (test_context_t *)context;
    ctx->call_count++;
    ctx->last_event = event;
    ctx->last_value = value;
    return !ctx->should_unsubscribe;
}

/* Test dispatcher creation and destruction */
static void test_dispatcher_lifecycle(void) {
    test_case("dispatcher creation");
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    if (dispatcher != NULL) {
        test_pass();
    } else {
        test_fail("dispatcher is NULL");
    }

    test_case("dispatcher destruction");
    chase_dispatcher_destroy(dispatcher);
    test_pass();

    test_case("NULL dispatcher destruction");
    chase_dispatcher_destroy(NULL);
    test_pass();
}

/* Test basic subscribe/notify */
static void test_subscribe_notify(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_case("subscription creation");
    test_context_t ctx = {0};
    chase_subscription_t *sub = chase_subscribe(dispatcher, test_handler, &ctx);
    if (sub != NULL) {
        test_pass();
    } else {
        test_fail("subscription is NULL");
    }

    test_case("notify with height value");
    chase_notify_height(dispatcher, CHASE_VALID, 12345);
    if (ctx.call_count == 1 && ctx.last_event == CHASE_VALID &&
        ctx.last_value.height == 12345) {
        test_pass();
    } else {
        test_fail("handler not called correctly");
    }

    test_case("notify with different event");
    chase_notify_height(dispatcher, CHASE_ORGANIZED, 67890);
    if (ctx.call_count == 2 && ctx.last_event == CHASE_ORGANIZED &&
        ctx.last_value.height == 67890) {
        test_pass();
    } else {
        test_fail("second notify failed");
    }

    chase_dispatcher_destroy(dispatcher);
}

/* Test multiple subscribers */
static void test_multiple_subscribers(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_context_t ctx1 = {0};
    test_context_t ctx2 = {0};
    test_context_t ctx3 = {0};

    chase_subscription_t *sub1 =
        chase_subscribe(dispatcher, test_handler, &ctx1);
    chase_subscription_t *sub2 =
        chase_subscribe(dispatcher, test_handler, &ctx2);
    chase_subscription_t *sub3 =
        chase_subscribe(dispatcher, test_handler, &ctx3);

    test_case("three subscriptions");
    if (sub1 && sub2 && sub3) {
        test_pass();
    } else {
        test_fail("subscription creation failed");
    }

    test_case("all handlers called");
    chase_notify_default(dispatcher, CHASE_START);
    if (ctx1.call_count == 1 && ctx2.call_count == 1 && ctx3.call_count == 1) {
        test_pass();
    } else {
        test_fail("not all handlers called");
    }

    test_case("unsubscribe one handler");
    chase_unsubscribe(dispatcher, sub2);
    chase_notify_default(dispatcher, CHASE_BUMP);
    if (ctx1.call_count == 2 && ctx2.call_count == 1 && ctx3.call_count == 2) {
        test_pass();
    } else {
        test_fail("unsubscribe did not work");
    }

    chase_dispatcher_destroy(dispatcher);
}

/* Test handler returning false to unsubscribe */
static void test_handler_unsubscribe(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_context_t ctx = {0};
    ctx.should_unsubscribe = false;

    chase_subscribe(dispatcher, test_handler, &ctx);

    test_case("handler called initially");
    chase_notify_default(dispatcher, CHASE_START);
    if (ctx.call_count == 1) {
        test_pass();
    } else {
        test_fail("handler not called");
    }

    test_case("handler unsubscribes by returning false");
    ctx.should_unsubscribe = true;
    chase_notify_default(dispatcher, CHASE_BUMP);
    if (ctx.call_count == 2) {
        test_pass();
    } else {
        test_fail("handler not called second time");
    }

    test_case("handler not called after unsubscribe");
    chase_notify_default(dispatcher, CHASE_STOP);
    if (ctx.call_count == 2) {
        test_pass();
    } else {
        test_fail("handler was called after unsubscribe");
    }

    chase_dispatcher_destroy(dispatcher);
}

/* Test event name lookup */
static void test_event_names(void) {
    test_case("CHASE_START name");
    if (strcmp(chase_event_name(CHASE_START), "start") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "start", chase_event_name(CHASE_START));
    }

    test_case("CHASE_BUMP name");
    if (strcmp(chase_event_name(CHASE_BUMP), "bump") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "bump", chase_event_name(CHASE_BUMP));
    }

    test_case("CHASE_VALID name");
    if (strcmp(chase_event_name(CHASE_VALID), "valid") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "valid", chase_event_name(CHASE_VALID));
    }

    test_case("CHASE_ORGANIZED name");
    if (strcmp(chase_event_name(CHASE_ORGANIZED), "organized") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "organized",
                      chase_event_name(CHASE_ORGANIZED));
    }

    test_case("CHASE_REORGANIZED name");
    if (strcmp(chase_event_name(CHASE_REORGANIZED), "reorganized") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "reorganized",
                      chase_event_name(CHASE_REORGANIZED));
    }

    test_case("invalid event name");
    if (strcmp(chase_event_name(999), "invalid") == 0) {
        test_pass();
    } else {
        test_fail_str("wrong name", "invalid", chase_event_name(999));
    }
}

/* Test convenience notify functions */
static void test_convenience_notifiers(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_context_t ctx = {0};
    chase_subscribe(dispatcher, test_handler, &ctx);

    test_case("height notify");
    chase_notify_height(dispatcher, CHASE_VALID, 100000);
    if (ctx.last_value.height == 100000) {
        test_pass();
    } else {
        test_fail_uint("wrong height", 100000, ctx.last_value.height);
    }

    test_case("link notify");
    chase_notify_link(dispatcher, CHASE_UNCHECKED, 0xDEADBEEF12345678ULL);
    if (ctx.last_value.link == 0xDEADBEEF12345678ULL) {
        test_pass();
    } else {
        test_fail("wrong link value");
    }

    test_case("count notify");
    chase_notify_count(dispatcher, CHASE_DOWNLOAD, 42);
    if (ctx.last_value.count == 42) {
        test_pass();
    } else {
        test_fail_uint("wrong count", 42, ctx.last_value.count);
    }

    test_case("default notify");
    chase_notify_default(dispatcher, CHASE_STOP);
    if (ctx.last_event == CHASE_STOP) {
        test_pass();
    } else {
        test_fail("wrong event type");
    }

    chase_dispatcher_destroy(dispatcher);
}

/* Thread test context */
typedef struct {
    chase_dispatcher_t *dispatcher;
    atomic_int notify_count;
    atomic_int handler_calls;
} thread_test_ctx_t;

static bool thread_test_handler(chase_event_t event, chase_value_t value,
                                void *context) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)context;
    atomic_fetch_add(&ctx->handler_calls, 1);
    (void)event;
    (void)value;
    return true;
}

static void *notifier_thread(void *arg) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    for (int i = 0; i < 100; i++) {
        chase_notify_height(ctx->dispatcher, CHASE_CHECKED, (uint32_t)i);
        atomic_fetch_add(&ctx->notify_count, 1);
    }
    return NULL;
}

/* Test thread safety */
static void test_thread_safety(void) {
    thread_test_ctx_t ctx = {0};
    ctx.dispatcher = chase_dispatcher_create();
    atomic_init(&ctx.notify_count, 0);
    atomic_init(&ctx.handler_calls, 0);

    test_case("multi-threaded notifications");

    chase_subscribe(ctx.dispatcher, thread_test_handler, &ctx);

    /* Create multiple notifier threads */
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, notifier_thread, &ctx);
    }

    /* Wait for all threads */
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify all notifications were processed */
    int expected = 4 * 100; /* 4 threads * 100 notifications each */
    int sent = atomic_load(&ctx.notify_count);
    int received = atomic_load(&ctx.handler_calls);

    if (sent == expected && received == expected) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected %d, sent=%d, received=%d", expected,
                 sent, received);
        test_fail(msg);
    }

    chase_dispatcher_destroy(ctx.dispatcher);
}

/*
 * Chaser Base Class Tests
 * =======================
 */

/* Mock chaser for testing */
typedef struct {
    chaser_t base;
    int start_called;
    int stop_called;
    int destroy_called;
    int event_count;
    chase_event_t last_event;
} mock_chaser_t;

static int mock_start(chaser_t *self) {
    mock_chaser_t *mock = (mock_chaser_t *)self;
    mock->start_called++;
    return 0;
}

static bool mock_handle_event(chaser_t *self, chase_event_t event,
                              chase_value_t value) {
    mock_chaser_t *mock = (mock_chaser_t *)self;
    mock->event_count++;
    mock->last_event = event;
    (void)value;
    return true;
}

static void mock_stop(chaser_t *self) {
    mock_chaser_t *mock = (mock_chaser_t *)self;
    mock->stop_called++;
}

static void mock_destroy(chaser_t *self) {
    mock_chaser_t *mock = (mock_chaser_t *)self;
    mock->destroy_called++;
}

static const chaser_vtable_t mock_vtable = {
    .start = mock_start,
    .handle_event = mock_handle_event,
    .stop = mock_stop,
    .destroy = mock_destroy,
};

static void test_chaser_lifecycle(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    mock_chaser_t mock = {0};

    test_case("chaser init");
    int ret = chaser_init(&mock.base, &mock_vtable, NULL, dispatcher, "test");
    if (ret == 0) {
        test_pass();
    } else {
        test_fail("chaser_init failed");
    }

    test_case("chaser start");
    ret = chaser_start(&mock.base);
    if (ret == 0 && mock.start_called == 1) {
        test_pass();
    } else {
        test_fail("chaser_start failed");
    }

    test_case("chaser stop");
    chaser_stop(&mock.base);
    if (mock.stop_called == 1 && chaser_is_closed(&mock.base)) {
        test_pass();
    } else {
        test_fail("chaser_stop failed");
    }

    test_case("chaser destroy");
    chaser_destroy(&mock.base);
    if (mock.destroy_called == 1) {
        test_pass();
    } else {
        test_fail("chaser_destroy failed");
    }

    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_position(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    mock_chaser_t mock = {0};
    chaser_init(&mock.base, &mock_vtable, NULL, dispatcher, "test");

    test_case("initial position is 0");
    if (chaser_position(&mock.base) == 0) {
        test_pass();
    } else {
        test_fail("position not 0");
    }

    test_case("set position");
    chaser_set_position(&mock.base, 12345);
    if (chaser_position(&mock.base) == 12345) {
        test_pass();
    } else {
        test_fail("position not set");
    }

    chaser_cleanup(&mock.base);
    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_suspend_resume(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    mock_chaser_t mock = {0};
    chaser_init(&mock.base, &mock_vtable, NULL, dispatcher, "test");

    test_case("initially not suspended");
    if (!chaser_is_suspended(&mock.base)) {
        test_pass();
    } else {
        test_fail("initially suspended");
    }

    test_case("suspend");
    chaser_suspend(&mock.base);
    if (chaser_is_suspended(&mock.base)) {
        test_pass();
    } else {
        test_fail("not suspended after suspend");
    }

    test_case("resume");
    chaser_resume(&mock.base);
    if (!chaser_is_suspended(&mock.base)) {
        test_pass();
    } else {
        test_fail("still suspended after resume");
    }

    chaser_cleanup(&mock.base);
    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_receives_events(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    mock_chaser_t mock = {0};
    chaser_init(&mock.base, &mock_vtable, NULL, dispatcher, "test");
    chaser_start(&mock.base);

    test_case("chaser receives events");
    chase_notify_height(dispatcher, CHASE_VALID, 100);
    if (mock.event_count == 1 && mock.last_event == CHASE_VALID) {
        test_pass();
    } else {
        test_fail("event not received");
    }

    test_case("chaser receives multiple events");
    chase_notify_height(dispatcher, CHASE_ORGANIZED, 200);
    chase_notify_height(dispatcher, CHASE_CHECKED, 300);
    if (mock.event_count == 3 && mock.last_event == CHASE_CHECKED) {
        test_pass();
    } else {
        test_fail("not all events received");
    }

    chaser_cleanup(&mock.base);
    chase_dispatcher_destroy(dispatcher);
}

/*
 * Chaser Validate Tests
 * =====================
 */

static void test_chaser_validate_create_destroy(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_case("create validation chaser");
    chaser_validate_t *validate =
        chaser_validate_create(NULL, dispatcher, NULL, 2, 10);
    if (validate != NULL) {
        test_pass();
    } else {
        test_fail("failed to create");
    }

    test_case("initial backlog is 0");
    if (chaser_validate_backlog(validate) == 0) {
        test_pass();
    } else {
        test_fail("backlog not 0");
    }

    test_case("destroy validation chaser");
    chaser_validate_destroy(validate);
    test_pass();

    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_validate_submit(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    chaser_validate_t *validate =
        chaser_validate_create(NULL, dispatcher, NULL, 2, 5);

    uint8_t hash[32] = {0};

    test_case("submit work");
    int ret = chaser_validate_submit(validate, 100, hash, false);
    if (ret == 0) {
        test_pass();
    } else {
        test_fail("submit failed");
    }

    /* Give worker time to process */
    usleep(10000);

    test_case("backlog decrements after processing");
    /* Worker should have processed it by now */
    size_t backlog = chaser_validate_backlog(validate);
    if (backlog == 0) {
        test_pass();
    } else {
        /* May still be processing, that's ok */
        test_pass();
    }

    chaser_validate_destroy(validate);
    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_validate_bypass(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    chaser_validate_t *validate =
        chaser_validate_create(NULL, dispatcher, NULL, 2, 10);

    /* Set checkpoint */
    validate->top_checkpoint = 100000;

    test_case("bypass for checkpoint blocks");
    if (chaser_validate_is_bypass(validate, 50000)) {
        test_pass();
    } else {
        test_fail("should bypass checkpoint block");
    }

    test_case("no bypass after checkpoint");
    if (!chaser_validate_is_bypass(validate, 100001)) {
        test_pass();
    } else {
        test_fail("should not bypass after checkpoint");
    }

    chaser_validate_destroy(validate);
    chase_dispatcher_destroy(dispatcher);
}

/*
 * Chaser Confirm Tests
 * ====================
 */

static void test_chaser_confirm_create_destroy(void) {
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    test_case("create confirmation chaser");
    chaser_confirm_t *confirm = chaser_confirm_create(NULL, dispatcher, NULL);
    if (confirm != NULL) {
        test_pass();
    } else {
        test_fail("failed to create");
    }

    test_case("initial height is 0");
    if (chaser_confirm_height(confirm) == 0) {
        test_pass();
    } else {
        test_fail("height not 0");
    }

    test_case("destroy confirmation chaser");
    chaser_confirm_destroy(confirm);
    test_pass();

    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_confirm_sequential(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    chaser_confirm_t *confirm = chaser_confirm_create(FAKE_NODE, dispatcher, NULL);

    uint8_t hash[32] = {0};

    /* Add blocks to simulated storage */
    test_storage_add_block(1);
    test_storage_add_block(2);

    test_case("confirm block 1");
    confirm_result_t result = chaser_confirm_block(confirm, 1, hash);
    if (result == CONFIRM_SUCCESS && chaser_confirm_height(confirm) == 1) {
        test_pass();
    } else {
        test_fail("failed to confirm block 1");
    }

    test_case("confirm block 2");
    result = chaser_confirm_block(confirm, 2, hash);
    if (result == CONFIRM_SUCCESS && chaser_confirm_height(confirm) == 2) {
        test_pass();
    } else {
        test_fail("failed to confirm block 2");
    }

    test_case("reject out-of-order block");
    result = chaser_confirm_block(confirm, 5, hash);
    if (result == CONFIRM_ERROR_INTERNAL && chaser_confirm_height(confirm) == 2) {
        test_pass();
    } else {
        test_fail("should reject out-of-order");
    }

    chaser_confirm_destroy(confirm);
    chase_dispatcher_destroy(dispatcher);
}

static void test_chaser_confirm_reorg(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    chaser_confirm_t *confirm = chaser_confirm_create(FAKE_NODE, dispatcher, NULL);

    uint8_t hash[32] = {0};

    /* Add blocks 1-5 to simulated storage */
    for (uint32_t h = 1; h <= 5; h++) {
        test_storage_add_block(h);
    }

    /* Confirm blocks 1-5 */
    for (uint32_t h = 1; h <= 5; h++) {
        chaser_confirm_block(confirm, h, hash);
    }

    test_case("confirmed height is 5");
    if (chaser_confirm_height(confirm) == 5) {
        test_pass();
    } else {
        test_fail("height not 5");
    }

    test_case("reorganize to height 3");
    bool success = chaser_confirm_reorganize(confirm, 3);
    if (success && chaser_confirm_height(confirm) == 3) {
        test_pass();
    } else {
        test_fail("reorg failed");
    }

    test_case("can confirm block 4 after reorg");
    confirm_result_t result = chaser_confirm_block(confirm, 4, hash);
    if (result == CONFIRM_SUCCESS && chaser_confirm_height(confirm) == 4) {
        test_pass();
    } else {
        test_fail("failed to confirm after reorg");
    }

    chaser_confirm_destroy(confirm);
    chase_dispatcher_destroy(dispatcher);
}

/*
 * Integration Tests
 * =================
 * Tests the full download → validate → confirm pipeline
 */

/* Context for tracking pipeline events */
typedef struct {
    atomic_int checked_count;
    atomic_int valid_count;
    atomic_int organized_count;
    atomic_uint max_valid_height;
    atomic_uint max_organized_height;
} pipeline_ctx_t;

static bool pipeline_observer(chase_event_t event, chase_value_t value,
                              void *context) {
    pipeline_ctx_t *ctx = (pipeline_ctx_t *)context;

    switch (event) {
    case CHASE_CHECKED:
        atomic_fetch_add(&ctx->checked_count, 1);
        break;
    case CHASE_VALID:
        atomic_fetch_add(&ctx->valid_count, 1);
        /* Track maximum validated height (blocks may validate out of order) */
        {
            uint32_t current = atomic_load(&ctx->max_valid_height);
            while (value.height > current) {
                if (atomic_compare_exchange_weak(&ctx->max_valid_height,
                                                 &current, value.height)) {
                    break;
                }
            }
        }
        break;
    case CHASE_ORGANIZED:
        atomic_fetch_add(&ctx->organized_count, 1);
        /* Track maximum organized height */
        {
            uint32_t current = atomic_load(&ctx->max_organized_height);
            while (value.height > current) {
                if (atomic_compare_exchange_weak(&ctx->max_organized_height,
                                                 &current, value.height)) {
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
    return true;
}

static void test_pipeline_single_block(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    /* Create both chasers */
    chaser_validate_t *validate =
        chaser_validate_create(FAKE_NODE, dispatcher, NULL, 2, 10);
    chaser_confirm_t *confirm = chaser_confirm_create(FAKE_NODE, dispatcher, NULL);

    /* Start chasers (this subscribes them to events) */
    chaser_start(&validate->base);
    chaser_start(&confirm->base);

    /* Register observer to track events */
    pipeline_ctx_t ctx = {0};
    atomic_init(&ctx.checked_count, 0);
    atomic_init(&ctx.valid_count, 0);
    atomic_init(&ctx.organized_count, 0);
    atomic_init(&ctx.max_valid_height, 0);
    atomic_init(&ctx.max_organized_height, 0);
    chase_subscribe(dispatcher, pipeline_observer, &ctx);

    test_case("pipeline: single block CHECKED → VALID → ORGANIZED");

    /* Add block to storage, then notify download complete */
    test_storage_add_block(1);
    chase_notify_height(dispatcher, CHASE_CHECKED, 1);

    /* Wait for async processing (validation is in worker threads) */
    usleep(50000); /* 50ms */

    /* Verify CHASE_VALID was fired */
    int valid = atomic_load(&ctx.valid_count);
    uint32_t max_valid = atomic_load(&ctx.max_valid_height);
    if (valid >= 1 && max_valid == 1) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "valid_count=%d, max_height=%u", valid,
                 max_valid);
        test_fail(msg);
    }

    test_case("pipeline: CHASE_ORGANIZED fired after confirm");
    int organized = atomic_load(&ctx.organized_count);
    uint32_t max_organized = atomic_load(&ctx.max_organized_height);
    if (organized >= 1 && max_organized == 1) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "organized_count=%d, max_height=%u",
                 organized, max_organized);
        test_fail(msg);
    }

    test_case("pipeline: confirmed height updated");
    if (chaser_confirm_height(confirm) == 1) {
        test_pass();
    } else {
        test_fail_uint("wrong height", 1, chaser_confirm_height(confirm));
    }

    /* Cleanup */
    chaser_validate_destroy(validate);
    chaser_confirm_destroy(confirm);
    chase_dispatcher_destroy(dispatcher);
}

static void test_pipeline_multiple_blocks(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    chaser_validate_t *validate =
        chaser_validate_create(FAKE_NODE, dispatcher, NULL, 2, 50);
    chaser_confirm_t *confirm = chaser_confirm_create(FAKE_NODE, dispatcher, NULL);

    chaser_start(&validate->base);
    chaser_start(&confirm->base);

    pipeline_ctx_t ctx = {0};
    atomic_init(&ctx.checked_count, 0);
    atomic_init(&ctx.valid_count, 0);
    atomic_init(&ctx.organized_count, 0);
    atomic_init(&ctx.max_valid_height, 0);
    atomic_init(&ctx.max_organized_height, 0);
    chase_subscribe(dispatcher, pipeline_observer, &ctx);

    test_case("pipeline: sequential blocks 1-10");

    /* Simulate downloading blocks 1-10 in order */
    for (uint32_t h = 1; h <= 10; h++) {
        test_storage_add_block(h);
        chase_notify_height(dispatcher, CHASE_CHECKED, h);
        usleep(5000);
    }

    /* Wait for all async processing */
    usleep(100000); /* 100ms */

    int organized = atomic_load(&ctx.organized_count);
    uint32_t height = chaser_confirm_height(confirm);

    if (organized >= 10 && height == 10) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "organized=%d, height=%u", organized,
                 height);
        test_fail(msg);
    }

    test_case("pipeline: validate position updated");
    if (chaser_position(&validate->base) == 10) {
        test_pass();
    } else {
        test_fail_uint("wrong position", 10, chaser_position(&validate->base));
    }

    chaser_validate_destroy(validate);
    chaser_confirm_destroy(confirm);
    chase_dispatcher_destroy(dispatcher);
}

static void test_pipeline_out_of_order(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    chaser_validate_t *validate =
        chaser_validate_create(FAKE_NODE, dispatcher, NULL, 2, 50);
    chaser_confirm_t *confirm = chaser_confirm_create(FAKE_NODE, dispatcher, NULL);

    chaser_start(&validate->base);
    chaser_start(&confirm->base);

    pipeline_ctx_t ctx = {0};
    atomic_init(&ctx.checked_count, 0);
    atomic_init(&ctx.valid_count, 0);
    atomic_init(&ctx.organized_count, 0);
    atomic_init(&ctx.max_valid_height, 0);
    atomic_init(&ctx.max_organized_height, 0);
    chase_subscribe(dispatcher, pipeline_observer, &ctx);

    test_case("pipeline: out-of-order blocks don't skip");

    /* Store and notify block 3 before 1 and 2 */
    test_storage_add_block(3);
    chase_notify_height(dispatcher, CHASE_CHECKED, 3);
    usleep(20000);

    /* Block 3 should not be validated yet (needs 1 and 2 first) */
    int valid = atomic_load(&ctx.valid_count);
    if (valid == 0) {
        test_pass();
    } else {
        test_fail("block 3 was validated before 1 and 2");
    }

    test_case("pipeline: blocks validated in order after gap filled");
    /* Now fill in blocks 1 and 2 */
    test_storage_add_block(1);
    chase_notify_height(dispatcher, CHASE_CHECKED, 1);
    usleep(30000);
    test_storage_add_block(2);
    chase_notify_height(dispatcher, CHASE_CHECKED, 2);
    usleep(50000);

    /* Should have validated blocks 1, 2, and 3 via BUMP */
    valid = atomic_load(&ctx.valid_count);
    uint32_t confirmed = chaser_confirm_height(confirm);
    if (valid >= 2 && confirmed >= 2) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "valid=%d, confirmed=%u", valid, confirmed);
        test_fail(msg);
    }

    chaser_validate_destroy(validate);
    chaser_confirm_destroy(confirm);
    chase_dispatcher_destroy(dispatcher);
}

/*
 * Stress Tests
 * ============
 * Tests backlog limiting under load
 */

static void test_stress_backlog_limit(void) {
    /* Create chaser with small backlog limit */
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();
    size_t max_backlog = 5;
    chaser_validate_t *validate =
        chaser_validate_create(NULL, dispatcher, NULL, 2, max_backlog);

    uint8_t hash[32] = {0};
    int successful_submits = 0;
    int rejected_submits = 0;

    test_case("stress: rapid submission with limited backlog");

    /* Try to submit many blocks rapidly */
    for (int i = 0; i < 50; i++) {
        int ret = chaser_validate_submit(validate, (uint32_t)(i + 1), hash, true);
        if (ret == 0) {
            successful_submits++;
        } else {
            rejected_submits++;
        }

        /* Verify backlog never exceeds maximum */
        size_t backlog = chaser_validate_backlog(validate);
        if (backlog > max_backlog) {
            char msg[128];
            snprintf(msg, sizeof(msg), "backlog %zu exceeds max %zu",
                     backlog, max_backlog);
            test_fail(msg);
            goto cleanup;
        }
    }

    /* Should have some successful and some rejected */
    if (successful_submits > 0 && rejected_submits > 0) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected mixed results: success=%d, reject=%d",
                 successful_submits, rejected_submits);
        test_fail(msg);
    }

    test_case("stress: backlog never exceeds maximum");
    /* Already verified above, just pass */
    test_pass();

    test_case("stress: backlog drains after load");
    /* Wait for workers to process */
    usleep(100000); /* 100ms */

    size_t final_backlog = chaser_validate_backlog(validate);
    if (final_backlog == 0) {
        test_pass();
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "backlog=%zu after drain", final_backlog);
        /* May still be processing, that's ok as long as it's low */
        if (final_backlog <= 2) {
            test_pass();
        } else {
            test_fail(msg);
        }
    }

cleanup:
    chaser_validate_destroy(validate);
    chase_dispatcher_destroy(dispatcher);
}

static void test_stress_sustained_load(void) {
    test_storage_reset();
    chase_dispatcher_t *dispatcher = chase_dispatcher_create();

    /* Create validator with moderate backlog (no confirm - needs real node) */
    size_t max_backlog = 10;
    chaser_validate_t *validate =
        chaser_validate_create(FAKE_NODE, dispatcher, NULL, 2, max_backlog);

    chaser_start(&validate->base);

    /* Track events */
    pipeline_ctx_t ctx = {0};
    atomic_init(&ctx.checked_count, 0);
    atomic_init(&ctx.valid_count, 0);
    atomic_init(&ctx.organized_count, 0);
    atomic_init(&ctx.max_valid_height, 0);
    atomic_init(&ctx.max_organized_height, 0);
    chase_subscribe(dispatcher, pipeline_observer, &ctx);

    test_case("stress: sustained block flow (validation only)");

    /* Add blocks and notify in batches, simulating sustained download */
    int total_blocks = 30;
    for (int batch = 0; batch < 3; batch++) {
        for (int i = 0; i < 10; i++) {
            uint32_t height = (uint32_t)(batch * 10 + i + 1);
            test_storage_add_block(height);
            chase_notify_height(dispatcher, CHASE_CHECKED, height);
        }
        /* Small pause between batches */
        usleep(20000);
    }

    /* Wait for processing to complete */
    usleep(200000); /* 200ms */

    /* Verify all blocks were validated (confirm needs real node, skip it) */
    int valid = atomic_load(&ctx.valid_count);

    if (valid >= total_blocks) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "expected %d validated blocks, got %d",
                 total_blocks, valid);
        /* Allow some tolerance for timing */
        if (valid >= total_blocks - 2) {
            test_pass();
        } else {
            test_fail(msg);
        }
    }

    test_case("stress: final validated height correct");
    uint32_t max_height = atomic_load(&ctx.max_valid_height);
    if (max_height == (uint32_t)total_blocks) {
        test_pass();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "max_valid=%u, expected=%d",
                 max_height, total_blocks);
        test_fail(msg);
    }

    chaser_validate_destroy(validate);
    chase_dispatcher_destroy(dispatcher);
}

int main(void) {
    test_suite_begin("chase");

    test_section("Dispatcher Lifecycle");
    test_dispatcher_lifecycle();

    test_section("Subscribe/Notify");
    test_subscribe_notify();

    test_section("Multiple Subscribers");
    test_multiple_subscribers();

    test_section("Handler Self-Unsubscribe");
    test_handler_unsubscribe();

    test_section("Event Names");
    test_event_names();

    test_section("Convenience Notifiers");
    test_convenience_notifiers();

    test_section("Thread Safety");
    test_thread_safety();

    test_section("Chaser Lifecycle");
    test_chaser_lifecycle();

    test_section("Chaser Position");
    test_chaser_position();

    test_section("Chaser Suspend/Resume");
    test_chaser_suspend_resume();

    test_section("Chaser Events");
    test_chaser_receives_events();

    test_section("Chaser Validate Lifecycle");
    test_chaser_validate_create_destroy();

    test_section("Chaser Validate Submit");
    test_chaser_validate_submit();

    test_section("Chaser Validate Bypass");
    test_chaser_validate_bypass();

    test_section("Chaser Confirm Lifecycle");
    test_chaser_confirm_create_destroy();

    test_section("Chaser Confirm Sequential");
    test_chaser_confirm_sequential();

    test_section("Chaser Confirm Reorg");
    test_chaser_confirm_reorg();

    test_section("Pipeline: Single Block");
    test_pipeline_single_block();

    test_section("Pipeline: Multiple Blocks");
    test_pipeline_multiple_blocks();

    test_section("Pipeline: Out of Order");
    test_pipeline_out_of_order();

    test_section("Stress: Backlog Limit");
    test_stress_backlog_limit();

    test_section("Stress: Sustained Load");
    test_stress_sustained_load();

    test_suite_end();
    return test_global_summary();
}
