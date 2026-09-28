#define _GNU_SOURCE

#include "dell_rf_actions.h"
#include "fixtures/4503_actions.h"
#include "fixtures/4503_queries.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Wrap the hardware boundary, while exercising the real transport and codec. */
static struct {
    struct drf_device device;
    const struct fixture_state *state;
    unsigned int device_count;

    unsigned int writes;
    unsigned int reads;
    unsigned int closes;
    unsigned int writable_opens;
    unsigned int queries;
    unsigned int mutations;
    unsigned int confirmations;
    unsigned int status_polls;
    unsigned int input_reads;
    unsigned int input_polls;
    unsigned int last_command;

    uint64_t time_ms;
    enum drf_action action;
    enum drf_slot_kind kind;
    const struct fixture_state *after;

    int guard;
    int acknowledgement_error;
    int input_short;
    int input_hup;
    int locked;
    int denied;
    int bad_descriptor;
    int lock_busy;
    int short_write;
    int short_read;
    int read_error;
    int denied_fd;
    int invalid_interface_fd;
    int bad_descriptor_fd;
    int glob_error;

    const uint8_t *reply;
} mock;

static void reset(void) {
    memset(&mock, 0, sizeof(mock));
    mock.device = (struct drf_device){.bus = BUS_USB,
                                      .vid = DELL_VID,
                                      .pid = DELL_PID_UNIVERSAL_4503,
                                      .interface_number = 2,
                                      .usb_release = 0x0244};
    mock.state = &fixture_states[0];
    mock.device_count = 1;
}

int __wrap_open(const char *path, int flags, ...) {
    assert(strcmp(path, "/dev/hidraw2") == 0 ||
           strcmp(path, "/dev/hidraw5") == 0 ||
           strcmp(path, "/dev/hidraw8") == 0);
    assert(flags & O_CLOEXEC);

    int fd = strcmp(path, "/dev/hidraw2") == 0   ? 42
             : strcmp(path, "/dev/hidraw5") == 0 ? 43
                                                 : 44;
    if (mock.denied || mock.denied_fd == fd) {
        errno = EACCES;
        return -1;
    }

    if ((flags & O_ACCMODE) != O_RDONLY)
        mock.writable_opens++;
    return fd;
}

int __wrap_close(int fd) {
    assert(fd >= 42 && fd <= 44);
    mock.closes++;
    return 0;
}

int __wrap_drf_probe_fd(int fd, struct drf_device *out) {
    assert(fd >= 42 && fd <= 44);
    *out = mock.device;
    if (fd == mock.invalid_interface_fd)
        out->interface_number = 1;
    return 0;
}

int __wrap_drf_read_descriptor_fd(int fd, uint8_t *out, size_t capacity,
                                  size_t *length) {
    assert(fd >= 42 && fd <= 44);
    assert(capacity >= sizeof(fixture_management_descriptor));

    memcpy(out, fixture_management_descriptor,
           sizeof(fixture_management_descriptor));
    if (mock.bad_descriptor || fd == mock.bad_descriptor_fd)
        out[0] ^= 1;
    *length = sizeof(fixture_management_descriptor);
    return 0;
}

