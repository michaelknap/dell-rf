#define _GNU_SOURCE

#include "dell_rf.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

int drf_is_supported_id(uint16_t vid, uint16_t pid) {
    return vid == DELL_VID && pid == DELL_PID_UNIVERSAL_4503;
}

const char *drf_product_label(uint16_t pid) {
    switch (pid) {
    case DELL_PID_UNIVERSAL_4503:
        return "Dell Universal Receiver (4503)";
    default:
        return "Unknown HID";
    }
}

static void copy_ioctl_string(int fd, unsigned long req, char *dst,
                              size_t cap) {
    if (!cap)
        return;

    memset(dst, 0, cap);
    if (ioctl(fd, req, dst) < 0)
        dst[0] = '\0';
    dst[cap - 1] = '\0';
}

static int usb_attribute(int fd, const char *attribute, unsigned int maximum) {
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode))
        return -1;

    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/%s",
                     major(st.st_rdev), minor(st.st_rdev), attribute);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    FILE *f = fopen(path, "re");
    if (!f)
        return -1;

    unsigned int value = 0;
    char extra;
    int fields = fscanf(f, "%x %c", &value, &extra);
    fclose(f);
    return fields == 1 && value <= maximum ? (int)value : -1;
}

int drf_probe_fd(int fd, struct drf_device *out) {
    if (fd < 0 || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->interface_number = -1;
    out->usb_release = -1;

    struct hidraw_devinfo info = {0};
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        return -errno;
    }

    int dsz = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &dsz) < 0)
        dsz = -1;

    out->bus = info.bustype;
    out->vid = info.vendor;
    out->pid = info.product;
    out->descriptor_size = dsz;

    copy_ioctl_string(fd, HIDIOCGRAWNAME(sizeof(out->name)), out->name,
                      sizeof(out->name));
    copy_ioctl_string(fd, HIDIOCGRAWPHYS(sizeof(out->phys)), out->phys,
                      sizeof(out->phys));

    if (out->bus == BUS_USB) {
        out->interface_number = usb_attribute(fd, "../bInterfaceNumber", 0xff);
        out->usb_release = usb_attribute(fd, "../../bcdDevice", 0xffff);
    }

    return 0;
}

int drf_probe_path(const char *path, struct drf_device *out) {
    if (!path || !out)
        return -EINVAL;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    int r = drf_probe_fd(fd, out);
    if (!r)
        snprintf(out->path, sizeof(out->path), "%s", path);

    close(fd);
    return r;
}

int drf_read_descriptor_fd(int fd, uint8_t *buf, size_t cap, size_t *len) {
    if (fd < 0 || !buf || !len)
        return -EINVAL;

    int dsz = 0;
    if (ioctl(fd, HIDIOCGRDESCSIZE, &dsz) < 0) {
        return -errno;
    }
    if (dsz <= 0 || (size_t)dsz > cap || dsz > DELL_RF_MAX_DESC) {
        return -EMSGSIZE;
    }

    struct hidraw_report_descriptor rpt = {0};
    rpt.size = (uint32_t)dsz;
    if (ioctl(fd, HIDIOCGRDESC, &rpt) < 0) {
        return -errno;
    }
    if (rpt.size != (uint32_t)dsz)
        return -EMSGSIZE;

    memcpy(buf, rpt.value, dsz);
    *len = (size_t)dsz;
    return 0;
}

int drf_read_descriptor(const char *path, uint8_t *buf, size_t cap,
                        size_t *len) {
    if (!path || !buf || !len)
        return -EINVAL;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    int r = drf_read_descriptor_fd(fd, buf, cap, len);
    close(fd);
    return r;
}

void drf_hexdump(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 16) {
        printf("%04zx  ", i);
        size_t n = len - i < 16 ? len - i : 16;

        for (size_t j = 0; j < 16; j++) {
            if (j < n)
                printf("%02x ", buf[i + j]);
            else
                printf("   ");
            if (j == 7)
                putchar(' ');
        }

        printf(" |");
        for (size_t j = 0; j < n; j++) {
            unsigned char c = buf[i + j];
            putchar(c >= 32 && c < 127 ? c : '.');
        }
        puts("|");
    }
}
