#include "dell_rf_actions.h"

#include <errno.h>
#include <string.h>

static int same_device(const struct drf_slot *a, const struct drf_slot *b) {
    return a->kind == b->kind &&
           memcmp(a->opaque_id, b->opaque_id, sizeof(a->opaque_id)) == 0 &&
           strcmp(a->model, b->model) == 0;
}

static int same_receiver(const struct drf_snapshot *a,
                         const struct drf_snapshot *b) {
    return strcmp(a->receiver.name, b->receiver.name) == 0 &&
           memcmp(a->receiver.opaque_version, b->receiver.opaque_version,
                  sizeof(a->receiver.opaque_version)) == 0;
}

static int same_snapshot(const struct drf_snapshot *a,
                         const struct drf_snapshot *b) {
    if (!same_receiver(a, b) || a->paired_count != b->paired_count)
        return 0;

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        if (a->slots[i].number != b->slots[i].number ||
            !same_device(&a->slots[i], &b->slots[i]))
            return 0;
    }

    return 1;
}

static int validate_identities(const struct drf_snapshot *snapshot) {
    static const uint8_t zero[3] = {0}, ones[3] = {0xff, 0xff, 0xff};

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        const struct drf_slot *device = &snapshot->slots[i];
        if (device->kind == DRF_SLOT_EMPTY)
            continue;
        if (memcmp(device->opaque_id, zero, 3) == 0 ||
            memcmp(device->opaque_id, ones, 3) == 0)
            return -EBADMSG;

        for (size_t j = 0; j < i; j++) {
            if (snapshot->slots[j].kind != DRF_SLOT_EMPTY &&
                memcmp(device->opaque_id, snapshot->slots[j].opaque_id, 3) == 0)
                return -EBADMSG;
        }
    }

    return 0;
}

static int cancelled(const struct drf_action_callbacks *callbacks) {
    return callbacks->cancelled && callbacks->cancelled(callbacks->context);
}

static int removed_only(const struct drf_snapshot *before,
                        const struct drf_snapshot *after,
                        const struct drf_slot *removed) {
    if (!same_receiver(before, after) ||
        after->paired_count + 1 != before->paired_count ||
        validate_identities(after))
        return 0;

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        const struct drf_slot *device = &after->slots[i];
        if (device->kind == DRF_SLOT_EMPTY)
            continue;
        if (memcmp(device->opaque_id, removed->opaque_id, 3) == 0)
            return 0;

        int found = 0;
        for (size_t j = 0; j < DRF_SLOT_COUNT; j++) {
            if (same_device(device, &before->slots[j]))
                found = 1;
        }
        if (!found)
            return 0;
    }

    return 1;
}

int drf_unpair(const struct drf_action_io *io, unsigned int slot,
               const struct drf_action_callbacks *callbacks,
               struct drf_action_result *result) {
    if (!result)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    if (!io || !io->exchange || !io->wait_ms || !callbacks ||
        !callbacks->confirm || slot < 1 || slot > DRF_SLOT_COUNT)
        return -EINVAL;
    if (cancelled(callbacks))
        return -ECANCELED;

    struct drf_snapshot before, current, after;
    int r = drf_read_snapshot(io->exchange, io->context, &before);
    if (r)
        return r;
    if ((r = validate_identities(&before)))
        return r;

    result->device = before.slots[slot - 1];
    if (result->device.kind == DRF_SLOT_EMPTY)
        return -ENOENT;

    r = callbacks->confirm(callbacks->context, DRF_ACTION_UNPAIR, &before,
                           &result->device, 0);
    if (r)
        return r < 0 ? r : -EINVAL;
    if (cancelled(callbacks))
        return -ECANCELED;

    r = drf_read_snapshot(io->exchange, io->context, &current);
    if (r)
        return r;
    if (!same_snapshot(&before, &current))
        return -EAGAIN;
    if (cancelled(callbacks))
        return -ECANCELED;

    uint8_t request[DRF_REPORT_SIZE], response[DRF_REPORT_SIZE] = {0};
    r = drf_encode_unpair(slot, request);
    if (r)
        return r;

    size_t length = sizeof(response);
    result->change_attempted = 1;
    r = io->exchange(io->context, request, response, &length);
    if (!r)
        r = drf_decode_unpair_ack(response, length);
    result->acknowledgement_error = r;

    /* DPM waits approximately one second before re-reading slots. Never
     * repeat a removal, even if its acknowledgement is lost. Readback may
     * still prove that the requested removal succeeded. */
    int wait_error = io->wait_ms(io->context, 1000);
    if (wait_error && wait_error != -EINTR)
        return r ? r : wait_error;

    int verify_error = drf_read_snapshot(io->exchange, io->context, &after);
    if (verify_error)
        return r ? r : verify_error;
    if (!removed_only(&before, &after, &result->device))
        return r ? r : -EAGAIN;

    result->snapshot = after;
    result->verified = 1;
    return 0;
}

