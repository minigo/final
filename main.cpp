#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <sstream>
#include <unordered_map>

#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

static const int max_events {32};

void
set_nonblock (int fd);
void
worker_processing (int fd, char *dir);
void
master_processing (pid_t *children, int num, const char *ip, uint16_t port, const char *dir);
void
epoll_add_event (int fd_epoll, int fd);
void
epoll_delete_event (int fd_epoll, int fd);

void
daemonize ();
pid_t
workerize (char *dir);
ssize_t
sock_fd_write (int sock, void *buf, ssize_t buflen, int fd);
ssize_t
sock_fd_read (int sock, void *buf, ssize_t bufsize, int *fd);

void
parse_http_request (char *request, char *path)
{
    const char *start_of_path = strchr (request, ' ') + 1;
    const char *end_of_path = strchr (start_of_path, ' ');

    strncpy (path, start_of_path,  end_of_path - start_of_path);
}

void parse_header (const char *msg, const char *msg_end,
                   std::unordered_map<std::string, std::string> &http_request)
{
    const char *head = msg;
    const char *tail = msg;

    // Find request type
    while (tail != msg_end && *tail != ' ') ++tail;
    http_request["Type"] = std::string(head, tail);

    // Find path
    while (tail != msg_end && *tail == ' ') ++tail;
    head = tail;
    while (tail != msg_end && *tail != ' ' && *tail != ':') ++tail;
    http_request["Path"] = std::string (head, tail);

    // Find HTTP version
    while (tail != msg_end && *tail == ' ') ++tail;
    head = tail;
    while (tail != msg_end && *tail != '\r') ++tail;
    http_request["Version"] = std::string(head, tail);
    if (tail != msg_end) ++tail;  // skip '\r'
    // TODO: what about the trailing '\n'?

    // Map all headers from a key to a value
    //    head = tail;
    //    while (head != msg_end && *head != '\r') {
    //        while (tail != msg_end && *tail != '\r') ++tail;
    //        const char *colon = memchr (head, tail, ':');
    //        if (colon == NULL) {
    //            // TODO: malformed headers, what should happen?
    //            break;
    //        }
    //        const char *value = colon+1;
    //        while (value != tail && *value == ' ') ++value;
    //        http_request[ std::string(head, colon) ] = std::string(value, tail);
    //        head = tail+1;
    //        // TODO: what about the trailing '\n'?
    //    }
    //return http_request;
}

//! \brief Обработчик сигналов.
//!TODO: Почему мало кто применяет signalfd ?
void
sig_handler (int signum, siginfo_t * siginfo, void *code);

static void
get_workers_status ();

int
main (int argc, char *argv[])
{
    char *h {nullptr};
    char *p {nullptr};
    char *d {nullptr};

    int res {-1};
    while ((res = getopt (argc, argv, "h:p:d:")) != -1) {
        switch (res) {
        case 'h':
            h = optarg;
            break;
        case 'p':
            p = optarg;
            break;
        case 'd':
            d = optarg;
            break;
        default:
            break;
        }
    }

    if (!h) {
        std::cerr << "incorrect ip address" << std::endl;
        return EXIT_FAILURE;
    }
    if (!p) {
        std::cerr << "incorrect port" << std::endl;
        return EXIT_FAILURE;
    }
    uint16_t port = atoi (p);
    if (port == 0) {
        std::cerr << "incorrect port" << std::endl;
        return EXIT_FAILURE;
    }
    if (!d) {
        std::cerr << "incorrect directory" << std::endl;
        return EXIT_FAILURE;
    }

    struct sigaction act;
    sigemptyset (&(act.sa_mask));
    act.sa_sigaction = sig_handler;
    act.sa_flags = SA_SIGINFO;
    if (sigaction (SIGINT, &act, NULL) == -1) {
        std::cerr << "error while sigaction (SIGINT): " << strerror (errno) << std::endl;
        return EXIT_FAILURE;
    }
    if (sigaction (SIGHUP, &act, NULL) == -1) {
        std::cerr << "error while sigaction (SIGHUP): " << strerror (errno) << std::endl;
        return EXIT_FAILURE;
    }
    if (sigaction (SIGCHLD, &act, NULL) == -1) {
        std::cerr << "error while sigaction (SIGCHLD): " << strerror (errno) << std::endl;
        return EXIT_FAILURE;
    }

    daemonize ();

    //-- количество worker равно количеству ядер
    int num_cpu = sysconf (_SC_NPROCESSORS_ONLN);

    pid_t children[num_cpu];
    for (int i = 0; i < num_cpu; ++i)
        children[i] = workerize (d);
    for (int i= 0; i < num_cpu; ++i)
        set_nonblock (children[i]);

    master_processing (children, num_cpu, h, port, d);
    return EXIT_SUCCESS;
}

