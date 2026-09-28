#include "dell_rf_protocol.h"

#include <errno.h>
#include <linux/input.h>
#include <string.h>

/* Management-interface descriptor for 413c:4503, USB release 0244. */
static const uint8_t management_descriptor[] = {
    0x06, 0x00, 0xff, 0x09, 0x00, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x00, 0x15,
    0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x1f, 0x82, 0x02, 0x01, 0xc0,
    0x06, 0x01, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x08, 0x95, 0x1f, 0x75,
    0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x09, 0x20, 0xb1, 0x03, 0xc0,
};

static int check_report(const uint8_t *report, size_t length,
                        unsigned int command) {
    if (!report)
        return -EINVAL;
    if (length != DRF_REPORT_SIZE)
        return -EMSGSIZE;
    if (report[0] != DRF_REPORT_ID || report[1] != command)
        return -EBADMSG;

    return 0;
}

static int filled_with(const uint8_t *data, size_t length, uint8_t value) {
    for (size_t i = 0; i < length; i++) {
        if (data[i] != value)
            return 0;
    }

    return 1;
}

static int copy_text(char *out, const uint8_t *data, size_t length) {
    size_t end = 0;
    while (end < length && data[end]) {
        /* Never pass receiver-supplied control characters to the terminal. */
        if (data[end] < 0x20 || data[end] > 0x7e)
            return -EBADMSG;

        out[end] = (char)data[end];
        end++;
    }

    if (!end || !filled_with(data + end, length - end, 0))
        return -EBADMSG;

    out[end] = '\0';
    return 0;
}

int drf_encode_query(enum drf_query query, unsigned int slot,
                     uint8_t report[DRF_REPORT_SIZE]) {
    if (!report)
        return -EINVAL;

    switch (query) {
    case DRF_QUERY_RECEIVER:
    case DRF_QUERY_COUNT:
        if (slot != 0)
            return -EINVAL;
        break;
    case DRF_QUERY_SLOT:
        if (slot < 1 || slot > DRF_SLOT_COUNT)
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }

    memset(report, 0, DRF_REPORT_SIZE);
    report[0] = DRF_REPORT_ID;
    report[1] = (uint8_t)query;
    report[2] = (uint8_t)slot;
    return 0;
}

int drf_decode_receiver(const uint8_t *report, size_t length,
                        struct drf_receiver_metadata *out) {
    if (!out)
        return -EINVAL;
    int r = check_report(report, length, DRF_QUERY_RECEIVER);
    if (r)
        return r;

    struct drf_receiver_metadata decoded = {0};
    r = copy_text(decoded.name, report + 6, DRF_RECEIVER_NAME_SIZE);
    if (r)
        return r;

    memcpy(decoded.opaque_version, report + 2, sizeof(decoded.opaque_version));
    *out = decoded;
    return 0;
}

int drf_decode_count(const uint8_t *report, size_t length, unsigned int *out) {
    if (!out)
        return -EINVAL;
    int r = check_report(report, length, DRF_QUERY_COUNT);
    if (r)
        return r;
    if (report[2] > DRF_SLOT_COUNT || !filled_with(report + 3, 29, 0))
        return -EBADMSG;

    *out = report[2];
    return 0;
}

int drf_decode_slot(const uint8_t *report, size_t length, unsigned int slot,
                    struct drf_slot *out) {
    if (!out || slot < 1 || slot > DRF_SLOT_COUNT)
        return -EINVAL;
    int r = check_report(report, length, DRF_QUERY_SLOT);
    if (r)
        return r;

    struct drf_slot decoded = {.number = slot, .kind = DRF_SLOT_EMPTY};

    /* With no paired devices, all six requests return the same zero record. */
    if (filled_with(report + 2, 30, 0)) {
        *out = decoded;
        return 0;
    }

    if (report[2] != slot)
        return -EBADMSG;
    if (filled_with(report + 3, 27, 0xff) && report[30] == 0 &&
        report[31] == 0) {
        *out = decoded;
        return 0;
    }

    if (report[3] != DRF_SLOT_KEYBOARD && report[3] != DRF_SLOT_MOUSE)
        return -EBADMSG;

    decoded.kind = (enum drf_slot_kind)report[3];
    memcpy(decoded.opaque_id, report + 5, sizeof(decoded.opaque_id));
    r = copy_text(decoded.model, report + 8, DRF_MODEL_SIZE);
    if (r)
        return r;

    *out = decoded;
    return 0;
}

