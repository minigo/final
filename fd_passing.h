#ifndef FD_PASSING_H
#define FD_PASSING_H

#include <sys/types.h>

//-- https://keithp.com/blogs/fd-passing/

ssize_t
sock_fd_write (int sock, void *buf, ssize_t buflen, int fd);
ssize_t
sock_fd_read (int sock, void *buf, ssize_t bufsize, int *fd);

#endif //FD_PASSING_H