void
sig_handler (int signum, siginfo_t * siginfo, void *code)
{
    if (signum == SIGHUP) {
        syslog (LOG_INFO, "no action for SIGHUP while");
    } else if (signum == SIGINT && signum == SIGQUIT) {
        syslog (LOG_INFO, "by-by baby");
        closelog ();
        exit (EXIT_SUCCESS);
    } else if (SIGCHLD) {
        syslog (LOG_INFO, "receive SIGCHLD signal");
        get_workers_status ();
    } else {
        syslog (LOG_INFO, "no action for signal %d while", signum);
    }
}

void
daemonize ()
{
    auto pid = fork ();

    if (pid == -1) {
        std::cerr << "error while fork: " << strerror (errno) << std::endl;
        exit (EXIT_FAILURE);
    }
    if (pid > 0)
        exit (EXIT_SUCCESS);

    //-- Set new file permissions
    umask (0);

    if (setsid () < 0) {
        std::cerr << "error while setsid: " << strerror (errno) << std::endl;
        exit (EXIT_FAILURE);
    }

    //-- Change the working directory to the root directory
    //-- or another appropriated directory
    if (chdir ("/") < 0) {
        std::cerr << "error while chdir: " << strerror (errno) << std::endl;
        exit (EXIT_FAILURE);
    }

    close (STDIN_FILENO);
    close (STDOUT_FILENO);
    close (STDERR_FILENO);

    //-- Close all open file descriptors
    //    int x {-1};
    //    for (x = sysconf (_SC_OPEN_MAX); x >= 0; x--)
    //        close (x);

    //-- Open the log file
    openlog ("FINAL", LOG_PID, LOG_DAEMON);
}

//! \brief Форкаемся для создания worker
//! \return pid от созданного socketpair, через который доступен worker
pid_t
workerize (char *dir)
{
    pid_t fd_ipc[2];

    //-- create socket pair
    if (socketpair (AF_UNIX, SOCK_STREAM, 0, fd_ipc) < 0) {
        syslog (LOG_CRIT, "[workerize] error while socketpair %s", strerror (errno));
        return EXIT_FAILURE;
    }

    //-- fork
    int fpid = fork ();
    if (fpid == -1) {
        syslog (LOG_CRIT, "[workerize] error while fork %s", strerror (errno));
        return EXIT_FAILURE;
    }

    //-- child
    if (fpid == 0) {
        close (fd_ipc[0]);
        worker_processing (fd_ipc[1], dir);
    }
    //-- parent
    return fd_ipc[0];
}

