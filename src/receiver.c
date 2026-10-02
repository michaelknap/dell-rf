#define _GNU_SOURCE

#include "dell_rf_actions.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct receiver_session {
    int fd;
    enum drf_action action;
    unsigned int removal_slot;
};

static int validate_interface(int fd) {
    struct drf_device device;
    int r = drf_probe_fd(fd, &device);
    if (r)
        return r;

    uint8_t descriptor[DELL_RF_MAX_DESC];
    size_t length = 0;
    r = drf_read_descriptor_fd(fd, descriptor, sizeof(descriptor), &length);
    if (r)
        return r;

    return drf_query_device_matches(&device, descriptor, length) ? 0
                                                                 : -EOPNOTSUPP;
}

int drf_list_query_receivers(drf_receiver_path_fn visit, void *context) {
    if (!visit)
        return -EINVAL;

    glob_t devices = {0};
    int gr = glob("/dev/hidraw*", 0, NULL, &devices);
    if (gr == GLOB_NOMATCH)
        return -ENODEV;
    if (gr != 0)
        return gr == GLOB_NOSPACE ? -ENOMEM : -EIO;

    int result = -ENODEV;
    for (size_t i = 0; i < devices.gl_pathc; i++) {
        int fd = open(devices.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        int r = validate_interface(fd);
        close(fd);
        if (r)
            continue;

        result = visit(context, devices.gl_pathv[i]);
        if (result) {
            if (result > 0)
                result = -EINVAL;
            break;
        }
    }

    globfree(&devices);
    return result;
}

struct receiver_selection {
    char *path;
    size_t capacity;
    int found;
};

static int select_receiver(void *context, const char *path) {
    struct receiver_selection *selection = context;
    if (selection->found)
        return -EEXIST;

    size_t length = strlen(path) + 1;
    if (length > selection->capacity)
        return -ENAMETOOLONG;

    memcpy(selection->path, path, length);
    selection->found = 1;
    return 0;
}

int drf_find_query_receiver(char *path, size_t capacity) {
    if (!path || !capacity)
        return -EINVAL;

    path[0] = '\0';
    struct receiver_selection selection = {.path = path, .capacity = capacity};
    int result = drf_list_query_receivers(select_receiver, &selection);
    if (result)
        path[0] = '\0';
    return result;
}

static int exchange(void *context, const uint8_t request[DRF_REPORT_SIZE],
                    uint8_t response[DRF_REPORT_SIZE], size_t *length) {
    struct receiver_session *session = context;
    if (!request || !response || !length)
        return -EINVAL;

    /* Query sessions cannot send mutations. A removal session can send only
     * its chosen slot, with the exact captured padding. */
    uint8_t expected[DRF_REPORT_SIZE];
    int r = drf_encode_query((enum drf_query)request[1], request[2], expected);
    if (r && session->action == DRF_ACTION_UNPAIR && request[1] == 0x06 &&
        request[2] == session->removal_slot)
        r = drf_encode_unpair(session->removal_slot, expected);
    if (r && session->action == DRF_ACTION_PAIR)
        r = drf_encode_pair_command(
            (enum drf_pair_command)request[1],
            request[1] == DRF_PAIR_SELECT ? request + 3 : NULL, expected);
    if (r || memcmp(request, expected, sizeof(expected)) != 0)
        return -EINVAL;

    uint8_t outgoing[DRF_REPORT_SIZE];
    memcpy(outgoing, request, sizeof(outgoing));
    r = ioctl(session->fd, HIDIOCSFEATURE(DRF_REPORT_SIZE), outgoing);
    if (r < 0)
        return -errno;
    if (r != DRF_REPORT_SIZE)
        return -EMSGSIZE;

    response[0] = DRF_REPORT_ID;
    r = ioctl(session->fd, HIDIOCGFEATURE(DRF_REPORT_SIZE), response);
    if (r < 0)
        return -errno;

    *length = (size_t)r;
    return 0;
}

static int wait_ms(void *context, unsigned int milliseconds) {
    (void)context;
    if (milliseconds > INT_MAX)
        return -EINVAL;

    return poll(NULL, 0, (int)milliseconds) < 0 ? -errno : 0;
}

static int now_ms(void *context, uint64_t *milliseconds) {
    (void)context;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -errno;

    *milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return 0;
}

static int receive(void *context, uint8_t report[DRF_REPORT_SIZE + 1],
                   size_t *length, unsigned int timeout_ms) {
    struct receiver_session *session = context;
    if (timeout_ms > INT_MAX)
        return -EINVAL;

    struct pollfd p = {.fd = session->fd, .events = POLLIN};
    int r = poll(&p, 1, (int)timeout_ms);
    if (r < 0)
        return -errno;
    if (!r)
        return -ETIMEDOUT;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -ENODEV;
    if (!(p.revents & POLLIN))
        return -EIO;

    ssize_t n = read(session->fd, report, DRF_REPORT_SIZE + 1);
    if (n < 0)
        return -errno;
    if (!n)
        return -ENODEV;

    *length = (size_t)n;
    return 0;
}

static int open_session(const char *path, struct receiver_session *session) {
    session->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (session->fd < 0)
        return -errno;

    int r = validate_interface(session->fd);
    if (!r && flock(session->fd, LOCK_EX | LOCK_NB) < 0)
        r = errno == EWOULDBLOCK ? -EBUSY : -errno;

    if (r)
        close(session->fd);
    return r;
}

int drf_receiver_slots(const char *path, struct drf_snapshot *out) {
    if (!path || !out)
        return -EINVAL;

    struct receiver_session session = {0};
    int r = open_session(path, &session);
    if (r)
        return r;

    if (!r)
        r = drf_read_snapshot(exchange, &session, out);

    close(session.fd);
    return r;
}

int drf_receiver_batteries(const char *path, struct drf_battery_snapshot *out) {
    if (!path || !out)
        return -EINVAL;

    struct receiver_session session = {0};
    int r = open_session(path, &session);
    if (r)
        return r;

    r = drf_read_batteries(exchange, &session, out);

    close(session.fd);
    return r;
}

int drf_receiver_unpair(const char *path, unsigned int slot,
                        const struct drf_action_callbacks *callbacks,
                        struct drf_action_result *result) {
    if (!result)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    if (!path || !callbacks || !callbacks->confirm || slot < 1 ||
        slot > DRF_SLOT_COUNT)
        return -EINVAL;

    struct receiver_session session = {.action = DRF_ACTION_UNPAIR,
                                       .removal_slot = slot};
    int r = open_session(path, &session);
    if (r)
        return r;

    struct drf_action_io io = {
        .context = &session, .exchange = exchange, .wait_ms = wait_ms};
    r = drf_unpair(&io, slot, callbacks, result);

    close(session.fd);
    return r;
}

int drf_receiver_pair(const char *path, enum drf_slot_kind kind,
                      unsigned int timeout_ms,
                      const struct drf_action_callbacks *callbacks,
                      struct drf_action_result *result) {
    if (!result)
        return -EINVAL;

    memset(result, 0, sizeof(*result));
    if (!path || !callbacks || !callbacks->confirm || !timeout_ms ||
        (kind != DRF_SLOT_MOUSE && kind != DRF_SLOT_KEYBOARD))
        return -EINVAL;

    struct receiver_session session = {.action = DRF_ACTION_PAIR};
    int r = open_session(path, &session);
    if (r)
        return r;

    struct drf_action_io io = {.context = &session,
                               .exchange = exchange,
                               .wait_ms = wait_ms,
                               .now_ms = now_ms,
                               .receive = receive};
    r = drf_pair(&io, kind, timeout_ms, callbacks, result);

    close(session.fd);
    return r;
}
