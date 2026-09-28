#ifndef DELL_RF_PROTOCOL_H
#define DELL_RF_PROTOCOL_H

#include "dell_rf.h"

#define DRF_REPORT_ID 0x08
#define DRF_REPORT_SIZE 32
#define DRF_SLOT_COUNT 6
#define DRF_MODEL_SIZE 20
#define DRF_RECEIVER_NAME_SIZE 24

/* Queries established by the 4503 capture. */
enum drf_query {
    DRF_QUERY_RECEIVER = 0x01,
    DRF_QUERY_COUNT = 0x02,
    DRF_QUERY_SLOT = 0x03,
};

enum drf_slot_kind {
    DRF_SLOT_EMPTY = 0,
    DRF_SLOT_KEYBOARD = 1,
    DRF_SLOT_MOUSE = 2,
};

enum drf_pair_command {
    DRF_PAIR_SELECT = 0x04,
    DRF_PAIR_STATUS = 0x05,
    DRF_PAIR_DISCOVERY_STATUS = 0x08,
    DRF_PAIR_BEGIN = 0x0b,
};

struct drf_receiver_metadata {
    char name[DRF_RECEIVER_NAME_SIZE + 1];
    uint8_t opaque_version[4];
};

struct drf_slot {
    unsigned int number;
    enum drf_slot_kind kind;
    uint8_t opaque_id[3];
    char model[DRF_MODEL_SIZE + 1];
};

struct drf_snapshot {
    struct drf_receiver_metadata receiver;
    unsigned int paired_count;
    struct drf_slot slots[DRF_SLOT_COUNT];
};

typedef int (*drf_exchange_fn)(void *context,
                               const uint8_t request[DRF_REPORT_SIZE],
                               uint8_t response[DRF_REPORT_SIZE],
                               size_t *length);

typedef int (*drf_receiver_path_fn)(void *context, const char *path);

int drf_encode_query(enum drf_query query, unsigned int slot,
                     uint8_t report[DRF_REPORT_SIZE]);

int drf_decode_receiver(const uint8_t *report, size_t length,
                        struct drf_receiver_metadata *out);

int drf_decode_count(const uint8_t *report, size_t length, unsigned int *out);

int drf_decode_slot(const uint8_t *report, size_t length, unsigned int slot,
                    struct drf_slot *out);

int drf_encode_unpair(unsigned int slot, uint8_t report[DRF_REPORT_SIZE]);

int drf_decode_unpair_ack(const uint8_t *report, size_t length);

int drf_encode_pair_command(enum drf_pair_command command,
                            const uint8_t opaque_id[3],
                            uint8_t report[DRF_REPORT_SIZE]);

int drf_decode_pair_ack(enum drf_pair_command command, const uint8_t *report,
                        size_t length);

/* -ENOMSG means an unrelated input report; -EAGAIN means pairing is pending. */
int drf_decode_candidate(const uint8_t *report, size_t length,
                         struct drf_slot *out);

int drf_decode_pair_status(const uint8_t *report, size_t length,
                           struct drf_slot *out);

int drf_query_device_matches(const struct drf_device *device,
                             const uint8_t *descriptor, size_t length);

int drf_read_snapshot(drf_exchange_fn exchange, void *context,
                      struct drf_snapshot *out);

/* Enumerate validated interfaces without sending feature reports. The path
 * is borrowed for the callback; return zero to continue or a negative errno
 * to stop. No accessible matches returns -ENODEV. */
int drf_list_query_receivers(drf_receiver_path_fn visit, void *context);

int drf_find_query_receiver(char *path, size_t capacity);

int drf_receiver_slots(const char *path, struct drf_snapshot *out);

#endif