void
worker_processing (int fd_pair, char *dir)
{
    set_nonblock (fd_pair);

    int fd_epoll = epoll_create1 (0);
    if (fd_epoll < 0) {
        syslog (LOG_CRIT, "[worker %d] error while epoll_create1 %s", getpid (), strerror (errno));
        exit (EXIT_FAILURE);
    }

    epoll_add_event (fd_epoll, fd_pair);
    struct epoll_event events[max_events];

    while (true)
    {
        int n = epoll_wait (fd_epoll, events, max_events, -1);
        for (int i = 0; i < n; ++i)
        {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP))
            {
                shutdown (events[i].data.fd, SHUT_RDWR);
                epoll_delete_event (fd_epoll, events[i].data.fd);
                syslog (LOG_ERR, "[worker %d] error EPOLLERR or EPOLLHUP", getpid ());
            }
            //-- receive new fd from master
            else if (events[i].data.fd == fd_pair)
            {
                int fd_client {-1};
                char buff[1];
                memset (buff, '\0', 1);

                sock_fd_read (events[i].data.fd, (void*)buff, 1, &fd_client);

                syslog (LOG_INFO, "[worker %d] receive new client fd %d", getpid (), fd_client);
                epoll_add_event (fd_epoll, fd_client);
            }
            //-- reveive data from somebody else
            else
            {
                static char request [2048];
                memset (request, '\0', 2048);

                if (recv (events[i].data.fd, request, 2048, 0) == 0) {
                    shutdown (events[i].data.fd, SHUT_RDWR);
                } else {
                    syslog (LOG_INFO, "[worker %d] receive data from client '%s'", getpid (), request);

                    //---- parse HTTP

                    std::unordered_map<std::string, std::string> http_request;
                    parse_header (&request[0], &request[strlen(request)], http_request);

                    //syslog (LOG_INFO, "[worker %d] path is '%s'", getpid (), http_request["Path"].c_str ());

                    //---- read file

                    std::string fpath = std::string (dir) + http_request["Path"];

                    auto file = fopen (fpath.c_str (), "r");
                    if (file)
                    {
                        fseek (file, 0, SEEK_END);
                        long fsize = ftell (file);
                        fseek (file, 0, SEEK_SET);  /* same as rewind(f); */

                        char *string = (char*)malloc (fsize + 1);
                        fread (string, fsize, 1, file);
                        fclose (file);

                        string[fsize] = 0;

                        std::stringstream ss;

                        // Create a result with "HTTP/1.0 200 OK"
                        ss << "HTTP/1.0 200 OK";
                        ss << "\r\n";
                        ss << "Content-length: ";
                        ss << (fsize + 1);
                        ss << "\r\n";
                        ss << "Content-Type: text/html";
                        ss << "\r\n\r\n";
                        ss << string;

                        send (events[i].data.fd, ss.str ().c_str (), ss.str ().size (), 0);
                    }
                    else
                    {
                        std::stringstream ss;

                        // Create a result with "HTTP/1.0 404 NOT FOUND"
                        ss << "HTTP/1.0 404 NOT FOUND";
                        ss << "\r\n";
                        ss << "Content-length: ";
                        ss << 0;
                        ss << "\r\n";
                        ss << "Content-Type: text/html";
                        ss << "\r\n\r\n";

                        send (events[i].data.fd, ss.str ().c_str (), ss.str ().size (), 0);
                    }
                }
            }
        }
    }
}

