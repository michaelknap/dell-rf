#ifndef DELL_RF_ACTIONS_H
#define DELL_RF_ACTIONS_H

#include "dell_rf_protocol.h"

enum drf_action {
    DRF_ACTION_UNPAIR = 1,
    DRF_ACTION_PAIR = 2,
};

/* The caller supplies confirmation; there is no implicit approval path.
 * A deadline of zero means no pairing deadline. Return zero to approve,
 * otherwise a negative errno. */
struct drf_action_callbacks {
    void *context;
    int (*confirm)(void *context, enum drf_action action,
                   const struct drf_snapshot *snapshot,
                   const struct drf_slot *device, uint64_t deadline_ms);
    int (*cancelled)(void *context);
    void (*search_started)(void *context);
};

struct drf_action_result {
    int change_attempted;
    int verified;
    int acknowledgement_error;
    struct drf_slot device;
    struct drf_snapshot snapshot;
};

/* Injectable hardware and clock boundary for transaction replay tests. */
struct drf_action_io {
    void *context;
    drf_exchange_fn exchange;
    int (*wait_ms)(void *context, unsigned int milliseconds);
    int (*now_ms)(void *context, uint64_t *milliseconds);
    int (*receive)(void *context, uint8_t report[DRF_REPORT_SIZE + 1],
                   size_t *length, unsigned int timeout_ms);
};

int drf_unpair(const struct drf_action_io *io, unsigned int slot,
               const struct drf_action_callbacks *callbacks,
               struct drf_action_result *result);

int drf_receiver_unpair(const char *path, unsigned int slot,
                        const struct drf_action_callbacks *callbacks,
                        struct drf_action_result *result);

int drf_pair(const struct drf_action_io *io, enum drf_slot_kind kind,
             unsigned int timeout_ms,
             const struct drf_action_callbacks *callbacks,
             struct drf_action_result *result);

int drf_receiver_pair(const char *path, enum drf_slot_kind kind,
                      unsigned int timeout_ms,
                      const struct drf_action_callbacks *callbacks,
                      struct drf_action_result *result);

#endif
