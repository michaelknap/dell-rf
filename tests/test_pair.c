#include "dell_rf_actions.h"
#include "fixtures/4503_actions.h"
#include "fixtures/4503_queries.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct replay {
    const struct fixture_state *before;
    const struct fixture_state *current;
    const struct fixture_state *after;
    enum drf_slot_kind kind;
    uint64_t now;

    unsigned int queries;
    unsigned int selections;
    unsigned int begins;
    unsigned int discovery_polls;
    unsigned int status_polls;
    unsigned int confirmations;
    unsigned int starts;
    unsigned int inputs;
    unsigned int drains;

    unsigned int pending;
    unsigned int fail_query;
    unsigned int confirmation_delay;
    uint8_t opaque_attribute;
    int confirmation_error;
    int selection_error;
    int status_error;

    int no_candidate;
    int stale_candidate;
    int wrong_kind_first;
    int unrelated_first;
    int malformed_candidate;
    int short_candidate;
    int endless_pending;

    int stopped;
    int stop_during_search;
    int stop_after_selection;

    int wrong_status_id;
    int bad_begin;
    int clock_error;
    int endless_drain;
};

static int query(struct replay *replay, const uint8_t request[32],
                 uint8_t response[32], size_t *length) {
    unsigned int step = replay->queries++;
    if (replay->fail_query == step + 1)
        return -ETIMEDOUT;

    const struct fixture_state *state = step < 9    ? replay->before
                                        : step < 18 ? replay->current
                                                    : replay->after;
    step %= 9;
    const uint8_t *expected, *reply;
    if (step == 0) {
        expected = fixture_metadata_request;
        reply = fixture_metadata_response;
    } else if (step == 1 || step == 8) {
        expected = state->count_request;
        reply = state->count_response;
    } else {
        expected = state->slot_requests[step - 2];
        reply = state->slot_responses[step - 2];
    }

    assert(memcmp(request, expected, 32) == 0);
    memcpy(response, reply, 32);
    *length = 32;
    return 0;
}

static int exchange(void *context, const uint8_t request[32],
                    uint8_t response[32], size_t *length) {
    struct replay *replay = context;
    if (request[1] >= 1 && request[1] <= 3)
        return query(replay, request, response, length);

    const struct fixture_action_exchange *fixture = NULL;
    int error = 0;
    switch (request[1]) {
    case DRF_PAIR_BEGIN:
        assert(replay->queries == 9 && ++replay->begins == 1);
        fixture = &fixture_pair_begin;
        break;
    case DRF_PAIR_DISCOVERY_STATUS:
        assert(replay->begins == 1 && replay->selections == 0);
        fixture = (++replay->discovery_polls % 2) ? &fixture_discovery_zero
                                                  : &fixture_discovery_one;
        break;
    case DRF_PAIR_SELECT:
        assert(replay->queries == 18 && replay->confirmations == 1);
        assert(++replay->selections == 1);
        fixture = replay->kind == DRF_SLOT_MOUSE ? &fixture_select_mouse
                                                 : &fixture_select_keyboard;
        error = replay->selection_error;
        if (replay->stop_after_selection)
            replay->stopped = 1;
        break;
    case DRF_PAIR_STATUS:
        assert(replay->selections == 1);
        replay->status_polls++;
        if (replay->pending || replay->endless_pending) {
            fixture = &fixture_pair_pending;
            if (replay->pending)
                replay->pending--;
        } else {
            fixture = replay->kind == DRF_SLOT_MOUSE ? &fixture_paired_mouse
                                                     : &fixture_paired_keyboard;
        }
        error = replay->status_error;
        break;
    default:
        assert(!"unexpected or guessed management command");
    }

    assert(fixture && memcmp(request, fixture->request, 32) == 0);
    if (error)
        return error;

    memcpy(response, fixture->response, 32);
    *length = 32;

    if (replay->bad_begin && request[1] == DRF_PAIR_BEGIN)
        response[1] = 0x0b;
    if (replay->wrong_status_id && request[1] == DRF_PAIR_STATUS &&
        response[2] == 1)
        response[6] ^= 1;
    if (request[1] == DRF_PAIR_STATUS && response[2] == 1)
        response[5] = replay->opaque_attribute;

    return 0;
}