static int pair_exchange(const struct drf_action_io *io,
                         enum drf_pair_command command, const uint8_t id[3],
                         uint8_t response[DRF_REPORT_SIZE], size_t *length) {
    uint8_t request[DRF_REPORT_SIZE];
    int r = drf_encode_pair_command(command, id, request);
    if (r)
        return r;

    memset(response, 0, DRF_REPORT_SIZE);
    *length = DRF_REPORT_SIZE;
    return io->exchange(io->context, request, response, length);
}

static int check_window(const struct drf_action_io *io,
                        const struct drf_action_callbacks *callbacks,
                        uint64_t deadline, uint64_t *now) {
    if (cancelled(callbacks))
        return -ECANCELED;

    int r = io->now_ms(io->context, now);
    if (r)
        return r;

    return *now >= deadline ? -ETIMEDOUT : 0;
}

static unsigned int poll_interval(uint64_t remaining) {
    return remaining < 1000 ? (unsigned int)remaining : 1000;
}

static int find_candidate(const struct drf_action_io *io,
                          enum drf_slot_kind kind,
                          const struct drf_snapshot *before,
                          const struct drf_action_callbacks *callbacks,
                          uint64_t deadline, struct drf_slot *out) {
    uint64_t now;
    int r = check_window(io, callbacks, deadline, &now);
    if (r)
        return r;

    uint64_t next_poll = now + poll_interval(deadline - now);
    for (;;) {
        r = check_window(io, callbacks, deadline, &now);
        if (r)
            return r;

        if (now >= next_poll) {
            uint8_t response[DRF_REPORT_SIZE];
            size_t length;
            r = pair_exchange(io, DRF_PAIR_DISCOVERY_STATUS, NULL, response,
                              &length);
            if (!r)
                r = drf_decode_pair_ack(DRF_PAIR_DISCOVERY_STATUS, response,
                                        length);
            if (r)
                return r;

            r = check_window(io, callbacks, deadline, &now);
            if (r)
                return r;
            next_poll = now + poll_interval(deadline - now);
        }

        uint8_t input[DRF_REPORT_SIZE + 1];
        size_t length = sizeof(input);
        r = io->receive(io->context, input, &length,
                        (unsigned int)(next_poll - now));
        if (r == -EAGAIN || r == -ETIMEDOUT || r == -EINTR)
            continue;
        if (r)
            return r;

        struct drf_slot candidate;
        r = drf_decode_candidate(input, length, &candidate);
        if (r == -ENOMSG)
            continue;
        if (r)
            return r;
        if (candidate.kind != kind)
            continue;

        *out = candidate;
        for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
            if (before->slots[i].kind != DRF_SLOT_EMPTY &&
                memcmp(before->slots[i].opaque_id, candidate.opaque_id, 3) ==
                    0) {
                *out = before->slots[i];
                return -EALREADY;
            }
        }

        return 0;
    }
}

static int added_only(const struct drf_snapshot *before,
                      const struct drf_snapshot *after,
                      const struct drf_slot *added, struct drf_slot *out) {
    if (!same_receiver(before, after) ||
        after->paired_count != before->paired_count + 1 ||
        validate_identities(after))
        return 0;

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        const struct drf_slot *device = &before->slots[i];
        if (device->kind == DRF_SLOT_EMPTY)
            continue;

        int found = 0;
        for (size_t j = 0; j < DRF_SLOT_COUNT; j++) {
            if (same_device(device, &after->slots[j]))
                found = 1;
        }
        if (!found)
            return 0;
    }

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        if (same_device(added, &after->slots[i])) {
            *out = after->slots[i];
            return 1;
        }
    }

    return 0;
}

static int verify_pair(const struct drf_action_io *io,
                       const struct drf_snapshot *before,
                       struct drf_action_result *result) {
    int r = io->wait_ms(io->context, 1000);
    if (r && r != -EINTR)
        return r;

    struct drf_snapshot after;
    r = drf_read_snapshot(io->exchange, io->context, &after);
    if (r)
        return r;

    struct drf_slot paired;
    if (!added_only(before, &after, &result->device, &paired))
        return -EAGAIN;

    result->device = paired;
    result->snapshot = after;
    result->verified = 1;
    return 0;
}

