#include "dell_rf_protocol.h"
#include "fixtures/4503_queries.h"

#include <assert.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

static void test_fixture_queries(void) {
    uint8_t request[DRF_REPORT_SIZE];
    assert(drf_encode_query(DRF_QUERY_RECEIVER, 0, request) == 0);
    assert(memcmp(request, fixture_metadata_request, sizeof(request)) == 0);

    struct drf_receiver_metadata metadata;
    assert(drf_decode_receiver(fixture_metadata_response, DRF_REPORT_SIZE,
                               &metadata) == 0);
    assert(strcmp(metadata.name, "Dell Universal Receiver") == 0);

    for (size_t state = 0;
         state < sizeof(fixture_states) / sizeof(fixture_states[0]); state++) {
        const struct fixture_state *fixture = &fixture_states[state];
        assert(drf_encode_query(DRF_QUERY_COUNT, 0, request) == 0);
        assert(memcmp(request, fixture->count_request, sizeof(request)) == 0);

        unsigned int count = 99;
        assert(drf_decode_count(fixture->count_response, DRF_REPORT_SIZE,
                                &count) == 0);
        assert(count == fixture->count);

        unsigned int occupied = 0;
        for (unsigned int slot = 1; slot <= DRF_SLOT_COUNT; slot++) {
            assert(drf_encode_query(DRF_QUERY_SLOT, slot, request) == 0);
            assert(memcmp(request, fixture->slot_requests[slot - 1],
                          sizeof(request)) == 0);

            struct drf_slot decoded;
            assert(drf_decode_slot(fixture->slot_responses[slot - 1],
                                   DRF_REPORT_SIZE, slot, &decoded) == 0);
            assert(decoded.number == slot);
            if (decoded.kind == DRF_SLOT_KEYBOARD) {
                assert(strcmp(decoded.model, "KB3121W") == 0);
                assert(decoded.capabilities ==
                       fixture->slot_responses[slot - 1][30]);
                occupied++;
            } else if (decoded.kind == DRF_SLOT_MOUSE) {
                assert(strcmp(decoded.model, "MS3121W") == 0);
                assert(decoded.capabilities ==
                       fixture->slot_responses[slot - 1][30]);
                occupied++;
            } else {
                assert(decoded.kind == DRF_SLOT_EMPTY);
                assert(decoded.model[0] == '\0');
            }
        }

        assert(occupied == fixture->count);
    }
}

static void test_battery_codec(void) {
    uint8_t report[DRF_REPORT_SIZE];
    unsigned int percentage = 1234;

    for (unsigned int slot = 1; slot <= 2; slot++) {
        assert(drf_encode_query(DRF_QUERY_BATTERY, slot, report) == 0);
        assert(memcmp(report, fixture_battery_requests[slot - 1],
                      sizeof(report)) == 0);
    }

    assert(drf_decode_battery(fixture_battery_responses[0], DRF_REPORT_SIZE, 1,
                              &percentage) == 0);
    assert(percentage == 73);
    assert(drf_decode_battery(fixture_battery_responses[1], DRF_REPORT_SIZE, 2,
                              &percentage) == 0);
    assert(percentage == DRF_BATTERY_UNKNOWN);

    memcpy(report, fixture_battery_responses[0], sizeof(report));
    report[3] = 0;
    assert(drf_decode_battery(report, sizeof(report), 1, &percentage) == 0);
    assert(percentage == 0);
    report[3] = 100;
    assert(drf_decode_battery(report, sizeof(report), 1, &percentage) == 0);
    assert(percentage == 100);

    percentage = 1234;
    assert(drf_decode_battery(report, sizeof(report) - 1, 1, &percentage) ==
           -EMSGSIZE);
    report[0] ^= 1;
    assert(drf_decode_battery(report, sizeof(report), 1, &percentage) ==
           -EBADMSG);
    report[0] ^= 1;
    report[1] = DRF_QUERY_SLOT;
    assert(drf_decode_battery(report, sizeof(report), 1, &percentage) ==
           -EBADMSG);
    report[1] = DRF_QUERY_BATTERY;
    report[2] = 2;
    assert(drf_decode_battery(report, sizeof(report), 1, &percentage) ==
           -EBADMSG);
    assert(drf_decode_battery(report, sizeof(report), 0, &percentage) ==
           -EINVAL);
    assert(drf_decode_battery(report, sizeof(report), 1, NULL) == -EINVAL);
    assert(percentage == 1234);
}

