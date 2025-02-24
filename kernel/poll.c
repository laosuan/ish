#include <string.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "kernel/calls.h"

static int user_read_or_zero(addr_t addr, void *data, size_t size) {
    if (addr == 0)
        memset(data, 0, size);
    else if (user_read(addr, data, size))
        return _EFAULT;
    return 0;
}

#define SELECT_READ (POLL_READ | POLL_HUP | POLL_ERR)
#define SELECT_WRITE (POLL_WRITE | POLL_ERR)
#define SELECT_EX (POLL_PRI)
struct select_context {
    char *readfds;
    char *writefds;
    char *exceptfds;
};
static int select_event_callback(void *context, int types, union poll_fd_info info) {
    struct select_context *c = context;
    if (types & SELECT_READ)
        bit_set(info.fd, c->readfds);
    if (types & SELECT_WRITE)
        bit_set(info.fd, c->writefds);
    if (types & SELECT_EX)
        bit_set(info.fd, c->exceptfds);
    if (!(types & (SELECT_READ | SELECT_WRITE | SELECT_EX)))
        return 0;
    return 1;
}

static dword_t select_common(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, struct timespec *timeout_addr, const char *name) {
    size_t fdset_size = BITS_SIZE(nfds);
    char readfds[fdset_size];
    if (user_read_or_zero(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    char writefds[fdset_size];
    if (user_read_or_zero(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    char exceptfds[fdset_size];
    if (user_read_or_zero(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;

    STRACE("%s(%d, 0x%x, 0x%x, 0x%x, 0x%x {%lus %luns}) ",
           name, nfds, readfds_addr, writefds_addr, exceptfds_addr,
           timeout_addr, timeout_addr ? timeout_addr->tv_sec : 0,
           timeout_addr ? timeout_addr->tv_nsec : 0);

    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);

    for (fd_t i = 0; i < nfds; i++) {
        int events = 0;
        if (bit_test(i, readfds))
            events |= SELECT_READ;
        if (bit_test(i, writefds))
            events |= SELECT_WRITE;
        if (bit_test(i, exceptfds))
            events |= SELECT_EX;
        if (events != 0) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
            struct fd *fd = f_get(i);
            if (fd == NULL) {
                poll_destroy(poll);
                return _EBADF;
            }
            poll_add_fd(poll, fd, events, (union poll_fd_info) i);
        }
    }
    STRACE("...\n");

    memset(readfds, 0, fdset_size);
    memset(writefds, 0, fdset_size);
    memset(exceptfds, 0, fdset_size);
    struct select_context context = {readfds, writefds, exceptfds};
    int err = poll_wait(poll, select_event_callback, &context, timeout_addr);
    STRACE("%d end %s ", current->pid, name);
    for (fd_t i = 0; i < nfds; i++) {
        if (bit_test(i, readfds) || bit_test(i, writefds) || bit_test(i, exceptfds)) {
            STRACE("%d{%s%s%s} ", i,
                    bit_test(i, readfds) ? "r" : "",
                    bit_test(i, writefds) ? "w" : "",
                    bit_test(i, exceptfds) ? "x" : "");
        }
    }
    poll_destroy(poll);
    if (err < 0)
        return err;

    if (readfds_addr && user_write(readfds_addr, readfds, fdset_size))
        return _EFAULT;
    if (writefds_addr && user_write(writefds_addr, writefds, fdset_size))
        return _EFAULT;
    if (exceptfds_addr && user_write(exceptfds_addr, exceptfds, fdset_size))
        return _EFAULT;
    return err;
}

dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr) {
    struct timespec timeout_ts = {};
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        struct timeval_ timeout_timeval;
        if (user_get(timeout_addr, timeout_timeval))
            return _EFAULT;
        timeout_ts = convert_timeval(timeout_timeval);
        timeout_ts_addr = &timeout_ts;
    }
    // XXX we do not implement the Linux behavior of writing the timeout with remaining time here.
    return select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_addr, "select");
}

struct poll_context {
    struct pollfd_ *polls;
    struct fd **files;
    int nfds;
};
#define POLL_ALWAYS_LISTENING (POLL_ERR|POLL_HUP|POLL_NVAL)
static int poll_event_callback(void *context, int types, union poll_fd_info info) {
    struct poll_context *c = context;
    struct pollfd_ *polls = c->polls;
    int nfds = c->nfds;
    int res = 0;
    for (int i = 0; i < nfds; i++) {
        if (c->files[i] == info.ptr) {
            polls[i].revents = types & (polls[i].events | POLL_ALWAYS_LISTENING);
            res = 1;
        }
    }
    return res;
}
dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout) {
    STRACE("poll(0x%x, %d, %d)", fds, nfds, timeout);
    struct pollfd_ polls[nfds];
    if (fds != 0 || nfds != 0)
        if (user_read(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    struct poll *poll = poll_create();
    if (IS_ERR(poll))
        return PTR_ERR(poll);

    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].events);
    STRACE("...\n");

    struct fd *files[nfds];
    for (unsigned i = 0; i < nfds; i++) {
        files[i] = f_get(polls[i].fd);
        if (files[i] != NULL)
            // FIXME it might have been closed by now by another thread
            fd_retain(files[i]);
        // clear revents, which is reused to mark whether a pollfd has been added or not
        polls[i].revents = 0;
    }

    // convert polls array into poll_add_fd calls
    // FIXME this is quadratic
    for (unsigned i = 0; i < nfds; i++) {
        if (polls[i].fd < 0 || polls[i].revents)
            continue;

        // if the same fd is listed more than once, merge the events bits together
        int events = polls[i].events;
        polls[i].revents = 1;
        if (files[i] == NULL)
            continue;
        for (unsigned j = 0; j < nfds; j++) {
            if (polls[j].revents)
                continue;
            if (files[i] == files[j]) {
                events |= polls[j].events;
                polls[j].revents = 1;
            }
        }

        poll_add_fd(poll, files[i], events | POLL_ALWAYS_LISTENING, (union poll_fd_info) (void *) files[i]);
    }

    for (unsigned i = 0; i < nfds; i++) {
        polls[i].revents = 0;
        if (polls[i].fd >= 0 && files[i] == NULL)
            polls[i].revents = POLL_NVAL;
    }
    struct poll_context context = {polls, files, nfds};
    struct timespec timeout_ts;
    if (timeout >= 0) {
        timeout_ts.tv_sec = timeout / 1000;
        timeout_ts.tv_nsec = (timeout % 1000) * 1000000;
    }
    int res = poll_wait(poll, poll_event_callback, &context, timeout < 0 ? NULL : &timeout_ts);
    poll_destroy(poll);
    for (unsigned i = 0; i < nfds; i++) {
        if (files[i] != NULL)
            fd_close(files[i]);
    }
    STRACE("%d end poll", current->pid);
    for (unsigned i = 0; i < nfds; i++)
        STRACE(" {%d, %#x}", polls[i].fd, polls[i].revents);

    if (res < 0)
        return res;
    if (fds != 0 || nfds != 0)
        if (user_write(fds, polls, sizeof(struct pollfd_) * nfds))
            return _EFAULT;
    return res;
}

dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr) {
    struct timespec_ timeout_timespec;
    struct timespec timeout_ts;
    struct timespec *timeout_ts_addr = NULL;
    if (timeout_addr != 0) {
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout_ts = convert_timespec(timeout_timespec);
        timeout_ts_addr = &timeout_ts;
    }
    // a system call can only take 6 parameters, so the last two need to be passed as a pointer to a struct
    struct {
        addr_t mask_addr;
        dword_t mask_size;
    } sigmask;
    if (user_get(sigmask_addr, sigmask))
        return _EFAULT;
    sigset_t_ mask;

    if (sigmask.mask_addr != 0) {
        if (sigmask.mask_size != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask.mask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }
    return select_common(nfds, readfds_addr, writefds_addr, exceptfds_addr, timeout_ts_addr, "pselect");
}

dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize) {
    int timeout = -1;
    if (timeout_addr != 0) {
        struct timespec_ timeout_timespec;
        if (user_get(timeout_addr, timeout_timespec))
            return _EFAULT;
        timeout = timeout_timespec.sec * 1000 + timeout_timespec.nsec / 1000000;
    }

    sigset_t_ mask;
    if (sigmask_addr != 0) {
        if (sigsetsize != sizeof(sigset_t_))
            return _EINVAL;
        if (user_get(sigmask_addr, mask))
            return _EFAULT;
        sigmask_set_temp(mask);
    }

    return sys_poll(fds, nfds, timeout);
}
