#define _GNU_SOURCE

#include "dell_rf_actions.h"
#include "fixtures/4503_actions.h"
#include "fixtures/4503_queries.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Linked only into the CLI test executable. Pair/unpair cannot access hardware.
 */
static const char *scenario(void) {
    const char *value = getenv("DRF_TEST_SCENARIO");
    return value ? value : "";
}

static unsigned int device_opens;
static int probed_fd = -1;

int __real_open(const char *path, int flags, ...);

int __wrap_open(const char *path, int flags, ...) {
    if (strcmp(scenario(), "monitor-single-open") == 0 &&
        strcmp(path, "/dev/null") == 0)
        device_opens++;

    return __real_open(path, flags);
}

int __real_ioctl(int fd, unsigned long request, ...);

int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *argument = va_arg(args, void *);
    va_end(args);

    if (strcmp(scenario(), "monitor-single-open") == 0) {
        if (request == HIDIOCGRAWINFO) {
            struct hidraw_devinfo *info = argument;
            *info = (struct hidraw_devinfo){.bustype = BUS_USB,
                                            .vendor = DELL_VID,
                                            .product = DELL_PID_UNIVERSAL_4503};
            return 0;
        }
        if (request == HIDIOCGRDESCSIZE) {
            *(int *)argument = 1;
            return 0;
        }
        if (request == HIDIOCGRAWNAME(256) || request == HIDIOCGRAWPHYS(256)) {
            *(char *)argument = '\0';
            return 0;
        }
        assert(0 && "unexpected monitor ioctl");
    }

    return __real_ioctl(fd, request, argument);
}

int __real_poll(struct pollfd *fds, nfds_t count, int timeout);

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (strcmp(scenario(), "monitor-single-open") == 0) {
        assert(device_opens == 1);
        assert(count == 1 && fds[0].fd == probed_fd);
        errno = EIO;
        return -1;
    }

    return __real_poll(fds, count, timeout);
}

int __real_drf_probe_fd(int fd, struct drf_device *out);

int __wrap_drf_probe_fd(int fd, struct drf_device *out) {
    if (strcmp(scenario(), "monitor-single-open") == 0) {
        memset(out, 0, sizeof(*out));
        out->bus = BUS_USB;
        out->vid = DELL_VID;
        out->pid = DELL_PID_UNIVERSAL_4503;
        probed_fd = fd;
        return 0;
    }

    if (strcmp(scenario(), "info-controls") == 0 ||
        strcmp(scenario(), "info-safe") == 0 ||
        strcmp(scenario(), "info-utf8") == 0) {
        memset(out, 0, sizeof(*out));
        out->bus = BUS_USB;
        out->vid = DELL_VID;
        out->pid = DELL_PID_UNIVERSAL_4503;
        out->interface_number = 2;
        out->usb_release = 0x0244;
        out->descriptor_size = 1;

        if (strcmp(scenario(), "info-safe") == 0) {
            strcpy(out->name, "Dell Universal Receiver");
            strcpy(out->phys, "usb-0000:00:14.0-1/input2");
        } else if (strcmp(scenario(), "info-utf8") == 0) {
            static const unsigned char name[] = {
                'M', 'a', 'u', 's', 'g', 'e', 'r', 0xc3, 0xa4, 't', 0,
            };
            memcpy(out->name, name, sizeof(name));
            strcpy(out->phys, "usb-0000:00:14.0-1/input2");
        } else {
            static const unsigned char name[] = {
                'D', 'e',  'l',  'l',  0x1b, '[',  '3', '1',
                'm', '\n', 0x7f, 0xc2, 0x9b, 0xff, 0,
            };
            static const unsigned char phys[] = {
                'u', 's', 'b',  '-', '1',  '\r', 's',  'p', 'o',
                'o', 'f', 0x1b, ']', '0',  ';',  't',  'i', 't',
                'l', 'e', 0x07, ' ', 0xe2, 0x80, 0xae, 0,
            };
            memcpy(out->name, name, sizeof(name));
            memcpy(out->phys, phys, sizeof(phys));
        }
        return 0;
    }

    return __real_drf_probe_fd(fd, out);
}

int __real_drf_read_descriptor_fd(int fd, uint8_t *buf, size_t cap,
                                  size_t *len);

int __wrap_drf_read_descriptor_fd(int fd, uint8_t *buf, size_t cap,
                                  size_t *len) {
    if (strcmp(scenario(), "info-controls") == 0 ||
        strcmp(scenario(), "info-safe") == 0 ||
        strcmp(scenario(), "info-utf8") == 0) {
        assert(fd >= 0 && cap >= 1);
        buf[0] = 0;
        *len = 1;
        return 0;
    }

    return __real_drf_read_descriptor_fd(fd, buf, cap, len);
}

