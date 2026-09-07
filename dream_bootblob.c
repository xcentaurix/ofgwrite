/* Dream DM820/DM7080 boot container and bounded bank A writer.
 * GPL-2.0-or-later. Container layout verified against mkbootblob a461d519
 * and a byte-for-byte capture of a DM820 boot image. No external helpers.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dream_bootblob.h"

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int dream_bootblob_parse(const unsigned char *header, size_t header_size,
                         struct dream_bootblob_parts *parts)
{
    uint32_t asize, ksize;
    size_t i;
    if (!header || header_size < 512 || !parts)
        goto invalid;
    asize = get_le32(header);
    ksize = get_le32(header + 16);
    /* Only the standard animation + kernel container can be restored
     * losslessly through the existing flash-kernel/ofgwrite paths.
     */
    if (!asize || !ksize || asize > DREAM_BANK_SIZE || ksize > DREAM_BANK_SIZE ||
        asize % 4096 || ksize % 4096 || 512ULL + asize + ksize > DREAM_BANK_SIZE ||
        get_le32(header + 4) != 1 || get_le32(header + 8) != 0x10000000U - asize ||
        get_le32(header + 12) != 8 || get_le32(header + 20) != 1 + asize / 512 ||
        get_le32(header + 24) != 0x1000 || get_le32(header + 28) != 1)
        goto invalid;
    for (i = 32; i < 512; ++i)
        if (header[i])
            goto invalid;
    parts->animation_offset = 512;
    parts->animation_size = asize;
    parts->kernel_offset = 512 + asize;
    parts->kernel_size = ksize;
    parts->blob_size = 512 + asize + ksize;
    return 1;
invalid:
    errno = EINVAL;
    return 0;
}

static int read_at(int fd, unsigned char *data, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size) {
        size_t count = size - done;
        ssize_t n;
        if (count > 65536)
            count = 65536;
        n = pread(fd, data + done, count, offset + (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return 0;
        }
        done += (size_t)n;
    }
    return 1;
}

int dream_bootblob_read_a(int fd, unsigned char **blob, size_t *size)
{
    unsigned char buffer[65536] __attribute__((aligned(4096)));
    unsigned char header[512];
    struct dream_bootblob_parts parts;
    unsigned char *data;
    void *aligned = NULL;
    size_t done;
    int error;
    *blob = NULL;
    *size = 0;
    if (!read_at(fd, buffer, 512, DREAM_BANK_A_OFFSET) ||
        !dream_bootblob_parse(buffer, 512, &parts))
        return 0;
    memcpy(header, buffer, 512);
    error = posix_memalign(&aligned, 4096, parts.blob_size);
    if (error) { errno = error; return 0; }
    data = aligned;
    if (!read_at(fd, data, parts.blob_size, DREAM_BANK_A_OFFSET))
        goto fail;
    if (memcmp(header, data, 512)) { errno = EIO; goto fail; }
    /* A second direct read rejects a bank changing during the backup. */
    for (done = 0; done < parts.blob_size;) {
        size_t count = parts.blob_size - done;
        if (count > sizeof(buffer))
            count = sizeof(buffer);
        if (!read_at(fd, buffer, count, DREAM_BANK_A_OFFSET + (off_t)done))
            goto fail;
        if (memcmp(buffer, data + done, count)) { errno = EIO; goto fail; }
        done += count;
    }
    *blob = data;
    *size = parts.blob_size;
    return 1;
fail:
    free(data);
    return 0;
}

static void put_le32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void entry(unsigned char *p, size_t size, unsigned sector,
                  uint32_t destination, unsigned type)
{
    put_le32(p, (uint32_t)size);
    put_le32(p + 4, sector);
    put_le32(p + 8, destination);
    put_le32(p + 12, type);
}

int dream_bootblob_create(const void *kernel, size_t kernel_size,
                         const void *animation, size_t animation_size,
                         unsigned char **blob, size_t *blob_size)
{
    size_t ksize, asize, total;
    unsigned char *data;
    void *aligned = NULL;
    int error;

    *blob = NULL;
    *blob_size = 0;
    if (!kernel || !animation || kernel_size < 4096 || !animation_size) {
        errno = EINVAL;
        return 0;
    }
    if (kernel_size > DREAM_BANK_SIZE || animation_size > DREAM_BANK_SIZE) {
        errno = EFBIG;
        return 0;
    }
    ksize = (kernel_size + 4095) & ~(size_t)4095;
    asize = (animation_size + 4095) & ~(size_t)4095;
    total = 512 + asize + ksize;
    if (total > DREAM_BANK_SIZE) {
        errno = EFBIG;
        return 0;
    }
    /* Also suitable for the block device's O_DIRECT I/O. */
    error = posix_memalign(&aligned, 4096, total);
    if (error) {
        errno = error;
        return 0;
    }
    data = aligned;
    memset(data, 0, total);

    /* Sector positions are relative to the beginning of the chosen bank.
     * Only payload lengths are aligned to 4 KiB; the table is 512 bytes.
     */
    entry(data, asize, 1, 0x10000000U - (uint32_t)asize, 8);
    entry(data + 16, ksize, 1 + (unsigned)(asize / 512), 0x1000, 1);
    memcpy(data + 512, animation, animation_size);
    memcpy(data + 512 + asize, kernel, kernel_size);
    *blob = data;
    *blob_size = total;
    return 1;
}

int dream_bootblob_write_a(int fd, const unsigned char *blob, size_t size,
                          void (*progress)(int))
{
    unsigned char verify[65536] __attribute__((aligned(4096)));
    size_t done = 0;

    if (!blob || size < 512 || size > DREAM_BANK_SIZE || size % 512) {
        errno = EINVAL;
        return 0;
    }
    while (done < size) {
        size_t count = size - done;
        ssize_t n;
        if (count > sizeof(verify))
            count = sizeof(verify);
        n = pwrite(fd, blob + done, count, DREAM_BANK_A_OFFSET + (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return 0;
        }
        done += (size_t)n;
        if (progress)
            progress((int)(done * 50 / size));
    }
    if (fsync(fd))
        return 0;

    done = 0;
    while (done < size) {
        size_t count = size - done;
        ssize_t n;
        if (count > sizeof(verify))
            count = sizeof(verify);
        n = pread(fd, verify, count, DREAM_BANK_A_OFFSET + (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || (n > 0 && memcmp(verify, blob + done, (size_t)n))) {
            if (n >= 0)
                errno = EIO;
            return 0;
        }
        done += (size_t)n;
        if (progress)
            progress(50 + (int)(done * 50 / size));
    }
    return 1;
}