int __wrap_flock(int fd, int operation) {
    assert(fd == 42);
    assert(operation == (LOCK_EX | LOCK_NB));
    if (mock.lock_busy) {
        errno = EWOULDBLOCK;
        return -1;
    }

    mock.locked = 1;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long operation, ...) {
    assert(fd == 42);

    va_list args;
    va_start(args, operation);
    uint8_t *report = va_arg(args, uint8_t *);
    va_end(args);

    if (operation == HIDIOCSFEATURE(DRF_REPORT_SIZE)) {
        const uint8_t *expected = NULL;
        mock.writes++;
        mock.last_command = report[1];
        unsigned int step = mock.queries % 9;

        if (report[1] == 6) {
            assert(mock.action == DRF_ACTION_UNPAIR && report[2] == 2);
            expected = fixture_unpair_mouse.request;
            mock.reply = fixture_unpair_mouse.response;
            assert(++mock.mutations == 1 && mock.confirmations == 1);
            mock.state = mock.after;
        } else if (report[1] == DRF_PAIR_BEGIN) {
            assert(mock.action == DRF_ACTION_PAIR);
            expected = fixture_pair_begin.request;
            mock.reply = fixture_pair_begin.response;
        } else if (report[1] == DRF_PAIR_DISCOVERY_STATUS) {
            assert(mock.action == DRF_ACTION_PAIR);
            expected = fixture_discovery_zero.request;
            mock.reply = fixture_discovery_zero.response;
        } else if (report[1] == DRF_PAIR_SELECT) {
            assert(mock.action == DRF_ACTION_PAIR);

            const struct fixture_action_exchange *fixture =
                mock.kind == DRF_SLOT_MOUSE ? &fixture_select_mouse
                                            : &fixture_select_keyboard;
            expected = fixture->request;
            mock.reply = fixture->response;
            assert(++mock.mutations == 1 && mock.confirmations == 1);
        } else if (report[1] == DRF_PAIR_STATUS) {
            assert(mock.action == DRF_ACTION_PAIR && mock.mutations == 1);

            const struct fixture_action_exchange *fixture =
                mock.status_polls++ == 0      ? &fixture_pair_pending
                : mock.kind == DRF_SLOT_MOUSE ? &fixture_paired_mouse
                                              : &fixture_paired_keyboard;
            expected = fixture->request;
            mock.reply = fixture->response;
            if (mock.status_polls > 1)
                mock.state = mock.after;
        } else if (step == 0) {
            expected = fixture_metadata_request;
            mock.reply = fixture_metadata_response;
        } else if (step == 1 || step == 8) {
            expected = mock.state->count_request;
            mock.reply = mock.state->count_response;
        } else {
            assert(step >= 2 && step <= 7);
            expected = mock.state->slot_requests[step - 2];
            mock.reply = mock.state->slot_responses[step - 2];
        }

        if (report[1] >= 1 && report[1] <= 3)
            mock.queries++;
        assert(memcmp(report, expected, DRF_REPORT_SIZE) == 0);
        return mock.short_write ? DRF_REPORT_SIZE - 1 : DRF_REPORT_SIZE;
    }

    assert(operation == HIDIOCGFEATURE(DRF_REPORT_SIZE));
    assert(report[0] == DRF_REPORT_ID);
    assert(mock.reply);

    mock.reads++;
    if (mock.acknowledgement_error &&
        (mock.last_command == 6 || mock.last_command == DRF_PAIR_SELECT)) {
        errno = mock.acknowledgement_error;
        return -1;
    }
    if (mock.read_error) {
        errno = mock.read_error;
        return -1;
    }

    memcpy(report, mock.reply, DRF_REPORT_SIZE);
    return mock.short_read ? DRF_REPORT_SIZE - 1 : DRF_REPORT_SIZE;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
    assert(mock.action && timeout >= 0 && timeout <= 1000);
    mock.time_ms += (unsigned int)timeout;

    if (!count) {
        assert(fds == NULL && timeout > 0);
        return 0;
    }

    assert(count == 1 && fds[0].fd == 42 && fds[0].events == POLLIN);
    if (!timeout)
        return 0; /* Empty preflight input queue. */
    if (mock.input_polls++ == 0)
        return 0; /* Discovery status polling also uses the real transport. */

    fds[0].revents = mock.input_hup ? POLLHUP : POLLIN;
    return 1;
}

ssize_t __wrap_read(int fd, void *buffer, size_t length) {
    assert(fd == 42 && length == DRF_REPORT_SIZE + 1);

    mock.input_reads++;
    memcpy(buffer,
           mock.kind == DRF_SLOT_MOUSE ? fixture_candidate_mouse
                                       : fixture_candidate_keyboard,
           32);
    return mock.input_short ? 31 : 32;
}

int __wrap_clock_gettime(clockid_t clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC);

    out->tv_sec = (time_t)(mock.time_ms / 1000);
    out->tv_nsec = (long)(mock.time_ms % 1000) * 1000000;
    return 0;
}

int __real_drf_read_snapshot(drf_exchange_fn exchange, void *context,
                             struct drf_snapshot *out);

int __real_drf_unpair(const struct drf_action_io *io, unsigned int slot,
                      const struct drf_action_callbacks *callbacks,
                      struct drf_action_result *result);

int __real_drf_pair(const struct drf_action_io *io, enum drf_slot_kind kind,
                    unsigned int timeout_ms,
                    const struct drf_action_callbacks *callbacks,
                    struct drf_action_result *result);

/* Deliberately bypass encoders to prove that each session's transport guard
 * rejects mutations belonging to another operation and malformed padding. */
int __wrap_drf_read_snapshot(drf_exchange_fn exchange, void *context,
                             struct drf_snapshot *out) {
    if (mock.guard && !mock.action) {
        uint8_t response[32];
        size_t length = sizeof(response);
        return exchange(context, fixture_unpair_mouse.request, response,
                        &length);
    }

    return __real_drf_read_snapshot(exchange, context, out);
}

