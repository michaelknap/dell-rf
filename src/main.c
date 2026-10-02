#define _GNU_SOURCE

#include "dell_rf.h"
#include "dell_rf_actions.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PAIR_WINDOW_SECONDS 30u

static volatile sig_atomic_t stop;
static volatile sig_atomic_t received_signal;

static void on_signal(int sig) {
    stop = 1;
    received_signal = sig;
}

static void usage(FILE *f) {
    fprintf(f, "Usage: dell-rf <command> [args]\n\n"
               "Commands:\n"
               "  --version               Show version\n"
               "  info <hidraw>           Show receiver/interface identity\n"
               "  descriptor <hidraw>     Dump raw HID report descriptor\n"
               "  monitor <hidraw>        Read input reports without sending "
               "anything\n"
               "  pair mouse|keyboard [hidraw]\n"
               "                           Discover and confirm one device\n"
               "  slots [hidraw]           List paired devices on a validated "
               "4503 receiver\n"
               "  battery [hidraw]         Read paired-device battery levels\n"
               "  unpair <slot> [hidraw]   Confirm removal of one paired "
               "device (slots 1-6)\n");
}

static int cmd_info(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 1;
    }

    struct drf_device d;
    int r = drf_probe_fd(fd, &d);
    uint8_t descriptor[DELL_RF_MAX_DESC];
    size_t length = 0;
    if (!r)
        r = drf_read_descriptor_fd(fd, descriptor, sizeof(descriptor), &length);

    close(fd);
    if (r) {
        fprintf(stderr, "%s: %s\n", path, strerror(-r));
        return 1;
    }

    int queryable = drf_query_device_matches(&d, descriptor, length);
    printf("Path:       %s\n", path);
    printf("USB/HID:    %04x:%04x\n", d.vid, d.pid);
    printf("Queryable:  %s\n", queryable ? "yes" : "no");
    printf("Family:     %s\n", drf_is_supported_id(d.vid, d.pid)
                                   ? drf_product_label(d.pid)
                                   : "Unknown HID");
    printf("Bus type:   %u\n", d.bus);
    printf("Name:       %s\n", d.name[0] ? d.name : "(unknown)");
    printf("Physical:   %s\n", d.phys[0] ? d.phys : "(unknown)");
    printf("Descriptor: %d bytes\n", d.descriptor_size);
    if (d.interface_number >= 0)
        printf("Interface:  %d\n", d.interface_number);
    if (d.usb_release >= 0)
        printf("USB release: %04x\n", (unsigned int)d.usb_release);

    return queryable ? 0 : 3;
}

static int cmd_descriptor(const char *path) {
    uint8_t buf[DELL_RF_MAX_DESC];
    size_t len = 0;
    int r = drf_read_descriptor(path, buf, sizeof(buf), &len);
    if (r) {
        fprintf(stderr, "%s: %s\n", path, strerror(-r));
        return 1;
    }

    printf("HID report descriptor: %zu bytes\n", len);
    drf_hexdump(buf, len);
    return 0;
}

static int cmd_monitor(const char *path) {
    struct drf_device d;
    int r = drf_probe_path(path, &d);
    if (r) {
        fprintf(stderr, "%s: %s\n", path, strerror(-r));
        return 1;
    }
    if (!drf_is_supported_id(d.vid, d.pid)) {
        fprintf(stderr, "refusing to monitor unsupported device %04x:%04x\n",
                d.vid, d.pid);
        return 3;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(
        stderr,
        "Monitoring %s read-only; Ctrl-C to stop. No reports will be sent.\n",
        path);

    while (!stop) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int pr = poll(&p, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            close(fd);
            return 1;
        }
        if (pr == 0)
            continue;

        if (p.revents & POLLIN) {
            uint8_t b[256];
            ssize_t n = read(fd, b, sizeof(b));
            if (n > 0) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                printf("%lld.%06ld  %zd  ", (long long)ts.tv_sec,
                       ts.tv_nsec / 1000, n);
                for (ssize_t i = 0; i < n; i++)
                    printf("%02x%s", b[i], i + 1 == n ? "" : " ");
                putchar('\n');
                fflush(stdout);
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                perror("read");
                close(fd);
                return 1;
            }
        }
    }

    close(fd);
    return 0;
}

static int print_receiver_path(void *context, const char *path) {
    FILE *out = context;
    return fprintf(out, "  %s\n", path) < 0 ? -EIO : 0;
}