static int finish_pair(const struct drf_action_io *io,
                       const struct drf_snapshot *before,
                       const struct drf_action_callbacks *callbacks,
                       uint64_t deadline, struct drf_action_result *result) {
    int r;
    for (;;) {
        uint64_t now;
        r = check_window(io, callbacks, deadline, &now);
        if (r)
            break;

        r = io->wait_ms(io->context, poll_interval(deadline - now));
        if (r == -EINTR)
            continue;
        if (r)
            break;

        r = check_window(io, callbacks, deadline, &now);
        if (r)
            break;

        uint8_t response[DRF_REPORT_SIZE];
        size_t length;
        r = pair_exchange(io, DRF_PAIR_STATUS, NULL, response, &length);
        if (r)
            break;

        struct drf_slot paired;
        r = drf_decode_pair_status(response, length, &paired);
        if (r == -EAGAIN)
            continue;
        if (!r && !same_device(&paired, &result->device))
            r = -EBADMSG;
        if (r)
            break;

        result->device = paired;
        break;
    }

    /* Selection is a commit point. Cancellation cannot retract it. Even if
     * an ACK, status read, or deadline fails, try read-only verification. */
    int verify_error = verify_pair(io, before, result);
    return verify_error ? (r ? r : verify_error) : 0;
}

int drf_pair(const struct drf_action_io *io, enum drf_slot_kind kind,
             unsigned int timeout_ms,
             const struct drf_action_callbacks *callbacks,
             struct drf_action_result *result) {
    if (!result)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    if (!io || !io->exchange || !io->wait_ms || !io->receive || !io->now_ms ||
        !callbacks || !callbacks->confirm || !timeout_ms ||
        (kind != DRF_SLOT_MOUSE && kind != DRF_SLOT_KEYBOARD))
        return -EINVAL;
    if (cancelled(callbacks))
        return -ECANCELED;

    struct drf_snapshot before, current;
    int r = drf_read_snapshot(io->exchange, io->context, &before);
    if (r)
        return r;
    if ((r = validate_identities(&before)))
        return r;
    if (before.paired_count == DRF_SLOT_COUNT)
        return -ENOSPC;

    /* Drop input queued during preflight. Never select a stale notification.
     * Bound the drain so a noisy or faulty receiver cannot starve the CLI. */
    for (unsigned int i = 0;; i++) {
        if (cancelled(callbacks))
            return -ECANCELED;
        if (i == 256)
            return -EOVERFLOW;

        uint8_t input[DRF_REPORT_SIZE + 1];
        size_t length = sizeof(input);
        r = io->receive(io->context, input, &length, 0);
        if (r == -EAGAIN || r == -ETIMEDOUT)
            break;
        if (r)
            return r;
    }

    uint64_t now;
    r = io->now_ms(io->context, &now);
    if (r)
        return r;
    if (now > UINT64_MAX - timeout_ms)
        return -EOVERFLOW;
    uint64_t deadline = now + timeout_ms;

    uint8_t response[DRF_REPORT_SIZE];
    size_t length;
    r = pair_exchange(io, DRF_PAIR_BEGIN, NULL, response, &length);
    if (!r)
        r = drf_decode_pair_ack(DRF_PAIR_BEGIN, response, length);
    if (r)
        return r;
    if (callbacks->search_started)
        callbacks->search_started(callbacks->context);

    r = find_candidate(io, kind, &before, callbacks, deadline, &result->device);
    if (r)
        return r;
    r = check_window(io, callbacks, deadline, &now);
    if (r)
        return r;

    r = callbacks->confirm(callbacks->context, DRF_ACTION_PAIR, &before,
                           &result->device, deadline);
    if (r)
        return r < 0 ? r : -EINVAL;

    r = check_window(io, callbacks, deadline, &now);
    if (r)
        return r;
    r = drf_read_snapshot(io->exchange, io->context, &current);
    if (r)
        return r;
    if (!same_snapshot(&before, &current))
        return -EAGAIN;
    r = check_window(io, callbacks, deadline, &now);
    if (r)
        return r;

    result->change_attempted = 1;
    r = pair_exchange(io, DRF_PAIR_SELECT, result->device.opaque_id, response,
                      &length);
    if (!r)
        r = drf_decode_pair_ack(DRF_PAIR_SELECT, response, length);
    result->acknowledgement_error = r;

    return finish_pair(io, &before, callbacks, deadline, result);
}
