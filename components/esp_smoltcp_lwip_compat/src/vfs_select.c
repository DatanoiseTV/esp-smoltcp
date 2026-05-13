/*
 * VFS registration for the smoltcp socket layer.
 *
 * Why: IDF's VFS select() dispatches via a function-pointer table. The
 * linker `--wrap=lwip_select` we use elsewhere only rewrites call sites,
 * not address-of expressions — so VFS calls the *real* lwip_select() and
 * bypasses our shim entirely. The fix is to register our own VFS for
 * the BSD-socket FD range with our own socket_select callback.
 *
 * esp_vfs_register_fd_range() clobbers any prior owner of the given FD
 * range (vfs.c:552-560: "esp_vfs_register_fd_range cannot set fd %d
 * (used by other VFS)" emits a warning but proceeds, reassigning the
 * FD to us). We register *after* lwIP, so the range becomes ours.
 *
 * Effect: drops the v0.1 `CONFIG_VFS_SUPPORT_SELECT=n` requirement.
 */

#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "esp_private/socket.h"   /* esp_vfs_register_fd_range declaration */
#include "lwip/sockets.h"     /* LWIP_SOCKET_OFFSET, MAX_FDS */
#include "lwip/sys.h"         /* sys_sem_t, sys_thread_sem_get */

#include "fd_table.h"

static const char *TAG = "smoltcp_vfs";

/* The wrap functions live in our other translation units. */
extern int     __wrap_lwip_select (int, fd_set*, fd_set*, fd_set*, struct timeval*);
extern ssize_t __wrap_lwip_read   (int, void*, size_t);
extern ssize_t __wrap_lwip_write  (int, const void*, size_t);
extern int     __wrap_lwip_close  (int);
extern int     __wrap_lwip_fcntl  (int, int, int);
extern int     __wrap_lwip_ioctl  (int, long, void*);

/* Provided by FreeRTOS port (not in lwip/sys.h, but in the lwIP port). */
extern int sys_sem_signal_isr(sys_sem_t *sem);

/* ---------------------------------------------------------------------- */
/* VFS-shape adapters around our shim functions                            */

static int smoltcp_vfs_fstat(int fd, struct stat *st)
{
    if (!st || !fd_is_socket(fd)) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFSOCK;
    return 0;
}

static int smoltcp_vfs_ioctl_va(int fd, int cmd, va_list args)
{
    return __wrap_lwip_ioctl(fd, cmd, va_arg(args, void *));
}

/* ---------------------------------------------------------------------- */
/* Select-callback set                                                     */

static void smoltcp_stop_socket_select(void *sem)
{
    sys_sem_signal((sys_sem_t *)sem);
}

static void smoltcp_stop_socket_select_isr(void *sem, BaseType_t *woken)
{
    if (sys_sem_signal_isr((sys_sem_t *)sem) && woken) {
        *woken = pdTRUE;
    }
}

static void *smoltcp_get_socket_select_semaphore(void)
{
    /* Per-task semaphore from lwIP's port. We share it because non-socket
     * VFSes signal this same sem to wake our socket_select (see start_select
     * in any non-socket VFS). The current __wrap_lwip_select poll loop
     * ignores it; task #2 of the v0.2 roadmap will integrate event-driven
     * wakeup. For now: the worst case is select() waits up to its inner
     * tick before noticing a non-socket FD became ready, which is the same
     * behaviour as before.
     */
    return (void *)sys_thread_sem_get();
}

/* ---------------------------------------------------------------------- */
/* Static op tables                                                        */

#ifdef CONFIG_VFS_SUPPORT_SELECT
static const esp_vfs_select_ops_t s_smoltcp_select_ops = {
    .socket_select               = &__wrap_lwip_select,
    .stop_socket_select          = &smoltcp_stop_socket_select,
    .stop_socket_select_isr      = &smoltcp_stop_socket_select_isr,
    .get_socket_select_semaphore = &smoltcp_get_socket_select_semaphore,
};
#endif

static const esp_vfs_fs_ops_t s_smoltcp_vfs = {
    .write  = &__wrap_lwip_write,
    .read   = &__wrap_lwip_read,
    .close  = &__wrap_lwip_close,
    .fstat  = &smoltcp_vfs_fstat,
    .fcntl  = &__wrap_lwip_fcntl,
    .ioctl  = &smoltcp_vfs_ioctl_va,
#ifdef CONFIG_VFS_SUPPORT_SELECT
    .select = &s_smoltcp_select_ops,
#endif
};

/* ---------------------------------------------------------------------- */

esp_err_t esp_smoltcp_vfs_register(void)
{
    /* Claim the same FD range lwIP claims. esp_vfs_register_fd_range()
     * unconditionally evicts the previous owner of any FD in the range
     * (see esp-idf vfs.c lines ~552). lwIP registers from a constructor,
     * so it's already in place by the time app_main runs; we just take
     * over. */
    esp_err_t err = esp_vfs_register_fd_range(&s_smoltcp_vfs,
                                              ESP_VFS_FLAG_STATIC,
                                              NULL,
                                              LWIP_SOCKET_OFFSET,
                                              MAX_FDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "VFS register_fd_range failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "registered for FDs [%d, %d) — took over from lwIP",
             LWIP_SOCKET_OFFSET, MAX_FDS);
    return ESP_OK;
}