void
master_processing (pid_t *children, int num, const char *ip, uint16_t port, const char */*dir*/)
{
    //-- create server fd
    int fd_server = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_server == -1) {
        syslog (LOG_CRIT, "[master %d] error while socket: %s", getpid (), strerror (errno));
        exit (EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    memset (&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr (ip);
    serv_addr.sin_port = htons (port);

    set_nonblock (fd_server);

    if (bind (fd_server, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        syslog (LOG_CRIT, "[master %d] error while bind: %s", getpid (), strerror (errno));
        exit (EXIT_FAILURE);
    }
    if (listen (fd_server, 8) == -1) {
        syslog (LOG_CRIT, "[master %d] error while listen: %s", getpid (), strerror (errno));
        exit (EXIT_FAILURE);
    }

    //-- creat epoll
    int fd_epoll = epoll_create1 (0);
    if (fd_epoll < 0) {
        syslog (LOG_CRIT, "[master %d] error while epoll_create1: %s", getpid (), strerror (errno));
        exit (EXIT_FAILURE);
    }

    for (int i = 0; i < num; ++i)
        epoll_add_event (fd_epoll, children[i]);
    epoll_add_event (fd_epoll, fd_server);

    struct epoll_event events[max_events];

    int queue_child {0};

    while (true)
    {
        int n = epoll_wait (fd_epoll, events, max_events, -1);
        for (int i = 0; i < n; ++i)
        {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP))
            {
                shutdown (events[i].data.fd, SHUT_RDWR);
                epoll_delete_event (fd_epoll, events[i].data.fd);
                syslog (LOG_ERR, "[master %d] error EPOLLERR or EPOLLHUP", getpid ());
            }
            else
            {
                //-- по сути проверка сообщения от worker не нужна, от них мы ничего не ждём
                //-- поэтому сразу проверяем сокет, на котором висит сервет
                if (events[i].data.fd == fd_server)
                {
                    //-- Получаем сокет для общения с клиентом и высылаем этот сокет worker'у.
                    //-- В логах номера отправленного и полученного сокета будут отличаться (не заьываем про это).
                    //-- next_worker служит нам для определения worker'а, которому высылаем дескриптор
                    int fd_client = accept (fd_server, 0, 0);
                    if (fd_client > 0)
                    {
                        short next_worker = queue_child++ % num;
                        sock_fd_write (children[next_worker], (void*)"0", 1, fd_client);

                        syslog (LOG_INFO, "[master %d] send fd (%d) to the worker %d",
                                getpid (), fd_client, next_worker);
                    } else {
                        syslog (LOG_INFO, "[master %d] error while 'accept': %s",
                                getpid (), strerror (errno));
                    }
                }
            }
        }
    }
}

void
set_nonblock (int fd) {
    if (fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        syslog (LOG_CRIT, "error while set_nonblock: %s", strerror (errno));
        exit (EXIT_FAILURE);
    }
}

void
epoll_add_event (int fd_epoll, int fd) {
    struct epoll_event eventd;
    eventd.data.fd = fd;
    eventd.events = EPOLLIN;
    epoll_ctl (fd_epoll, EPOLL_CTL_ADD, fd, &eventd);
}

void
epoll_delete_event (int fd_epoll, int fd) {
    struct epoll_event eventd;
    eventd.data.fd = fd;
    eventd.events = EPOLLIN;
    epoll_ctl (fd_epoll, EPOLL_CTL_DEL, fd, &eventd);
}

ssize_t
sock_fd_write (int sock, void *buf, ssize_t buflen, int fd)
{
    ssize_t     size;
    struct msghdr   msg;
    struct iovec    iov;
    union {
        struct cmsghdr  cmsghdr;
        char        control[CMSG_SPACE(sizeof (int))];
    } cmsgu;
    struct cmsghdr  *cmsg;

    iov.iov_base = buf;
    iov.iov_len = buflen;

    msg.msg_name = NULL;
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

        printf ("passing fd %d\n", fd);
        *((int *) CMSG_DATA(cmsg)) = fd;
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
        printf ("not passing fd\n");
    }

    size = sendmsg (sock, &msg, 0);

    if (size < 0)
        perror ("sendmsg");
    return size;
}

ssize_t
sock_fd_read (int sock, void *buf, ssize_t bufsize, int *fd)
{
    ssize_t     size;

    if (fd) {
        struct msghdr   msg;
        struct iovec    iov;
        union {
            struct cmsghdr  cmsghdr;
            char        control[CMSG_SPACE(sizeof (int))];
        } cmsgu;
        struct cmsghdr  *cmsg;

        iov.iov_base = buf;
        iov.iov_len = bufsize;

        msg.msg_name = NULL;
        msg.msg_namelen = 0;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgu.control;
        msg.msg_controllen = sizeof(cmsgu.control);
        size = recvmsg (sock, &msg, 0);
        if (size < 0) {
            perror ("recvmsg");
            exit(1);
        }
        cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            if (cmsg->cmsg_level != SOL_SOCKET) {
                fprintf (stderr, "invalid cmsg_level %d\n",
                         cmsg->cmsg_level);
                exit(1);
            }
            if (cmsg->cmsg_type != SCM_RIGHTS) {
                fprintf (stderr, "invalid cmsg_type %d\n",
                         cmsg->cmsg_type);
                exit(1);
            }

            *fd = *((int *) CMSG_DATA(cmsg));
            printf ("received fd %d\n", *fd);
        } else
            *fd = -1;
    } else {
        size = read (sock, buf, bufsize);
        if (size < 0) {
            perror ("read");
            exit (1);
        }
    }
    return size;
}

static void
get_workers_status ()
{
    int status;
    pid_t pid;

    for (;;)
    {
        pid = waitpid (-1, &status, WNOHANG);

        if (pid == 0)
            return;

        if (pid == -1) {
            syslog (LOG_CRIT, "error while waitpid %s", strerror (errno));
            return;
        }

        if (WIFEXITED (status))
            syslog (LOG_INFO, "child exited with status of %d", WEXITSTATUS(status));
        else
            syslog (LOG_CRIT, "child did not exit successfully");
        return;
    }
}
