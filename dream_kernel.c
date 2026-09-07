/* Static Dream bootblob preparation with built-in model-specific LCD payloads.
 * Only kernel.bin is read, before any rootfs or device modification.
 * GPL-2.0-or-later.
 */
#include "ofgwrite.h"
#include "dream_bootblob.h"
#include "dream_kernel.h"
#include "busybox/include/libbb.h"
#include "dream_lcd.h"
#include <linux/fs.h>
#include <dirent.h>
#include <sys/ioctl.h>

static unsigned char *prepared_blob;
static size_t prepared_size;

static unsigned char *load_input(const char *path, size_t *size)
{
    struct stat st;
    unsigned char *data = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 &&
        st.st_size <= DREAM_BANK_SIZE) {
        data = malloc((size_t)st.st_size);
        if (data && full_read(fd, data, (size_t)st.st_size) != st.st_size) {
            free(data);
            data = NULL;
        }
        if (data)
            *size = (size_t)st.st_size;
    }
    close(fd);
    return data;
}

int dream_kernel_model(const char *model)
{
    return model && (!strcmp(model, "dm820") || !strcmp(model, "dm7080"));
}

static int read_number(const char *path, unsigned long long *number)
{
    FILE *f = fopen(path, "r");
    int ok;
    if (!f)
        return 0;
    ok = fscanf(f, "%llu", number) == 1;
    fclose(f);
    return ok;
}

static int partitions_clear_banks(unsigned long long disk_sectors)
{
    DIR *dir = opendir("/sys/class/block");
    struct dirent *entry;
    char path[512];
    unsigned long long start, size;
    int ok = 1;
    if (!dir)
        return 0;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "mmcblk0p", 8))
            continue;
        snprintf(path, sizeof(path), "/sys/class/block/%s/start", entry->d_name);
        if (!read_number(path, &start)) { ok = 0; break; }
        snprintf(path, sizeof(path), "/sys/class/block/%s/size", entry->d_name);
        if (!read_number(path, &size) || start < DREAM_ROOT_START_SECTORS ||
            start > disk_sectors || !size || size > disk_sectors - start) {
            ok = 0;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static int check_device(int for_write)
{
    struct stat st;
    unsigned long long size = 0, start = 0, sectors = 0;
    unsigned char selector[5];
    int fd, sector_size = 0, read_only = 0, ok = 0;

    fd = open("/dev/mmcblk0", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        goto out;
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) ||
        ioctl(fd, BLKGETSIZE64, &size) || ioctl(fd, BLKSSZGET, &sector_size) ||
        ioctl(fd, BLKROGET, &read_only) || (for_write && read_only) ||
        sector_size != 512 || size < DREAM_ROOT_START_SECTORS * 512 ||
        !read_number("/sys/class/block/mmcblk0p1/start", &start) ||
        !read_number("/sys/class/block/mmcblk0p1/size", &sectors) ||
        start != DREAM_ROOT_START_SECTORS || !sectors ||
        sectors > size / 512 - start || !partitions_clear_banks(size / 512)) {
        my_printf("Dream: unrecognized eMMC layout; bank A requires rootfs p1 at 72 MiB\n");
        goto out;
    }
    if (pread(fd, selector, sizeof(selector), DREAM_SELECTOR_OFFSET) != sizeof(selector) ||
        memcmp(selector, "optA\0", sizeof(selector))) {
        my_printf("Dream: boot source is not A. B, C and unknown selectors are unsupported; no boot source will be changed\n");
        goto out;
    }
    my_printf("Dream: shared kernel bank A, /dev/mmcblk0 offset 8388608, limit 33554432 bytes; selector A preserved\n");
    ok = 1;
out:
    if (fd >= 0)
        close(fd);
    if (!ok)
        my_printf("Dream: kernel target validation failed\n");
    return ok;
}

int dream_kernel_check_device(void)
{
    return check_device(1);
}

int dream_kernel_check_backup_device(void)
{
    return check_device(0);
}

void dream_kernel_cleanup(void)
{
    free(prepared_blob);
    prepared_blob = NULL;
    prepared_size = 0;
}