int drf_encode_unpair(unsigned int slot, uint8_t report[DRF_REPORT_SIZE]) {
    if (!report || slot < 1 || slot > DRF_SLOT_COUNT)
        return -EINVAL;

    memset(report, 0, DRF_REPORT_SIZE);
    report[0] = DRF_REPORT_ID;
    report[1] = 0x06;
    report[2] = (uint8_t)slot;
    return 0;
}

int drf_decode_unpair_ack(const uint8_t *report, size_t length) {
    int r = check_report(report, length, 0x06);
    if (r)
        return r;

    return filled_with(report + 2, 30, 0) ? 0 : -EBADMSG;
}

static int valid_id(const uint8_t id[3]) {
    return id && !filled_with(id, 3, 0) && !filled_with(id, 3, 0xff);
}

int drf_encode_pair_command(enum drf_pair_command command,
                            const uint8_t opaque_id[3],
                            uint8_t report[DRF_REPORT_SIZE]) {
    if (!report)
        return -EINVAL;

    switch (command) {
    case DRF_PAIR_SELECT:
        if (!valid_id(opaque_id))
            return -EINVAL;
        break;
    case DRF_PAIR_BEGIN:
    case DRF_PAIR_DISCOVERY_STATUS:
    case DRF_PAIR_STATUS:
        if (opaque_id)
            return -EINVAL;
        break;
    default:
        return -EINVAL;
    }

    memset(report, 0, DRF_REPORT_SIZE);
    report[0] = DRF_REPORT_ID;
    report[1] = (uint8_t)command;

    if (command == DRF_PAIR_SELECT) {
        report[2] = 0x01; /* Observed constant, not a slot or device type. */
        memcpy(report + 3, opaque_id, 3);
    }

    return 0;
}

int drf_decode_pair_ack(enum drf_pair_command command, const uint8_t *report,
                        size_t length) {
    unsigned int response_command;
    switch (command) {
    case DRF_PAIR_BEGIN:
        response_command = 0x00;
        break;
    case DRF_PAIR_DISCOVERY_STATUS:
    case DRF_PAIR_SELECT:
        response_command = (unsigned int)command;
        break;
    default:
        return -EINVAL;
    }

    int r = check_report(report, length, response_command);
    if (r)
        return r;
    if (!filled_with(report + 3, 29, 0))
        return -EBADMSG;

    /* Both 00 and 01 occur in successful traces. Do not treat 01 as an
     * error or claim a meaning for it. Final slot readback is authoritative. */
    if (command == DRF_PAIR_BEGIN)
        return report[2] == 0 ? 0 : -EBADMSG;
    return report[2] <= 1 ? 0 : -EBADMSG;
}

int drf_decode_candidate(const uint8_t *report, size_t length,
                         struct drf_slot *out) {
    if (!report || !out)
        return -EINVAL;
    if (length != DRF_REPORT_SIZE)
        return -EMSGSIZE;
    if (report[0] != 0x01)
        return -EBADMSG;
    if (report[1] != 0x01)
        return -ENOMSG;
    if ((report[2] != DRF_SLOT_MOUSE && report[2] != DRF_SLOT_KEYBOARD) ||
        report[3] != 0x04 || !valid_id(report + 14) ||
        !filled_with(report + 17, 15, 0))
        return -EBADMSG;

    struct drf_slot decoded = {.kind = (enum drf_slot_kind)report[2]};
    int r = copy_text(decoded.model, report + 4, 10);
    if (r)
        return r;

    memcpy(decoded.opaque_id, report + 14, 3);
    *out = decoded;
    return 0;
}