static int now_ms(void *context, uint64_t *now) {
    struct replay *replay = context;
    if (replay->clock_error)
        return replay->clock_error;

    *now = replay->now;
    return 0;
}

static int wait_ms(void *context, unsigned int milliseconds) {
    struct replay *replay = context;
    assert(milliseconds > 0 && milliseconds <= 1000);

    replay->now += milliseconds;
    return 0;
}

static int receive(void *context, uint8_t report[DRF_REPORT_SIZE + 1],
                   size_t *length, unsigned int timeout_ms) {
    struct replay *replay = context;
    if (!timeout_ms) {
        replay->drains++;
        if ((replay->stale_candidate && replay->drains == 1) ||
            replay->endless_drain) {
            assert(replay->begins == 0);
            memcpy(report, fixture_candidate_mouse, 32);
            report[14] ^= 1; /* A stale ID must never be selected. */
            *length = 32;
            return 0;
        }
        return -EAGAIN;
    }

    assert(replay->begins == 1 && replay->selections == 0);
    replay->now += timeout_ms;
    if (replay->stop_during_search) {
        replay->stopped = 1;
        return -EINTR;
    }
    if (replay->no_candidate || replay->now < 2000)
        return -ETIMEDOUT;

    const uint8_t *candidate = replay->kind == DRF_SLOT_MOUSE
                                   ? fixture_candidate_mouse
                                   : fixture_candidate_keyboard;
    if (replay->wrong_kind_first && replay->inputs == 0)
        candidate = replay->kind == DRF_SLOT_MOUSE ? fixture_candidate_keyboard
                                                   : fixture_candidate_mouse;

    memcpy(report, candidate, 32);
    report[3] = replay->opaque_attribute;
    if (replay->unrelated_first && replay->inputs == 0)
        report[1] = 0x07;
    replay->inputs++;
    if (replay->malformed_candidate)
        report[4] = 0x1b;
    *length = replay->short_candidate ? 31 : 32;
    return 0;
}

static int confirm(void *context, enum drf_action action,
                   const struct drf_snapshot *snapshot,
                   const struct drf_slot *device, uint64_t deadline_ms) {
    struct replay *replay = context;
    assert(action == DRF_ACTION_PAIR && deadline_ms > replay->now);
    assert(replay->queries == 9 && replay->selections == 0);
    assert(snapshot->paired_count == replay->before->count);
    assert(device->kind == replay->kind && device->number == 0);

    replay->confirmations++;
    replay->now += replay->confirmation_delay;
    return replay->confirmation_error;
}

static int cancelled(void *context) {
    return ((struct replay *)context)->stopped;
}

static void started(void *context) {
    struct replay *replay = context;
    assert(replay->begins == 1 && replay->queries == 9);
    replay->starts++;
}

static struct replay fresh(enum drf_slot_kind kind) {
    const struct fixture_state *before =
        &fixture_states[kind == DRF_SLOT_MOUSE ? 2 : 3];
    return (struct replay){.before = before,
                           .current = before,
                           .after =
                               &fixture_states[kind == DRF_SLOT_MOUSE ? 3 : 4],
                           .kind = kind,
                           .opaque_attribute = 0x04,
                           .pending = kind == DRF_SLOT_MOUSE ? 1 : 3};
}

static int run(struct replay *replay, unsigned int timeout,
               struct drf_action_result *result) {
    struct drf_action_io io = {.context = replay,
                               .exchange = exchange,
                               .wait_ms = wait_ms,
                               .now_ms = now_ms,
                               .receive = receive};
    struct drf_action_callbacks callbacks = {.context = replay,
                                             .confirm = confirm,
                                             .cancelled = cancelled,
                                             .search_started = started};

    return drf_pair(&io, replay->kind, timeout, &callbacks, result);
}

