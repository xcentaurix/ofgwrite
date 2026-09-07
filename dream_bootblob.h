#ifndef DREAM_BOOTBLOB_H
#define DREAM_BOOTBLOB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DREAM_BANK_A_OFFSET ((off_t)8 * 1024 * 1024)
#define DREAM_BANK_SIZE (32U * 1024 * 1024)
#define DREAM_ROOT_START_SECTORS 147456ULL
#define DREAM_SELECTOR_OFFSET ((off_t)64 * 512)

struct dream_bootblob_parts {
    size_t animation_offset, animation_size;
    size_t kernel_offset, kernel_size;
    size_t blob_size;
};

int dream_bootblob_parse(const unsigned char *header, size_t header_size,
                         struct dream_bootblob_parts *parts);
int dream_bootblob_read_a(int fd, unsigned char **blob, size_t *size);

int dream_bootblob_create(const void *kernel, size_t kernel_size,
                         const void *animation, size_t animation_size,
                         unsigned char **blob, size_t *blob_size);
int dream_bootblob_write_a(int fd, const unsigned char *blob, size_t size,
                          void (*progress)(int));

#endif