static int bad_request(const struct drf_action_io *io, int pairing) {
    uint8_t request[32], response[32];
    memcpy(request,
           pairing ? fixture_select_mouse.request
                   : fixture_unpair_mouse.request,
           32);

    switch (mock.guard) {
    case 1:
        request[1] = pairing ? 6 : DRF_PAIR_BEGIN;
        break;
    case 2:
        request[2] = 1 + request[2];
        break;
    case 3:
        request[31] = 1;
        break;
    case 4:
        request[0] = 1;
        break;
    case 5:
        request[1] = 0xff;
        break;
    case 6:
        request[2] = 0;
        break;
    case 7:
        memset(request + 3, 0, 3);
        request[1] = DRF_PAIR_SELECT;
        break;
    }

    size_t length = sizeof(response);
    return io->exchange(io->context, request, response, &length);
}

int __wrap_drf_unpair(const struct drf_action_io *io, unsigned int slot,
                      const struct drf_action_callbacks *callbacks,
                      struct drf_action_result *result) {
    return mock.guard ? bad_request(io, 0)
                      : __real_drf_unpair(io, slot, callbacks, result);
}

int __wrap_drf_pair(const struct drf_action_io *io, enum drf_slot_kind kind,
                    unsigned int timeout_ms,
                    const struct drf_action_callbacks *callbacks,
                    struct drf_action_result *result) {
    return mock.guard
               ? bad_request(io, 1)
               : __real_drf_pair(io, kind, timeout_ms, callbacks, result);
}

int __wrap_glob(const char *pattern, int flags, int (*error)(const char *, int),
                glob_t *out) {
    static char first[] = "/dev/hidraw2", second[] = "/dev/hidraw5",
                third[] = "/dev/hidraw8";
    static char *paths[] = {first, second, third, NULL};
    assert(strcmp(pattern, "/dev/hidraw*") == 0);
    assert(flags == 0 && error == NULL);
    assert(mock.device_count <= 3);
    if (mock.glob_error)
        return mock.glob_error;
    if (!mock.device_count)
        return GLOB_NOMATCH;

    out->gl_pathc = mock.device_count;
    out->gl_pathv = paths;
    return 0;
}

void __wrap_globfree(glob_t *devices) {
    (void)devices;
}

static void test_selection(void) {
    char path[256];
    reset();
    assert(drf_find_query_receiver(path, sizeof(path)) == 0);
    assert(strcmp(path, "/dev/hidraw2") == 0);
    assert(mock.writes == 0 && mock.reads == 0);

    reset();
    mock.device_count = 2;
    assert(drf_find_query_receiver(path, sizeof(path)) == -EEXIST);
    assert(path[0] == '\0');
    assert(mock.writes == 0 && mock.reads == 0);

    reset();
    mock.device_count = 0;
    assert(drf_find_query_receiver(path, sizeof(path)) == -ENODEV);
    assert(path[0] == '\0');

    reset();
    mock.bad_descriptor = 1;
    assert(drf_find_query_receiver(path, sizeof(path)) == -ENODEV);

    reset();
    mock.device_count = 2;
    mock.invalid_interface_fd = 43;
    assert(drf_find_query_receiver(path, sizeof(path)) == 0);
    assert(strcmp(path, "/dev/hidraw2") == 0);

    reset();
    char short_path[] = {'X', 'Y'};
    assert(drf_find_query_receiver(short_path, 1) == -ENAMETOOLONG);
    assert(short_path[0] == '\0' && short_path[1] == 'Y');
}

struct receiver_paths {
    char paths[3][256];
    unsigned int count;
    int result;
};

static int collect_receiver_path(void *context, const char *path) {
    struct receiver_paths *listing = context;
    assert(listing->count < 3);
    size_t length = strlen(path) + 1;
    assert(length <= sizeof(listing->paths[0]));
    memcpy(listing->paths[listing->count++], path, length);
    return listing->result;
}