static void test_success_and_rejection(void) {
    struct drf_action_result result;
    for (enum drf_slot_kind kind = DRF_SLOT_KEYBOARD; kind <= DRF_SLOT_MOUSE;
         kind++) {
        struct replay replay = fresh(kind);
        assert(run(&replay, 30000, &result) == 0);
        assert(result.verified && result.change_attempted);
        assert(result.device.kind == kind);
        assert(result.snapshot.paired_count == replay.after->count);
        assert(replay.selections == 1 && replay.begins == 1);
        assert(replay.confirmations == 1 && replay.starts == 1);
        assert(replay.status_polls == (kind == DRF_SLOT_MOUSE ? 2u : 4u));
        assert(replay.queries == 27);
    }

    const struct {
        enum drf_slot_kind kind;
        uint8_t attribute;
    } variants[] = {{DRF_SLOT_KEYBOARD, 0x07}, {DRF_SLOT_MOUSE, 0x01}};

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        struct replay replay = fresh(variants[i].kind);
        replay.opaque_attribute = variants[i].attribute;
        assert(run(&replay, 30000, &result) == 0);
        assert(result.verified && result.change_attempted);
        assert(replay.selections == 1 && replay.confirmations == 1);
    }

    struct replay replay = fresh(DRF_SLOT_MOUSE);
    replay.stale_candidate = 1;
    assert(run(&replay, 30000, &result) == 0);
    assert(replay.drains == 2 && replay.selections == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.wrong_kind_first = 1;
    assert(run(&replay, 30000, &result) == 0);
    assert(replay.inputs == 2 && result.device.kind == DRF_SLOT_MOUSE);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.unrelated_first = 1;
    assert(run(&replay, 30000, &result) == 0);
    assert(replay.inputs == 2);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.before = replay.current = &fixture_states[4];
    assert(run(&replay, 30000, &result) == -EALREADY);
    assert(result.device.number == 1);
    assert(replay.selections == 0 && replay.confirmations == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.confirmation_error = -ECANCELED;
    assert(run(&replay, 30000, &result) == -ECANCELED);
    assert(replay.selections == 0 && !result.change_attempted);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.confirmation_error = 1;
    assert(run(&replay, 30000, &result) == -EINVAL);
    assert(replay.selections == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.before = &fixture_states[1];
    replay.current = &fixture_states[3]; /* Same count, different device. */
    assert(run(&replay, 30000, &result) == -EAGAIN);
    assert(replay.selections == 0 && !result.change_attempted);

    struct fixture_state full = fixture_states[4];
    full.count = full.count_response[2] = DRF_SLOT_COUNT;
    for (size_t i = 2; i < DRF_SLOT_COUNT; i++) {
        memcpy(full.slot_responses[i], full.slot_responses[0], 32);
        full.slot_responses[i][2] = (uint8_t)(i + 1);
        full.slot_responses[i][5] = (uint8_t)i;
    }

    replay = fresh(DRF_SLOT_MOUSE);
    replay.before = &full;
    assert(run(&replay, 30000, &result) == -ENOSPC);
    assert(replay.begins == 0 && replay.selections == 0);
}

static void test_deadlines_and_signals(void) {
    struct drf_action_result result;
    struct replay replay = fresh(DRF_SLOT_MOUSE);
    replay.no_candidate = 1;
    assert(run(&replay, 30000, &result) == -ETIMEDOUT);
    assert(replay.now == 30000 && replay.selections == 0);
    assert(replay.begins == 1 && replay.confirmations == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.confirmation_delay = 30000;
    assert(run(&replay, 30000, &result) == -ETIMEDOUT);
    assert(replay.selections == 0 && !result.change_attempted);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.no_candidate = 1;
    assert(run(&replay, 17, &result) == -ETIMEDOUT);
    assert(replay.now == 17 && replay.selections == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.stopped = 1;
    assert(run(&replay, 30000, &result) == -ECANCELED);
    assert(replay.queries == 0 && replay.begins == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.stop_during_search = 1;
    assert(run(&replay, 30000, &result) == -ECANCELED);
    assert(replay.selections == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.endless_pending = 1;
    replay.after = replay.before;
    assert(run(&replay, 6000, &result) == -ETIMEDOUT);
    assert(result.change_attempted && !result.verified);
    assert(replay.selections == 1 && replay.queries == 27);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.stop_after_selection = 1;
    replay.after = replay.before;
    assert(run(&replay, 30000, &result) == -ECANCELED);
    assert(result.change_attempted && !result.verified);
    assert(replay.selections == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.stop_after_selection = 1;
    assert(run(&replay, 30000, &result) == 0);
    assert(result.verified); /* Receiver committed before local interruption. */

    replay = fresh(DRF_SLOT_MOUSE);
    replay.now = UINT64_MAX - 100;
    assert(run(&replay, 30000, &result) == -EOVERFLOW);
    assert(replay.begins == 0 && replay.selections == 0);
}

static void test_transport_failures(void) {
    struct drf_action_result result;
    for (unsigned int query = 1; query <= 27; query++) {
        struct replay replay = fresh(DRF_SLOT_MOUSE);
        replay.fail_query = query;
        assert(run(&replay, 30000, &result) == -ETIMEDOUT);
        assert(replay.selections == (query > 18));
        assert(!result.verified);
    }

    struct replay replay = fresh(DRF_SLOT_MOUSE);
    replay.selection_error = -ETIMEDOUT;
    assert(run(&replay, 30000, &result) == 0);
    assert(result.verified && result.acknowledgement_error == -ETIMEDOUT);
    assert(replay.selections == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.status_error = -EIO;
    replay.after = replay.before;
    assert(run(&replay, 30000, &result) == -EIO);
    assert(result.change_attempted && !result.verified);
    assert(replay.selections == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.wrong_status_id = 1;
    replay.after = &fixture_states[1];
    assert(run(&replay, 30000, &result) == -EBADMSG);
    assert(!result.verified && replay.selections == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.after = &fixture_states[1]; /* An unexpected keyboard, not mouse. */
    assert(run(&replay, 30000, &result) == -EAGAIN);
    assert(!result.verified);

    replay = fresh(DRF_SLOT_KEYBOARD);
    replay.after = &fixture_states[0]; /* Existing mouse moved; allowed. */
    assert(run(&replay, 30000, &result) == 0);
    assert(result.verified && result.device.number == 1);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.bad_begin = 1;
    assert(run(&replay, 30000, &result) == -EBADMSG);
    assert(replay.selections == 0 && replay.confirmations == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.malformed_candidate = 1;
    assert(run(&replay, 30000, &result) == -EBADMSG);
    assert(replay.selections == 0 && replay.confirmations == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.short_candidate = 1;
    assert(run(&replay, 30000, &result) == -EMSGSIZE);
    assert(replay.selections == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.clock_error = -EIO;
    assert(run(&replay, 30000, &result) == -EIO);
    assert(replay.begins == 0);

    replay = fresh(DRF_SLOT_MOUSE);
    replay.endless_drain = 1;
    assert(run(&replay, 30000, &result) == -EOVERFLOW);
    assert(replay.drains == 256 && replay.begins == 0);
}

int main(void) {
    test_success_and_rejection();
    test_deadlines_and_signals();
    test_transport_failures();

    puts("pair: device discovery/completion, confirmation, duplicate "
         "refusal, deadlines, signals, and uncertain outcomes passed");
    return 0;
}
