#include "dell_rf_actions.h"
#include "fixtures/4503_actions.h"
#include "fixtures/4503_queries.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void test_action_codec(void) {
    uint8_t report[DRF_REPORT_SIZE];
    assert(drf_encode_unpair(2, report) == 0);
    assert(memcmp(report, fixture_unpair_mouse.request, sizeof(report)) == 0);

    assert(drf_encode_unpair(1, report) == 0);
    assert(memcmp(report, fixture_unpair_keyboard.request, sizeof(report)) ==
           0);

    assert(drf_decode_unpair_ack(fixture_unpair_mouse.response, 32) == 0);
    assert(drf_decode_unpair_ack(fixture_unpair_keyboard.response, 32) == 0);

    assert(drf_encode_unpair(0, report) == -EINVAL);
    assert(drf_encode_unpair(7, report) == -EINVAL);

    memcpy(report, fixture_unpair_mouse.response, 32);
    report[2] = 1;
    assert(drf_decode_unpair_ack(report, 32) == -EBADMSG);
    assert(drf_decode_unpair_ack(report, 31) == -EMSGSIZE);

    struct {
        enum drf_pair_command command;
        const struct fixture_action_exchange *fixture;
    } cases[] = {
        {DRF_PAIR_BEGIN, &fixture_pair_begin},
        {DRF_PAIR_DISCOVERY_STATUS, &fixture_discovery_zero},
        {DRF_PAIR_DISCOVERY_STATUS, &fixture_discovery_one},
        {DRF_PAIR_SELECT, &fixture_select_mouse},
        {DRF_PAIR_SELECT, &fixture_select_keyboard},
        {DRF_PAIR_STATUS, &fixture_pair_pending},
        {DRF_PAIR_STATUS, &fixture_paired_mouse},
        {DRF_PAIR_STATUS, &fixture_paired_keyboard},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const uint8_t *id = cases[i].command == DRF_PAIR_SELECT
                                ? cases[i].fixture->request + 3
                                : NULL;
        assert(drf_encode_pair_command(cases[i].command, id, report) == 0);
        assert(memcmp(report, cases[i].fixture->request, 32) == 0);

        if (cases[i].command != DRF_PAIR_STATUS)
            assert(drf_decode_pair_ack(cases[i].command,
                                       cases[i].fixture->response, 32) == 0);
    }

    assert(drf_encode_pair_command((enum drf_pair_command)6, NULL, report) ==
           -EINVAL);
    assert(drf_encode_pair_command(DRF_PAIR_SELECT, NULL, report) == -EINVAL);

    const uint8_t zero[3] = {0}, ones[3] = {0xff, 0xff, 0xff};
    assert(drf_encode_pair_command(DRF_PAIR_SELECT, zero, report) == -EINVAL);
    assert(drf_encode_pair_command(DRF_PAIR_SELECT, ones, report) == -EINVAL);
    assert(drf_encode_pair_command(DRF_PAIR_BEGIN, zero, report) == -EINVAL);

    struct drf_slot device, original;
    memset(&original, 0x55, sizeof(original));
    device = original;
    assert(drf_decode_candidate(fixture_candidate_mouse, 32, &device) == 0);
    assert(device.kind == DRF_SLOT_MOUSE && device.number == 0);
    assert(strcmp(device.model, "MS3121W") == 0);
    assert(memcmp(device.opaque_id, fixture_select_mouse.request + 3, 3) == 0);

    assert(drf_decode_candidate(fixture_candidate_keyboard, 32, &device) == 0);
    assert(device.kind == DRF_SLOT_KEYBOARD && device.number == 0);
    assert(strcmp(device.model, "KB3121W") == 0);

    assert(drf_decode_pair_status(fixture_paired_mouse.response, 32, &device) ==
           0);
    assert(device.number == 1 && device.kind == DRF_SLOT_MOUSE);

    assert(drf_decode_pair_status(fixture_paired_keyboard.response, 32,
                                  &device) == 0);
    assert(device.number == 2 && device.kind == DRF_SLOT_KEYBOARD);

    device = original;
    assert(drf_decode_pair_status(fixture_pair_pending.response, 32, &device) ==
           -EAGAIN);
    assert(memcmp(&device, &original, sizeof(device)) == 0);

    for (unsigned int failure = 0; failure < 8; failure++) {
        memcpy(report, fixture_candidate_mouse, 32);
        switch (failure) {
        case 0:
            report[0] = 0x08;
            break;
        case 1:
            report[2] = 0x03;
            break;
        case 2:
            report[3] = 0x05;
            break;
        case 3:
            report[4] = 0x1b;
            break;
        case 4:
            report[13] = 'X';
            break;
        case 5:
            memset(report + 14, 0, 3);
            break;
        case 6:
            memset(report + 14, 0xff, 3);
            break;
        case 7:
            report[31] = 1;
            break;
        }

        assert(drf_decode_candidate(report, 32, &device) == -EBADMSG);
        assert(memcmp(&device, &original, sizeof(device)) == 0);
    }

    memcpy(report, fixture_candidate_mouse, 32);
    report[1] = 7;
    assert(drf_decode_candidate(report, 32, &device) == -ENOMSG);
    assert(drf_decode_candidate(report, 33, &device) == -EMSGSIZE);

    memcpy(report, fixture_paired_mouse.response, 32);
    report[6] ^= 1; /* Structurally valid, but caller must match selected ID. */
    assert(drf_decode_pair_status(report, 32, &device) == 0);

    report[3] = 7;
    assert(drf_decode_pair_status(report, 32, &device) == -EBADMSG);

    memcpy(report, fixture_select_keyboard.response, 32);
    report[2] = 2;
    assert(drf_decode_pair_ack(DRF_PAIR_SELECT, report, 32) == -EBADMSG);

    memcpy(report, fixture_pair_begin.response, 32);
    report[1] = 0x0b;
    assert(drf_decode_pair_ack(DRF_PAIR_BEGIN, report, 32) == -EBADMSG);
}