static void test_receiver_listing(void) {
    struct receiver_paths listing = {0};
    reset();
    mock.device_count = 3;
    assert(drf_list_query_receivers(collect_receiver_path, &listing) == 0);
    assert(listing.count == 3);
    assert(strcmp(listing.paths[0], "/dev/hidraw2") == 0);
    assert(strcmp(listing.paths[1], "/dev/hidraw5") == 0);
    assert(strcmp(listing.paths[2], "/dev/hidraw8") == 0);
    assert(mock.closes == 3 && mock.writable_opens == 0);
    assert(mock.writes == 0 && mock.reads == 0);

    reset();
    listing = (struct receiver_paths){0};
    mock.device_count = 3;
    mock.invalid_interface_fd = 43;
    mock.bad_descriptor_fd = 44;
    assert(drf_list_query_receivers(collect_receiver_path, &listing) == 0);
    assert(listing.count == 1);
    assert(strcmp(listing.paths[0], "/dev/hidraw2") == 0);
    assert(mock.closes == 3 && mock.writable_opens == 0);
    assert(mock.writes == 0 && mock.reads == 0);

    reset();
    listing = (struct receiver_paths){0};
    mock.device_count = 3;
    mock.denied_fd = 43;
    assert(drf_list_query_receivers(collect_receiver_path, &listing) == 0);
    assert(listing.count == 2);
    assert(strcmp(listing.paths[0], "/dev/hidraw2") == 0);
    assert(strcmp(listing.paths[1], "/dev/hidraw8") == 0);
    assert(mock.closes == 2 && mock.writable_opens == 0);
    assert(mock.writes == 0 && mock.reads == 0);

    for (unsigned int failure = 0; failure < 3; failure++) {
        reset();
        listing = (struct receiver_paths){0};
        if (failure == 0)
            mock.device_count = 0;
        else if (failure == 1)
            mock.bad_descriptor = 1;
        else
            mock.denied = 1;

        assert(drf_list_query_receivers(collect_receiver_path, &listing) ==
               -ENODEV);
        assert(listing.count == 0);
        assert(mock.writable_opens == 0 && mock.writes == 0 && mock.reads == 0);
    }

    const int callback_results[] = {-ECANCELED, 1};
    for (size_t i = 0; i < 2; i++) {
        reset();
        listing = (struct receiver_paths){.result = callback_results[i]};
        mock.device_count = 3;
        assert(drf_list_query_receivers(collect_receiver_path, &listing) ==
               (callback_results[i] < 0 ? callback_results[i] : -EINVAL));
        assert(listing.count == 1 && mock.closes == 1);
        assert(mock.writable_opens == 0 && mock.writes == 0 && mock.reads == 0);
    }

    const int glob_errors[] = {GLOB_NOSPACE, GLOB_ABORTED};
    const int expected[] = {-ENOMEM, -EIO};
    for (size_t i = 0; i < 2; i++) {
        reset();
        listing = (struct receiver_paths){0};
        mock.glob_error = glob_errors[i];
        assert(drf_list_query_receivers(collect_receiver_path, &listing) ==
               expected[i]);
        assert(listing.count == 0 && mock.closes == 0);
    }
}

static void test_guard_and_transport(void) {
    struct drf_snapshot snapshot, original;
    memset(&original, 0x55, sizeof(original));
    snapshot = original;

    for (unsigned int failure = 0; failure < 5; failure++) {
        reset();
        switch (failure) {
        case 0:
            mock.device.vid = 0x046d;
            break;
        case 1:
            mock.device.pid = 0x301c;
            break;
        case 2:
            mock.device.bus = BUS_BLUETOOTH;
            break;
        case 3:
            mock.device.interface_number = 1;
            break;
        case 4:
            mock.bad_descriptor = 1;
            break;
        }

        assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EOPNOTSUPP);
        assert(mock.writes == 0 && mock.reads == 0 && mock.closes == 1);
        assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);
    }

    reset();
    mock.lock_busy = 1;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EBUSY);
    assert(mock.writes == 0 && mock.reads == 0 && mock.closes == 1);

    reset();
    mock.denied = 1;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EACCES);
    assert(mock.writes == 0 && mock.reads == 0);

    reset();
    mock.short_write = 1;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EMSGSIZE);
    assert(mock.writes == 1 && mock.reads == 0 && mock.closes == 1);

    reset();
    mock.short_read = 1;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EMSGSIZE);
    assert(mock.writes == 1 && mock.reads == 1 && mock.closes == 1);

    reset();
    mock.read_error = ETIMEDOUT;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -ETIMEDOUT);
    assert(mock.writes == 1 && mock.reads == 1 && mock.closes == 1);
    assert(memcmp(&snapshot, &original, sizeof(snapshot)) == 0);

    const int revisions[] = {0x0240, 0x0244, 0x0245, -1};
    for (size_t revision = 0;
         revision < sizeof(revisions) / sizeof(revisions[0]); revision++) {
        for (size_t i = 0;
             i < sizeof(fixture_states) / sizeof(fixture_states[0]); i++) {
            reset();
            mock.device.usb_release = revisions[revision];
            mock.state = &fixture_states[i];
            assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == 0);
            assert(snapshot.paired_count == mock.state->count);
            assert(mock.writes == 9 && mock.reads == 9 && mock.closes == 1);
        }
    }
}