static int receiver_error(const char *command, const char *path, int r) {
    switch (-r) {
    case ENODEV:
        fprintf(stderr, "No accessible 4503 management interface found.\n"
                        "Check the HID interface, permissions and VM "
                        "passthrough.\n");
        break;
    case EEXIST: {
        fprintf(stderr, "Multiple validated receivers found; specify "
                        "an explicit /dev/hidrawN path.\n"
                        "Compatible receiver paths:\n");

        int list_error = drf_list_query_receivers(print_receiver_path, stderr);
        if (list_error)
            fprintf(stderr, "Could not list compatible receiver paths: %s.\n",
                    strerror(-list_error));
        break;
    }
    case EOPNOTSUPP:
        fprintf(stderr, "Receiver identity or management descriptor does "
                        "not match.\n"
                        "Required: USB 413c:4503, interface 2, matching "
                        "descriptor.\n");
        break;
    case EACCES:
        fprintf(stderr,
                "%s: permission denied; install the included udev rule or "
                "run with appropriate access.\n",
                path);
        break;
    case EAGAIN:
        fprintf(stderr, "Pairing state changed or was inconsistent during "
                        "enumeration; retry after it settles.\n");
        break;
    case EBUSY:
        fprintf(stderr,
                "%s: another dell-rf process holds the receiver lock.\n", path);
        break;
    default:
        fprintf(stderr, "%s failed: %s\n", command, strerror(-r));
        break;
    }

    return r == -EOPNOTSUPP ? 3 : 1;
}

static int cmd_slots(const char *path) {
    char selected[256];
    struct drf_snapshot snapshot;
    int r = 0;
    if (!path) {
        r = drf_find_query_receiver(selected, sizeof(selected));
        path = selected;
    }

    if (!r)
        r = drf_receiver_slots(path, &snapshot);
    if (r)
        return receiver_error("slots", path, r);

    printf("Receiver: %s (%s)\n", snapshot.receiver.name, path);
    printf("Paired:   %u/%u\n\n", snapshot.paired_count, DRF_SLOT_COUNT);
    printf("Slot  Type      Model\n");

    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        const struct drf_slot *slot = &snapshot.slots[i];
        const char *kind = slot->kind == DRF_SLOT_KEYBOARD ? "keyboard"
                           : slot->kind == DRF_SLOT_MOUSE  ? "mouse"
                                                           : "empty";
        printf("%u     %-8s  %s\n", slot->number, kind, slot->model);
    }

    return 0;
}

static int cmd_battery(const char *path) {
    char selected[256];
    struct drf_battery_snapshot result;
    int r = 0;
    if (!path) {
        r = drf_find_query_receiver(selected, sizeof(selected));
        path = selected;
    }

    if (!r)
        r = drf_receiver_batteries(path, &result);
    if (r)
        return receiver_error("battery", path, r);

    printf("Receiver: %s (%s)\n", result.snapshot.receiver.name, path);
    if (!result.snapshot.paired_count) {
        puts("No paired devices.");
        return 0;
    }

    printf("\nSlot  Type      Model                 Battery\n");
    for (size_t i = 0; i < DRF_SLOT_COUNT; i++) {
        const struct drf_slot *slot = &result.snapshot.slots[i];
        if (slot->kind == DRF_SLOT_EMPTY)
            continue;

        const char *kind =
            slot->kind == DRF_SLOT_KEYBOARD ? "keyboard" : "mouse";
        printf("%u     %-8s  %-20s  ", slot->number, kind, slot->model);
        if (!(slot->capabilities & DRF_CAP_BATTERY))
            puts("unsupported");
        else if (result.percentages[i] == DRF_BATTERY_UNKNOWN)
            puts("unavailable");
        else
            printf("%u%%\n", result.percentages[i]);
    }

    return 0;
}

static const char *kind_name(enum drf_slot_kind kind) {
    return kind == DRF_SLOT_MOUSE ? "mouse" : "keyboard";
}

struct terminal_context {
    int fd;
    const char *path;

    enum drf_slot_kind kind;

    int error;
};

static int monotonic_ms(uint64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -errno;

    *out = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    return 0;
}