static void test_invalid_reports(void) {
    uint8_t report[DRF_REPORT_SIZE];
    struct drf_slot slot;
    struct drf_receiver_metadata metadata;
    unsigned int count = 99;

    assert(drf_encode_query((enum drf_query)0x06, 1, report) == -EINVAL);
    assert(drf_encode_query(DRF_QUERY_SLOT, 0, report) == -EINVAL);
    assert(drf_encode_query(DRF_QUERY_SLOT, DRF_SLOT_COUNT + 1, report) ==
           -EINVAL);
    assert(drf_encode_query(DRF_QUERY_COUNT, 1, report) == -EINVAL);

    memcpy(report, fixture_states[0].count_response, sizeof(report));
    assert(drf_decode_count(report, sizeof(report) - 1, &count) == -EMSGSIZE);
    assert(drf_decode_count(report, sizeof(report) + 1, &count) == -EMSGSIZE);

    report[0] ^= 1;
    assert(drf_decode_count(report, sizeof(report), &count) == -EBADMSG);

    report[0] ^= 1;
    report[1] = DRF_QUERY_SLOT;
    assert(drf_decode_count(report, sizeof(report), &count) == -EBADMSG);

    report[1] = DRF_QUERY_COUNT;
    report[2] = DRF_SLOT_COUNT + 1;
    assert(drf_decode_count(report, sizeof(report), &count) == -EBADMSG);
    assert(count == 99);

    memcpy(report, fixture_states[0].slot_responses[0], sizeof(report));
    assert(drf_decode_slot(report, sizeof(report), 2, &slot) == -EBADMSG);

    report[3] = 0x7f;
    assert(drf_decode_slot(report, sizeof(report), 1, &slot) == -EBADMSG);

    report[3] = DRF_SLOT_KEYBOARD;
    report[8] = 0x1b;
    assert(drf_decode_slot(report, sizeof(report), 1, &slot) == -EBADMSG);

    report[8] = 'K';
    report[27] = 'X'; /* Garbage after the model terminator. */
    assert(drf_decode_slot(report, sizeof(report), 1, &slot) == -EBADMSG);

    memcpy(report, fixture_states[0].slot_responses[2], sizeof(report));
    report[10] = 0;
    assert(drf_decode_slot(report, sizeof(report), 3, &slot) == -EBADMSG);

    memcpy(report, fixture_metadata_response, sizeof(report));
    report[6] = '\n';
    assert(drf_decode_receiver(report, sizeof(report), &metadata) == -EBADMSG);
}

static void test_fingerprint(void) {
    struct drf_device device = {.bus = BUS_USB,
                                .vid = DELL_VID,
                                .pid = DELL_PID_UNIVERSAL_4503,
                                .interface_number = 2,
                                .usb_release = 0x0244};
    assert(
        drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.pid = 0x301c;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.pid = 0x8505;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.pid = DELL_PID_UNIVERSAL_4503;
    device.bus = BUS_BLUETOOTH;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.bus = BUS_USB;
    device.vid = 0x046d;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.vid = DELL_VID;
    device.interface_number = 1;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 47));

    device.interface_number = 2;
    assert(
        !drf_query_device_matches(&device, fixture_management_descriptor, 46));

    for (size_t i = 0; i < 47; i++) {
        uint8_t descriptor[47];
        memcpy(descriptor, fixture_management_descriptor, sizeof(descriptor));
        descriptor[i] ^= 1;
        assert(
            !drf_query_device_matches(&device, descriptor, sizeof(descriptor)));
    }
}

struct replay {
    const struct fixture_state *fixture;
    unsigned int step;
    unsigned int fail_step;
    int short_reply;
    int change_count;
};

static int exchange(void *context, const uint8_t request[DRF_REPORT_SIZE],
                    uint8_t response[DRF_REPORT_SIZE], size_t *length) {
    struct replay *replay = context;
    unsigned int step = replay->step++;
    if (replay->fail_step && replay->fail_step == step + 1)
        return -ETIMEDOUT;

    const uint8_t *expected;
    const uint8_t *reply;
    if (step == 0) {
        expected = fixture_metadata_request;
        reply = fixture_metadata_response;
    } else if (step == 1 || step == 8) {
        expected = replay->fixture->count_request;
        reply = replay->fixture->count_response;
    } else {
        assert(step >= 2 && step <= 7);
        expected = replay->fixture->slot_requests[step - 2];
        reply = replay->fixture->slot_responses[step - 2];
    }

    assert(memcmp(request, expected, DRF_REPORT_SIZE) == 0);
    memcpy(response, reply, DRF_REPORT_SIZE);
    *length = replay->short_reply ? DRF_REPORT_SIZE - 1 : DRF_REPORT_SIZE;
    if (step == 8 && replay->change_count)
        response[2] = 0;
    return 0;
}

