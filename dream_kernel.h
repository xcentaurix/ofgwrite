#ifndef DREAM_KERNEL_H
#define DREAM_KERNEL_H

#include <stddef.h>

int dream_kernel_model(const char *model);
int dream_kernel_check_device(void);
int dream_kernel_check_backup_device(void);
int dream_kernel_get_release(const unsigned char *data, size_t size, const char *model,
                              char *release, size_t release_size);
int dream_kernel_export_blob(const unsigned char *blob, size_t size, const char *model,
                              const char *directory);
int dream_kernel_backup_a(const char *directory);
int dream_kernel_prepare(const char *model, const char *kernel);
int dream_kernel_write(void);
void dream_kernel_cleanup(void);
int dream_kernel_running_root(char *device, size_t device_size,
                              char *subdir, size_t subdir_size);

#endif