static int read_confirmation(int fd, const char *expected,
                             uint64_t deadline_ms) {
    char answer[64];
    size_t used = 0;
    int overflow = 0;

    for (;;) {
        if (stop)
            return -ECANCELED;

        int timeout = 1000;
        if (deadline_ms) {
            uint64_t now = 0;
            int r = monotonic_ms(&now);
            if (r)
                return r;
            if (now >= deadline_ms)
                return -ETIMEDOUT;
            if (deadline_ms - now < (uint64_t)timeout)
                timeout = (int)(deadline_ms - now);
        }

        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (!r)
            continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -ECANCELED;
        if (!(p.revents & POLLIN))
            return -EIO;

        char byte;
        ssize_t length = read(fd, &byte, 1);
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -errno;
        }
        if (!length)
            return -ECANCELED;

        if (byte == '\n' || byte == '\r')
            break;
        if (used < sizeof(answer) - 1)
            answer[used++] = byte;
        else
            overflow = 1;
    }

    answer[used] = '\0';
    return !overflow && strcmp(answer, expected) == 0 ? 0 : -ECANCELED;
}

static int confirm_action(void *context, enum drf_action action,
                          const struct drf_snapshot *snapshot,
                          const struct drf_slot *device, uint64_t deadline_ms) {
    struct terminal_context *terminal = context;
    if (stop)
        return -ECANCELED;
    if (tcflush(terminal->fd, TCIFLUSH) < 0)
        return -errno;

    char expected[32];
    if (action == DRF_ACTION_UNPAIR)
        snprintf(expected, sizeof(expected), "unpair %u", device->number);
    else
        snprintf(expected, sizeof(expected), "pair");

    int r = dprintf(terminal->fd,
                    "\nReceiver: %s (%s)\n"
                    "Device:   %s (%s), ID %02x:%02x:%02x\n",
                    snapshot->receiver.name, terminal->path, device->model,
                    kind_name(device->kind), device->opaque_id[0],
                    device->opaque_id[1], device->opaque_id[2]);
    if (r < 0)
        return -errno;

    if (action == DRF_ACTION_UNPAIR &&
        dprintf(terminal->fd, "Remove this device from slot %u.\n",
                device->number) < 0)
        return -errno;
    if (action == DRF_ACTION_PAIR &&
        dprintf(terminal->fd, "Pair this device. The receiver may finish "
                              "independently after approval.\n") < 0)
        return -errno;

    if (dprintf(terminal->fd,
                "Type '%s' to confirm; anything else cancels: ", expected) < 0)
        return -errno;

    r = read_confirmation(terminal->fd, expected, deadline_ms);
    if (r)
        (void)dprintf(terminal->fd, "\n");
    return r;
}

static int action_cancelled(void *context) {
    struct terminal_context *terminal = context;
    return stop || terminal->error;
}

static void search_started(void *context) {
    struct terminal_context *terminal = context;
    if (dprintf(terminal->fd,
                "Searching for a %s for %u seconds.\n"
                "Turn it off, hold a button/key, then turn it on.\n"
                "Release the button/key when the device appears.\n"
                "Ctrl+C ends the local search before confirmation.\n",
                kind_name(terminal->kind), PAIR_WINDOW_SECONDS) < 0)
        terminal->error = -errno;
}

static int positive_integer(const char *text, unsigned int maximum,
                            unsigned int *out) {
    if (!text || !text[0])
        return -EINVAL;
    for (size_t i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -EINVAL;
    }

    errno = 0;
    unsigned long value = strtoul(text, NULL, 10);
    if (errno == ERANGE || value == 0 || value > maximum)
        return -EINVAL;

    *out = (unsigned int)value;
    return 0;
}