int __wrap_drf_find_query_receiver(char *path, size_t capacity) {
    if (strcmp(scenario(), "multiple") == 0 ||
        strcmp(scenario(), "multiple-list-error") == 0) {
        path[0] = '\0';
        return -EEXIST;
    }

    static const char selected[] = "/dev/hidraw-test";
    if (capacity < sizeof(selected))
        return -ENAMETOOLONG;

    memcpy(path, selected, sizeof(selected));
    return 0;
}

int __wrap_drf_list_query_receivers(drf_receiver_path_fn visit, void *context) {
    assert(strcmp(scenario(), "multiple") == 0 ||
           strcmp(scenario(), "multiple-list-error") == 0);
    if (strcmp(scenario(), "multiple-list-error") == 0)
        return -EIO;

    const char *paths[] = {"/dev/hidraw2", "/dev/hidraw5", "/dev/hidraw8"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int r = visit(context, paths[i]);
        if (r)
            return r;
    }
    return 0;
}

static void snapshot(unsigned int index, struct drf_snapshot *out) {
    memset(out, 0, sizeof(*out));
    const struct fixture_state *state = &fixture_states[index];
    assert(drf_decode_receiver(fixture_metadata_response, 32, &out->receiver) ==
           0);

    out->paired_count = state->count;
    for (unsigned int slot = 1; slot <= DRF_SLOT_COUNT; slot++)
        assert(drf_decode_slot(state->slot_responses[slot - 1], 32, slot,
                               &out->slots[slot - 1]) == 0);
}

int __wrap_drf_receiver_batteries(const char *path,
                                  struct drf_battery_snapshot *out) {
    printf("MOCK_ARGS battery %s\n", path);
    memset(out, 0, sizeof(*out));
    snapshot(0, &out->snapshot);
    for (size_t i = 0; i < DRF_SLOT_COUNT; i++)
        out->percentages[i] = DRF_BATTERY_UNKNOWN;
    out->percentages[0] = 73;
    return 0;
}

static int approve(enum drf_action action,
                   const struct drf_action_callbacks *callbacks,
                   const struct drf_snapshot *before, uint64_t deadline,
                   struct drf_action_result *result) {
    if (strcmp(scenario(), "stale") == 0) {
        struct timespec delay = {.tv_nsec = 200000000};
        assert(nanosleep(&delay, NULL) == 0);
    }

    int r = callbacks->confirm(callbacks->context, action, before,
                               &result->device, deadline);
    if (r)
        return r;

    puts("MOCK_COMMIT");
    result->change_attempted = 1;
    if (strcmp(scenario(), "uncertain") == 0)
        return -ETIMEDOUT;

    result->verified = 1;
    return 0;
}

int __wrap_drf_receiver_unpair(const char *path, unsigned int slot,
                               const struct drf_action_callbacks *callbacks,
                               struct drf_action_result *result) {
    memset(result, 0, sizeof(*result));
    printf("MOCK_ARGS unpair %u %s\n", slot, path);
    fflush(stdout);
    if (strcmp(scenario(), "unsupported") == 0)
        return -EOPNOTSUPP;

    struct drf_snapshot before;
    snapshot(4, &before);
    result->device = before.slots[slot - 1];
    if (result->device.kind == DRF_SLOT_EMPTY)
        return -ENOENT;

    result->snapshot = before;
    result->snapshot.paired_count--;
    return approve(DRF_ACTION_UNPAIR, callbacks, &before, 0, result);
}

int __wrap_drf_receiver_pair(const char *path, enum drf_slot_kind kind,
                             unsigned int timeout_ms,
                             const struct drf_action_callbacks *callbacks,
                             struct drf_action_result *result) {
    assert(timeout_ms == 30000);
    memset(result, 0, sizeof(*result));
    printf("MOCK_ARGS pair %u %u %s\n", kind, timeout_ms, path);
    fflush(stdout);

    struct drf_snapshot before;
    snapshot(kind == DRF_SLOT_MOUSE ? 1 : 3, &before);
    if (strcmp(scenario(), "already") == 0) {
        snapshot(4, &before);
        result->device = before.slots[kind == DRF_SLOT_MOUSE ? 0 : 1];
        return -EALREADY;
    }

    assert(drf_decode_candidate(kind == DRF_SLOT_MOUSE
                                    ? fixture_candidate_mouse
                                    : fixture_candidate_keyboard,
                                32, &result->device) == 0);
    if (callbacks->search_started)
        callbacks->search_started(callbacks->context);

    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    unsigned int remaining_ms =
        strcmp(scenario(), "deadline") == 0 ? 1000 : timeout_ms;
    uint64_t deadline = (uint64_t)ts.tv_sec * 1000 +
                        (uint64_t)ts.tv_nsec / 1000000 + remaining_ms;

    snapshot(4, &result->snapshot);
    int r = approve(DRF_ACTION_PAIR, callbacks, &before, deadline, result);
    if (!r)
        result->device.number = kind == DRF_SLOT_MOUSE ? 1 : 2;
    return r;
}