struct replay {
    const struct fixture_state *before;
    const struct fixture_state *current;
    const struct fixture_state *after;
    unsigned int slot;

    unsigned int queries;
    unsigned int mutations;
    unsigned int confirmations;
    unsigned int waits;

    unsigned int fail_query;
    int confirmation_error;
    int acknowledgement_error;
    int stopped;
    int stop_on_confirmation;
    int invalid_identity;
};

static int exchange(void *context, const uint8_t request[DRF_REPORT_SIZE],
                    uint8_t response[DRF_REPORT_SIZE], size_t *length) {
    struct replay *replay = context;
    if (request[1] == 6) {
        assert(request[2] == replay->slot && replay->queries == 18);
        assert(++replay->mutations == 1);

        uint8_t expected[32];
        assert(drf_encode_unpair(replay->slot, expected) == 0);
        assert(memcmp(request, expected, 32) == 0);

        if (replay->acknowledgement_error)
            return replay->acknowledgement_error;

        memcpy(response, fixture_unpair_mouse.response, 32);
        *length = 32;
        return 0;
    }

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
    if (replay->invalid_identity && request[1] == 3 && request[2] == 1)
        memset(response + 5, 0, 3);
    return 0;
}

static int confirm(void *context, enum drf_action action,
                   const struct drf_snapshot *snapshot,
                   const struct drf_slot *device, uint64_t deadline) {
    struct replay *replay = context;
    assert(action == DRF_ACTION_UNPAIR && deadline == 0);
    assert(replay->queries == 9 && replay->mutations == 0);
    assert(snapshot->paired_count == replay->before->count);
    assert(device->number == replay->slot && device->kind != DRF_SLOT_EMPTY);

    replay->confirmations++;
    if (replay->stop_on_confirmation)
        replay->stopped = 1;
    return replay->confirmation_error;
}