static int cmd_action(int argc, char **argv, enum drf_action action) {
    const char *command = action == DRF_ACTION_PAIR ? "pair" : "unpair";
    enum drf_slot_kind kind = DRF_SLOT_EMPTY;
    unsigned int slot = 0;
    const char *path = NULL;

    if (argc != 3 && argc != 4)
        goto invalid;
    if (action == DRF_ACTION_UNPAIR) {
        if (positive_integer(argv[2], DRF_SLOT_COUNT, &slot))
            goto invalid;
    } else if (strcmp(argv[2], "mouse") == 0) {
        kind = DRF_SLOT_MOUSE;
    } else if (strcmp(argv[2], "keyboard") == 0) {
        kind = DRF_SLOT_KEYBOARD;
    } else {
        goto invalid;
    }

    if (argc == 4) {
        if (!argv[3][0] || argv[3][0] == '-')
            goto invalid;
        path = argv[3];
    }

    int tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (tty < 0 || !isatty(tty)) {
        if (tty >= 0)
            close(tty);
        fprintf(stderr,
                "%s requires an interactive controlling terminal; "
                "piped input cannot approve a device change.\n",
                command);
        return 1;
    }

    char selected[256];
    int r = 0;
    if (!path) {
        r = drf_find_query_receiver(selected, sizeof(selected));
        path = selected;
    }
    if (r) {
        close(tty);
        return receiver_error(command, path, r);
    }

    stop = 0;
    received_signal = 0;
    struct sigaction handler = {.sa_handler = on_signal};
    sigemptyset(&handler.sa_mask);
    if (sigaction(SIGINT, &handler, NULL) < 0 ||
        sigaction(SIGTERM, &handler, NULL) < 0) {
        r = -errno;
        close(tty);
        return receiver_error(command, path, r);
    }

    struct terminal_context terminal = {.fd = tty, .path = path, .kind = kind};
    struct drf_action_callbacks callbacks = {.context = &terminal,
                                             .confirm = confirm_action,
                                             .cancelled = action_cancelled,
                                             .search_started = search_started};

    struct drf_action_result result;
    if (action == DRF_ACTION_PAIR)
        r = drf_receiver_pair(path, kind, PAIR_WINDOW_SECONDS * 1000,
                              &callbacks, &result);
    else
        r = drf_receiver_unpair(path, slot, &callbacks, &result);

    close(tty);
    if (terminal.error && !result.change_attempted)
        r = terminal.error;

    if (!r) {
        printf("%s %s (%s) %s slot %u; %u/%u paired.\n",
               action == DRF_ACTION_PAIR ? "Paired" : "Unpaired",
               result.device.model, kind_name(result.device.kind),
               action == DRF_ACTION_PAIR ? "in" : "from", result.device.number,
               result.snapshot.paired_count, DRF_SLOT_COUNT);
        if (result.acknowledgement_error)
            fprintf(stderr,
                    "Receiver acknowledgement failed (%s); slot "
                    "readback verified the change.\n",
                    strerror(-result.acknowledgement_error));
        return 0;
    }

    if (result.change_attempted) {
        fprintf(stderr,
                "%s outcome is unverified: %s.\n"
                "A device change was attempted and may still take "
                "effect.\n"
                "Run `dell-rf slots` before another pairing change; "
                "the request was not retried.\n",
                command, strerror(-r));
        return received_signal ? 128 + received_signal : 1;
    }

    switch (-r) {
    case ECANCELED:
        fprintf(stderr, "%s cancelled before sending a device change.\n",
                command);
        return received_signal ? 128 + received_signal : 1;
    case ETIMEDOUT:
        fprintf(stderr, "Pairing window expired before device selection.\n");
        return 1;
    case EALREADY:
        fprintf(stderr,
                "%s is already paired in slot %u; unpair that slot "
                "first.\n",
                result.device.model, result.device.number);
        return 1;
    case ENOSPC:
        fprintf(stderr, "Receiver has no empty pairing slots.\n");
        return 1;
    case ENOENT:
        if (result.device.number == slot && slot) {
            fprintf(stderr, "Slot %u is empty.\n", slot);
            return 1;
        }
        break;
    }

    return receiver_error(command, path, r);

invalid:
    fprintf(stderr, "Invalid %s arguments.\n", command);
    usage(stderr);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    if (!strcmp(argv[1], "--version") && argc == 2) {
        puts("dell-rf " DRF_VERSION);
        return 0;
    }

    if (!strcmp(argv[1], "info") && argc == 3)
        return cmd_info(argv[2]);
    if (!strcmp(argv[1], "descriptor") && argc == 3)
        return cmd_descriptor(argv[2]);
    if (!strcmp(argv[1], "monitor") && argc == 3)
        return cmd_monitor(argv[2]);
    if (!strcmp(argv[1], "pair"))
        return cmd_action(argc, argv, DRF_ACTION_PAIR);
    if (!strcmp(argv[1], "slots") && (argc == 2 || argc == 3))
        return cmd_slots(argc == 3 ? argv[2] : NULL);
    if (!strcmp(argv[1], "battery") &&
        (argc == 2 || (argc == 3 && argv[2][0] && argv[2][0] != '-')))
        return cmd_battery(argc == 3 ? argv[2] : NULL);
    if (!strcmp(argv[1], "unpair"))
        return cmd_action(argc, argv, DRF_ACTION_UNPAIR);

    usage(stderr);
    return 2;
}
