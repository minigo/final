#include "fd_passing.h"

#include <sys/socket.h>
#include <syslog.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

ssize_t
sock_fd_read (int sock, void *buf, ssize_t bufsize, int *fd)
{
    ssize_t size;

    if (fd) {
        struct msghdr msg;
        struct iovec iov;
        union {
            struct cmsghdr cmsghdr;
            char control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = nullptr;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        size = recvmsg (sock, &msg, 0);
        if (size < 0) {
            syslog (LOG_ERR, "error while 'recvmsg': %s", strerror (errno));
            return -1;
        }

        cmsg = CMSG_FIRSTHDR (&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN (sizeof (int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                syslog (LOG_ERR, "invalid cmsg_level %d", cmsg->cmsg_level);
                return -1;
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                syslog (LOG_ERR, "invalid cmsg_type %d\n", cmsg->cmsg_type);
                return -1;
            }

            *fd = *((int *) CMSG_DATA(cmsg));
        } else
            *fd = -1;
    } else {
        size = read (sock, buf, bufsize);
        if (size < 0) {
            syslog (LOG_ERR, "error while 'read': %s", strerror (errno));
            return -1;
        }
    }

    return size;
}

ssize_t
sock_fd_write (int sock, void *buf, ssize_t buflen, int fd)
{
    ssize_t size;
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr cmsghdr;
        char control[CMSG_SPACE(sizeof (int))];
    } cmsgu;
    struct cmsghdr *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd != -1) {
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);

        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_len = CMSG_LEN(sizeof (int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;

        *((int *) CMSG_DATA(cmsg)) = fd;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }

    size = sendmsg (sock, &msg, 0);

    if (size < 0) {
        syslog (LOG_ERR, "error while 'sendmsg': %s", strerror (errno));
        return -1;
    }

    return size;
}