static int confirm(void *context, enum drf_action action,
                   const struct drf_snapshot *snapshot,
                   const struct drf_slot *device, uint64_t deadline) {
    (void)context;
    (void)snapshot;

    assert(action == mock.action && mock.locked && mock.closes == 0);
    assert(mock.queries == 9 && mock.mutations == 0);
    assert(device->kind == mock.kind);
    assert((deadline > 0) == (action == DRF_ACTION_PAIR));

    mock.confirmations++;
    return 0;
}

static void test_native_actions_and_guards(void) {
    struct drf_action_callbacks callbacks = {.confirm = confirm};
    struct drf_action_result result;
    struct drf_snapshot snapshot;

    reset();
    mock.guard = 1;
    assert(drf_receiver_slots("/dev/hidraw2", &snapshot) == -EINVAL);
    assert(mock.writes == 0 && mock.closes == 1);

    for (unsigned int guard = 1; guard <= 7; guard++) {
        reset();
        mock.guard = (int)guard;
        mock.action = DRF_ACTION_UNPAIR;
        assert(drf_receiver_unpair("/dev/hidraw2", 2, &callbacks, &result) ==
               -EINVAL);
        assert(mock.writes == 0 && mock.closes == 1);

        reset();
        mock.guard = (int)guard;
        mock.action = DRF_ACTION_PAIR;
        assert(drf_receiver_pair("/dev/hidraw2", DRF_SLOT_MOUSE, 30000,
                                 &callbacks, &result) == -EINVAL);
        assert(mock.writes == 0 && mock.closes == 1);
    }

    for (unsigned int lost_ack = 0; lost_ack < 2; lost_ack++) {
        reset();
        mock.device.usb_release = 0x0240;
        mock.action = DRF_ACTION_UNPAIR;
        mock.kind = DRF_SLOT_MOUSE;
        mock.after = &fixture_states[1];
        mock.acknowledgement_error = lost_ack ? ETIMEDOUT : 0;
        assert(drf_receiver_unpair("/dev/hidraw2", 2, &callbacks, &result) ==
               0);
        assert(result.verified && mock.mutations == 1);
        assert(mock.writes == 28 && mock.reads == 28 && mock.closes == 1);

        for (enum drf_slot_kind kind = DRF_SLOT_KEYBOARD;
             kind <= DRF_SLOT_MOUSE; kind++) {
            reset();
            mock.device.usb_release = 0x0240;
            mock.action = DRF_ACTION_PAIR;
            mock.kind = kind;
            mock.state = &fixture_states[kind == DRF_SLOT_MOUSE ? 2 : 3];
            mock.after = &fixture_states[kind == DRF_SLOT_MOUSE ? 3 : 4];
            mock.acknowledgement_error = lost_ack ? ETIMEDOUT : 0;
            assert(drf_receiver_pair("/dev/hidraw2", kind, 30000, &callbacks,
                                     &result) == 0);
            assert(result.verified && result.device.kind == kind);
            assert(mock.mutations == 1 && mock.closes == 1);
            assert(mock.input_reads == 1 && mock.status_polls == 2);
        }
    }

    for (unsigned int failure = 0; failure < 4; failure++) {
        reset();
        mock.action = DRF_ACTION_PAIR;
        mock.kind = DRF_SLOT_MOUSE;
        mock.state = &fixture_states[2];
        mock.after = &fixture_states[3];
        switch (failure) {
        case 0:
            mock.bad_descriptor = 1;
            break;
        case 1:
            mock.lock_busy = 1;
            break;
        case 2:
            mock.input_short = 1;
            break;
        case 3:
            mock.input_hup = 1;
            break;
        }

        int expected[] = {-EOPNOTSUPP, -EBUSY, -EMSGSIZE, -ENODEV};
        assert(drf_receiver_pair("/dev/hidraw2", DRF_SLOT_MOUSE, 30000,
                                 &callbacks, &result) == expected[failure]);
        assert(mock.mutations == 0 && !result.change_attempted);
        assert(mock.closes == 1);
    }
}

int main(void) {
    test_selection();
    test_receiver_listing();
    test_guard_and_transport();
    test_native_actions_and_guards();

    puts("receiver: selection, refusal before writes, locking, short "
         "transfers, operation guards, and action replay passed");
    return 0;
}