static int is_cancelled(void *context) {
    return ((struct replay *)context)->stopped;
}

static int wait_ms(void *context, unsigned int milliseconds) {
    struct replay *replay = context;
    assert(milliseconds == 1000 && replay->mutations == 1);
    replay->waits++;
    return 0;
}

static struct replay fresh(void) {
    return (struct replay){.before = &fixture_states[0],
                           .current = &fixture_states[0],
                           .after = &fixture_states[1],
                           .slot = 2};
}

static int run(struct replay *replay, struct drf_action_result *result) {
    struct drf_action_io io = {
        .context = replay, .exchange = exchange, .wait_ms = wait_ms};
    struct drf_action_callbacks callbacks = {
        .context = replay, .confirm = confirm, .cancelled = is_cancelled};

    return drf_unpair(&io, replay->slot, &callbacks, result);
}

static void test_removal_transactions(void) {
    struct drf_action_result result;
    struct replay replay = fresh();
    assert(run(&replay, &result) == 0);
    assert(result.verified && result.change_attempted);
    assert(result.device.kind == DRF_SLOT_MOUSE);
    assert(result.snapshot.paired_count == 1);
    assert(replay.queries == 27 && replay.mutations == 1);
    assert(replay.confirmations == 1 && replay.waits == 1);

    replay = (struct replay){.before = &fixture_states[1],
                             .current = &fixture_states[1],
                             .after = &fixture_states[2],
                             .slot = 1};
    assert(run(&replay, &result) == 0);
    assert(result.verified && result.snapshot.paired_count == 0);

    replay = fresh();
    replay.confirmation_error = -ECANCELED;
    assert(run(&replay, &result) == -ECANCELED);
    assert(replay.queries == 9 && replay.mutations == 0);
    assert(!result.change_attempted && !result.verified);

    replay = fresh();
    replay.stop_on_confirmation = 1;
    assert(run(&replay, &result) == -ECANCELED);
    assert(replay.mutations == 0);

    replay = fresh();
    replay.stopped = 1;
    assert(run(&replay, &result) == -ECANCELED);
    assert(replay.queries == 0 && replay.mutations == 0);

    replay = fresh();
    replay.slot = 3;
    assert(run(&replay, &result) == -ENOENT);
    assert(replay.mutations == 0 && replay.confirmations == 0);

    replay = fresh();
    replay.current = &fixture_states[4]; /* Same count, swapped devices. */
    assert(run(&replay, &result) == -EAGAIN);
    assert(replay.mutations == 0 && !result.change_attempted);

    replay = fresh();
    replay.invalid_identity = 1;
    assert(run(&replay, &result) == -EBADMSG);
    assert(replay.mutations == 0 && replay.confirmations == 0);

    for (unsigned int query = 1; query <= 27; query++) {
        replay = fresh();
        replay.fail_query = query;
        assert(run(&replay, &result) == -ETIMEDOUT);
        assert(replay.mutations == (query > 18));
        assert(!result.verified);
    }

    replay = fresh();
    replay.acknowledgement_error = -ETIMEDOUT;
    assert(run(&replay, &result) == 0);
    assert(result.verified && result.acknowledgement_error == -ETIMEDOUT);
    assert(replay.mutations == 1);

    replay = fresh();
    replay.after = replay.before;
    assert(run(&replay, &result) == -EAGAIN);
    assert(replay.mutations == 1 && result.change_attempted &&
           !result.verified);

    replay = fresh();
    replay.after = &fixture_states[3]; /* Wrong device was removed. */
    assert(run(&replay, &result) == -EAGAIN);
    assert(replay.mutations == 1 && !result.verified);
}

int main(void) {
    test_action_codec();
    test_removal_transactions();

    puts("actions: fixture codecs, confirmed removal, stale targets, "
         "lost acknowledgements, and no mutation retries passed");
    return 0;
}
