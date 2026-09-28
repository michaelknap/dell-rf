#ifndef DELL_RF_H
#define DELL_RF_H

#include <linux/hidraw.h>
#include <stddef.h>
#include <stdint.h>

#define DELL_VID 0x413c
#define DELL_PID_UNIVERSAL_4503 0x4503
#define DELL_RF_MAX_DESC 4096

struct drf_device {
    char path[256];
    char name[256];
    char phys[256];
    unsigned int bus;
    uint16_t vid;
    uint16_t pid;
    int interface_number;
    int usb_release;
    int descriptor_size;
};

int drf_is_supported_id(uint16_t vid, uint16_t pid);

const char *drf_product_label(uint16_t pid);

int drf_probe_path(const char *path, struct drf_device *out);

int drf_probe_fd(int fd, struct drf_device *out);

int drf_read_descriptor(const char *path, uint8_t *buf, size_t cap,
                        size_t *len);

int drf_read_descriptor_fd(int fd, uint8_t *buf, size_t cap, size_t *len);

void drf_hexdump(const uint8_t *buf, size_t len);

#endif
