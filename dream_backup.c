/* Read-only export of the shared DM820/DM7080 kernel A. GPL-2.0-or-later. */
#include "ofgwrite.h"
#include "dream_kernel.h"
#include "dream_bootblob.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

static int save_file(int dir, const char *name, const void *data, size_t size, mode_t mode)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    size_t done = 0;
    int ok = 0;
    if (fd < 0)
        return 0;
    while (done < size) {
        ssize_t n = write(fd, (const unsigned char *)data + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto out;
        done += (size_t)n;
    }
    ok = !fchmod(fd, mode) && !fsync(fd);
out:
    if (close(fd))
        ok = 0;
    return ok;
}

int dream_kernel_export_blob(const unsigned char *blob, size_t size, const char *model,
                              const char *directory)
{
    struct dream_bootblob_parts parts;
    char release[128], name[192], target[160];
    const char postinst[] = "#!/bin/sh\n[ -n \"$D\" ] || exec flash-kernel /boot/vmlinux.bin\n";
    const char *dirs[] = { "rootfs", "rootfs/boot", "rootfs/usr", "rootfs/usr/share",
                           "rootfs/usr/share/fastboot" };
    int dir = -1, ok = 0;
    size_t i;
    if (!dream_kernel_model(model) || !dream_bootblob_parse(blob, size, &parts) ||
        size != parts.blob_size ||
        !dream_kernel_get_release(blob + parts.kernel_offset, parts.kernel_size,
                                  model, release, sizeof(release))) {
        my_printf("Dream: unsupported or invalid kernel A container\n");
        return 0;
    }
    /* Lengths in the container include padding. Preserve every byte: guessing
     * the original file length by trimming zeroes could truncate the kernel.
     */
    if (mkdir(directory, 0700) ||
        (dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) < 0)
        goto out;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if (mkdirat(dir, dirs[i], 0755))
            goto out;
    snprintf(target, sizeof(target), "vmlinux.bin-%s", release);
    snprintf(name, sizeof(name), "rootfs/boot/%s", target);
    if (!save_file(dir, "kernel.bin", blob + parts.kernel_offset, parts.kernel_size, 0644) ||
        !save_file(dir, name, blob + parts.kernel_offset, parts.kernel_size, 0644) ||
        symlinkat(target, dir, "rootfs/boot/vmlinux.bin") ||
        !save_file(dir, "rootfs/usr/share/fastboot/lcd_anim.bin",
                   blob + parts.animation_offset, parts.animation_size, 0644) ||
        !save_file(dir, "kernel-image.postinst", postinst, sizeof(postinst) - 1, 0755) ||
        fsync(dir))
        goto out;
    my_printf("Dream: exported kernel A (%s, %zu bytes including padding) and its LCD animation to %s\n",
              release, parts.kernel_size, directory);
    ok = 1;
out:
    if (!ok)
        my_printf("Dream: backup export failed (%s); output must be a new directory; discard incomplete output\n", strerror(errno));
    if (dir >= 0)
        close(dir);
    return ok;
}

int dream_kernel_backup_a(const char *directory)
{
    struct dream_bootblob_parts parts;
    struct utsname running;
    unsigned char *blob = NULL;
    size_t size = 0, version_size;
    char model[32], release[128], version[1024];
    FILE *f = fopen("/proc/stb/info/model", "r");
    int fd = -1, ok = 0;
    if (!f)
        return 0;
    ok = fgets(model, sizeof(model), f) != NULL;
    fclose(f);
    if (!ok)
        return 0;
    ok = 0;
    model[strcspn(model, "\r\n")] = '\0';
    if (!dream_kernel_model(model) || !dream_kernel_check_backup_device())
        return 0;
    fd = open("/dev/mmcblk0", O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0 || !dream_bootblob_read_a(fd, &blob, &size) ||
        !dream_bootblob_parse(blob, size, &parts)) {
        my_printf("Dream: cannot read a stable, supported kernel A container: %s\n", strerror(errno));
        goto out;
    }
    if (uname(&running) ||
        !dream_kernel_get_release(blob + parts.kernel_offset, parts.kernel_size,
                                  model, release, sizeof(release)) || strcmp(release, running.release)) {
        my_printf("Dream: kernel A release does not match the running kernel; backup aborted\n");
        goto out;
    }
    f = fopen("/proc/version", "r");
    if (!f)
        goto out;
    version_size = fread(version, 1, sizeof(version), f);
    ok = !ferror(f) && feof(f) && version_size && version_size < sizeof(version);
    fclose(f);
    if (ok) {
        const unsigned char *found = memmem(blob + parts.kernel_offset, parts.kernel_size,
                                            version, version_size);
        ok = found && found + version_size < blob + size && !found[version_size];
    }
    if (!ok) {
        my_printf("Dream: kernel A build banner does not match /proc/version; backup aborted\n");
        goto out;
    }
    /* Check the selector again after reading; never fall back to /boot. */
    ok = dream_kernel_check_backup_device() && dream_kernel_export_blob(blob, size, model, directory);
out:
    if (fd >= 0)
        close(fd);
    free(blob);
    return ok;
}