static void test_snapshot(void) {
    for (size_t i = 0; i < sizeof(fixture_states) / sizeof(fixture_states[0]);
         i++) {
        struct replay replay = {.fixture = &fixture_states[i]};
        struct drf_snapshot snapshot;
        assert(drf_read_snapshot(exchange, &replay, &snapshot) == 0);
        assert(replay.step == 9);
        assert(snapshot.paired_count == replay.fixture->count);
        assert(strcmp(snapshot.receiver.name, "Dell Universal Receiver") == 0);
    }

    struct drf_snapshot snapshot, original;
    memset(&original, 0x55, sizeof(original));
    snapshot = original;
    struct replay replay = {.fixture = &fixture_states[0], .short_reply = 1};
    assert(drf_read_snapshot(exchange, &replay, &snapshot) == -EMSGSIZE);
    assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);

    for (unsigned int step = 1; step <= 9; step++) {
        replay =
            (struct replay){.fixture = &fixture_states[0], .fail_step = step};
        assert(drf_read_snapshot(exchange, &replay, &snapshot) == -ETIMEDOUT);
        assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);
    }

    replay = (struct replay){.fixture = &fixture_states[0], .change_count = 1};
    assert(drf_read_snapshot(exchange, &replay, &snapshot) == -EAGAIN);
    assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);
}

struct battery_replay {
    struct replay snapshot;
    unsigned int battery_step;
    int malformed;
};

static int battery_exchange(void *context,
                            const uint8_t request[DRF_REPORT_SIZE],
                            uint8_t response[DRF_REPORT_SIZE], size_t *length) {
    struct battery_replay *replay = context;
    if (replay->snapshot.step < 9)
        return exchange(&replay->snapshot, request, response, length);

    unsigned int step = replay->battery_step++;
    assert(step < 2);
    assert(memcmp(request, fixture_battery_requests[step], DRF_REPORT_SIZE) ==
           0);
    memcpy(response, fixture_battery_responses[step], DRF_REPORT_SIZE);
    if (replay->malformed)
        response[2] = 6;
    *length = DRF_REPORT_SIZE;
    return 0;
}

static void test_battery_snapshot(void) {
    struct battery_replay replay = {
        .snapshot = {.fixture = &fixture_states[0]},
    };
    struct drf_battery_snapshot result;
    assert(drf_read_batteries(battery_exchange, &replay, &result) == 0);
    assert(replay.snapshot.step == 9);
    assert(replay.battery_step == 2);
    assert(result.percentages[0] == 73);
    assert(result.percentages[1] == DRF_BATTERY_UNKNOWN);
    for (size_t i = 2; i < DRF_SLOT_COUNT; i++)
        assert(result.percentages[i] == DRF_BATTERY_UNKNOWN);

    struct fixture_state no_mouse_battery = fixture_states[0];
    no_mouse_battery.slot_responses[1][30] &= (uint8_t)~DRF_CAP_BATTERY;
    replay = (struct battery_replay){
        .snapshot = {.fixture = &no_mouse_battery},
    };
    assert(drf_read_batteries(battery_exchange, &replay, &result) == 0);
    assert(replay.battery_step == 1);
    assert(!(result.snapshot.slots[1].capabilities & DRF_CAP_BATTERY));
    assert(result.percentages[1] == DRF_BATTERY_UNKNOWN);

    replay = (struct battery_replay){
        .snapshot = {.fixture = &fixture_states[2]},
    };
    assert(drf_read_batteries(battery_exchange, &replay, &result) == 0);
    assert(replay.battery_step == 0);

    struct drf_battery_snapshot original;
    memset(&original, 0x55, sizeof(original));
    result = original;
    replay = (struct battery_replay){
        .snapshot = {.fixture = &fixture_states[0]}, .malformed = 1};
    assert(drf_read_batteries(battery_exchange, &replay, &result) == -EBADMSG);
    assert(memcmp(&result, &original, sizeof(result)) == 0);
}

int main(void) {
    test_fixture_queries();
    test_battery_codec();
    test_invalid_reports();
    test_fingerprint();
    test_snapshot();
    test_battery_snapshot();

    puts("protocol: fixture states, battery reports, malformed reports, "
         "identity checks, and failures passed");
    return 0;
}