int dream_kernel_get_release(const unsigned char *data, size_t size, const char *model,
                              char *release, size_t release_size)
{
    const char prefix[] = "Linux version ";
    char suffix[32];
    const unsigned char *p = data, *end = data + size, *release_end;
    size_t suffix_size;
    snprintf(suffix, sizeof(suffix), "-%s", model);
    suffix_size = strlen(suffix);
    while ((p = memmem(p, end - p, prefix, sizeof(prefix) - 1))) {
        p += sizeof(prefix) - 1;
        release_end = memchr(p, ' ', end - p);
        if (release_end && (size_t)(release_end - p) > suffix_size &&
            !memcmp(release_end - suffix_size, suffix, suffix_size)) {
            size_t len = release_end - p, i;
            if (len > 127 || (release && len >= release_size))
                return 0;
            for (i = 0; i < len; ++i)
                if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z') ||
                      (p[i] >= '0' && p[i] <= '9') || (p[i] && strchr("._+-", p[i]))))
                    return 0;
            if (release) {
                memcpy(release, p, len);
                release[len] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

int dream_kernel_prepare(const char *model, const char *kernel)
{
    unsigned char *input;
    const unsigned char *animation;
    size_t input_size = 0, animation_size = 0;
    int ok;

    if (!dream_kernel_model(model) || !kernel || !*kernel) {
        my_printf("Dream: -k requires kernel.bin containing the raw model-specific vmlinux.bin\n");
        return 0;
    }
    dream_kernel_cleanup();
    input = load_input(kernel, &input_size);
    if (!input || input_size < 4096 ||
        !dream_kernel_get_release(input, input_size, model, NULL, 0) ||
        !memcmp(input, "\177ELF", 4) ||
        (input[4] == 1 && !input[5] && !input[6] && !input[7] &&
         (input[12] == 8 || input[12] == 1) && !input[13] && !input[14] && !input[15])) {
        my_printf("Dream: kernel.bin must be a raw %s kernel; dummy, ELF and prebuilt bootblob inputs are not supported\n", model);
        free(input);
        return 0;
    }
    animation = dream_lcd_animation(model, &animation_size);
    ok = dream_bootblob_create(input, input_size, animation, animation_size,
                               &prepared_blob, &prepared_size);
    free(input);
    if (!ok) {
        my_printf("Dream: cannot create bank A bootblob: %s\n", strerror(errno));
        return 0;
    }
    my_printf("Dream: kernel.bin passed size/format/model checks; using built-in %s LCD animation (%zu bytes)\n",
              model, animation_size);
    my_printf("Dream: bootblob ready in RAM (%llu bytes); rootfs archive was not opened for kernel preparation\n",
              (unsigned long long)prepared_size);
    return 1;
}

static void progress(int percent)
{
    set_step_progress(percent);
}

int dream_kernel_write(void)
{
    int fd, result;
    struct stat st;
    if (!prepared_blob || !dream_kernel_check_device())
        return 0;
    /* Bypass the page cache so readback verifies device data after fsync. */
    fd = open("/dev/mmcblk0", O_RDWR | O_CLOEXEC | O_DIRECT);
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode)) {
        close(fd);
        return 0;
    }
    set_step("Writing and verifying Dream kernel A");
    result = dream_bootblob_write_a(fd, prepared_blob, prepared_size, progress);
    if (!result)
        my_printf("Dream: kernel A write/sync/readback failed: %s\n", strerror(errno));
    if (close(fd))
        result = 0;
    return result;
}

int dream_kernel_running_root(char *device, size_t device_size,
                              char *subdir, size_t subdir_size)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[8192], root[1024], mountpoint[1024], source[1024], fs[32];
    int ok = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *sep = strstr(line, " - ");
        if (sscanf(line, "%*u %*u %*u:%*u %1023s %1023s", root, mountpoint) != 2 ||
            strcmp(mountpoint, "/") || !sep ||
            sscanf(sep + 3, "%31s %1023s", fs, source) != 2)
            continue;
        if (strcmp(fs, "ext4") || strncmp(source, "/dev/", 5) ||
            strlen(source) >= device_size || strlen(root) >= subdir_size)
            break;
        if (strcmp(root, "/")) {
            const char *suffix;
            if (strncmp(root, "/linuxrootfs", 12))
                break;
            suffix = root + 12;
            if (!*suffix || strspn(suffix, "0123456789") != strlen(suffix))
                break;
        }
        char *resolved = realpath(source, NULL);
        if (!resolved || strlen(resolved) >= device_size) {
            free(resolved);
            break;
        }
        strcpy(device, resolved);
        free(resolved);
        strcpy(subdir, root + 1);
        ok = 1;
        break;
    }
    fclose(f);
    if (!ok)
        my_printf("Dream: cannot determine actual root/Chkroot directory from mountinfo\n");
    return ok;
}