int drf_decode_pair_status(const uint8_t *report, size_t length,
                           struct drf_slot *out) {
    if (!out)
        return -EINVAL;
    int r = check_report(report, length, DRF_PAIR_STATUS);
    if (r)
        return r;
    if (report[2] == 0xff && filled_with(report + 3, 29, 0))
        return -EAGAIN;
    if (report[2] != 0x01 || report[3] < 1 || report[3] > DRF_SLOT_COUNT ||
        (report[4] != DRF_SLOT_MOUSE && report[4] != DRF_SLOT_KEYBOARD) ||
        report[5] != 0x04 || !valid_id(report + 6))
        return -EBADMSG;

    struct drf_slot decoded = {.number = report[3],
                               .kind = (enum drf_slot_kind)report[4]};
    r = copy_text(decoded.model, report + 9, DRF_MODEL_SIZE);
    if (r)
        return r;

    memcpy(decoded.opaque_id, report + 6, 3);
    *out = decoded;
    return 0;
}

int drf_query_device_matches(const struct drf_device *device,
                             const uint8_t *descriptor, size_t length) {
    return device && descriptor && device->bus == BUS_USB &&
           device->vid == DELL_VID && device->pid == DELL_PID_UNIVERSAL_4503 &&
           device->interface_number == 2 && device->usb_release == 0x0244 &&
           length == sizeof(management_descriptor) &&
           memcmp(descriptor, management_descriptor, length) == 0;
}

static int query(drf_exchange_fn exchange, void *context,
                 enum drf_query command, unsigned int slot,
                 uint8_t response[DRF_REPORT_SIZE]) {
    uint8_t request[DRF_REPORT_SIZE];
    int r = drf_encode_query(command, slot, request);
    if (r)
        return r;

    memset(response, 0, DRF_REPORT_SIZE);
    size_t length = DRF_REPORT_SIZE;
    r = exchange(context, request, response, &length);
    if (r)
        return r;

    return check_report(response, length, command);
}

int drf_read_snapshot(drf_exchange_fn exchange, void *context,
                      struct drf_snapshot *out) {
    if (!exchange || !out)
        return -EINVAL;

    struct drf_snapshot snapshot = {0};
    uint8_t response[DRF_REPORT_SIZE];
    int r = query(exchange, context, DRF_QUERY_RECEIVER, 0, response);
    if (r)
        return r;
    r = drf_decode_receiver(response, sizeof(response), &snapshot.receiver);
    if (r)
        return r;

    r = query(exchange, context, DRF_QUERY_COUNT, 0, response);
    if (r)
        return r;
    r = drf_decode_count(response, sizeof(response), &snapshot.paired_count);
    if (r)
        return r;

    unsigned int occupied = 0;
    for (unsigned int slot = 1; slot <= DRF_SLOT_COUNT; slot++) {
        r = query(exchange, context, DRF_QUERY_SLOT, slot, response);
        if (r)
            return r;
        if (snapshot.paired_count && response[2] == 0)
            return -EAGAIN;

        r = drf_decode_slot(response, sizeof(response), slot,
                            &snapshot.slots[slot - 1]);
        if (r)
            return r;
        if (snapshot.slots[slot - 1].kind != DRF_SLOT_EMPTY)
            occupied++;
    }

    unsigned int after = 0;
    r = query(exchange, context, DRF_QUERY_COUNT, 0, response);
    if (r)
        return r;
    r = drf_decode_count(response, sizeof(response), &after);
    if (r)
        return r;
    if (occupied != snapshot.paired_count || after != snapshot.paired_count)
        return -EAGAIN;

    /* Publish only a complete, validated result. */
    *out = snapshot;
    return 0;
}
