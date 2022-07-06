#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <sstream>
#include <unordered_map>
#include <fstream>

#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#include "fd_passing.h"

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
    while (tail != msg_end && *tail != ' ' && *tail != '?') ++tail;
    http_request["Path"] = std::string (head, tail);
}

void
send_http_responce_200 (int fd, const char *data, int sz)
{
    std::stringstream ss;

    // Create a result with "HTTP/1.0 200 OK"
    ss << "HTTP/1.0 200 OK";
    ss << "\r\n";
    ss << "Content-length: ";
    ss << sz;
    ss << "\r\n";
    ss << "Connection: close";
    ss << "\r\n";
    ss << "Content-Type: text/html";
    ss << "\r\n\r\n";
    ss << data;

    if (send (fd, ss.str ().c_str (), ss.str ().size (), 0) == -1) {
        syslog (LOG_ERR, "[send_http_responce_200] could not send responce \n%s\n%s",
                data, strerror(errno));
    }
    else {
        syslog (LOG_DEBUG, "[send_http_responce_200] responce sended \n%s", data);
    }
}

void
send_http_responce_404 (int fd)
{
    std::stringstream ss;

    // Create a result with "HTTP/1.0 404 NOT FOUND"
    ss << "HTTP/1.0 404 NOT FOUND";
    ss << "\r\n";
    ss << "Content-length: ";
    ss << 0;
    ss << "\r\n";
    ss << "Connection: close";
    ss << "Content-Type: text/html";
    ss << "\r\n\r\n";

    send (fd, ss.str ().c_str (), ss.str ().size (), 0);

    if (send (fd, ss.str ().c_str (), ss.str ().size (), 0) == -1) {
        syslog (LOG_ERR, "[send_http_responce_404] could not send responce %s", strerror(errno));
    }
    else {
        syslog (LOG_DEBUG, "[send_http_responce_404] responce sended");
    }
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
        std::cerr << "[main] incorrect ip address" << std::endl;
        return EXIT_FAILURE;
    }
    if (!p) {
        std::cerr << "[main] incorrect port" << std::endl;
        return EXIT_FAILURE;
    }
    uint16_t port = atoi (p);
    if (port == 0) {
        std::cerr << "[main] incorrect port" << std::endl;
        return EXIT_FAILURE;
    }
    if (!d) {
        std::cerr << "[main] incorrect directory" << std::endl;
        return EXIT_FAILURE;
    }

    struct sigaction act;
    sigemptyset (&(act.sa_mask));
    act.sa_sigaction = sig_handler;
    act.sa_flags = SA_SIGINFO;
    if (sigaction (SIGINT, &act, NULL) == -1) {
        std::cerr << "[main] error while sigaction (SIGINT): " << strerror (errno) << std::endl;
        return EXIT_FAILURE;
    }
    if (sigaction (SIGHUP, &act, NULL) == -1) {
        std::cerr << "[main] error while sigaction (SIGHUP): " << strerror (errno) << std::endl;
        return EXIT_FAILURE;
    }
    if (sigaction (SIGCHLD, &act, NULL) == -1) {
        std::cerr << "[main] error while sigaction (SIGCHLD): " << strerror (errno) << std::endl;
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
        syslog (LOG_INFO, "[sig_handler] no action for SIGHUP while");
    } else if (signum == SIGINT && signum == SIGQUIT) {
        syslog (LOG_INFO, "[sig_handler] by-by baby");
        closelog ();
        exit (EXIT_SUCCESS);
    } else if (SIGCHLD) {
        syslog (LOG_INFO, "[sig_handler] receive SIGCHLD signal");
        get_workers_status ();
    } else {
        syslog (LOG_INFO, "[sig_handler] no action for signal %d while", signum);
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
    else {
        //-- parent
        close (fd_ipc[1]);
    }

    return fd_ipc[0];
}

void
worker_processing (int fd_pair, char *dir)
{
    set_nonblock (fd_pair);

    int fd_epoll = epoll_create1 (0);
    if (fd_epoll < 0) {
        syslog (LOG_CRIT, "[worker] error while epoll_create1 %s", strerror (errno));
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
                epoll_delete_event (fd_epoll, events[i].data.fd);
                shutdown (events[i].data.fd, SHUT_RDWR);
                close (events[i].data.fd);

                syslog (LOG_ERR, "[worker] error EPOLLERR or EPOLLHUP");
            }
            //-- receive new fd from master
            else if (events[i].data.fd == fd_pair)
            {
                int fd_client {-1};
                char buff[1];
                memset (buff, '\0', 1);

                sock_fd_read (events[i].data.fd, (void*)buff, 1, &fd_client);

                syslog (LOG_DEBUG, "[worker] receive new client fd %d", fd_client);
                epoll_add_event (fd_epoll, fd_client);
            }
            //-- reveive data from somebody else
            else
            {
                static char request [2048];
                memset (request, '\0', 2048);

                if (recv (events[i].data.fd, request, 2048, 0) == 0)
                {
                    //shutdown (events[i].data.fd, SHUT_RDWR);

                    epoll_delete_event (fd_epoll, events[i].data.fd);
                    if (shutdown (events[i].data.fd, SHUT_RDWR) == -1) {
                        syslog (LOG_ERR, "[worker] could not shutdown the socket: %s", strerror (errno));
                    }
                    if (close (events[i].data.fd) == -1) {
                        syslog (LOG_ERR, "[worker] could not close the socket: %s", strerror (errno));
                    }
                }
                else
                {
                    syslog (LOG_DEBUG, "[worker] receive data from client '%s'", request);

                    //---- parse HTTP

                    std::unordered_map<std::string, std::string> http_request;
                    parse_header (&request[0], &request[strlen(request)], http_request);

                    syslog (LOG_DEBUG, "[worker] path is '%s'", http_request["Path"].c_str ());

                    //-- пустой путь
                    if (http_request["Path"] == "/")
                    {
                        send_http_responce_404 (events[i].data.fd);
                    }
                    else
                    {
                        //---- read file

                        std::string fpath = std::string (dir) + http_request["Path"];
                        std::ifstream file (fpath);
                        if (!file)
                        {
                            send_http_responce_404 (events[i].data.fd);
                        }
                        else
                        {
                            file.seekg (0, file.end);
                            int lenght = file.tellg ();
                            file.seekg (0, file.beg);

                            char *fbuffer = new char [lenght + 1];
                            memset (fbuffer, '\0', lenght + 1);
                            file.read (fbuffer, lenght);

                            send_http_responce_200 (events[i].data.fd, fbuffer, strlen (fbuffer));
                            delete [] fbuffer;
                        }
                    }

                    epoll_delete_event (fd_epoll, events[i].data.fd);
                    if (close (events[i].data.fd) == -1) {
                        syslog (LOG_ERR, "[worker] could not close the socket: %s", strerror (errno));
                    } else {
                        syslog (LOG_ERR, "[worker] close this fucking socket");
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
        syslog (LOG_CRIT, "[master] error while socket: %s", strerror (errno));
        exit (EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    memset (&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr (ip);
    serv_addr.sin_port = htons (port);

    set_nonblock (fd_server);

    if (bind (fd_server, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1) {
        syslog (LOG_CRIT, "[master] error while bind: %s", strerror (errno));
        exit (EXIT_FAILURE);
    }
    if (listen (fd_server, 8) == -1) {
        syslog (LOG_CRIT, "[master] error while listen: %s", strerror (errno));
        exit (EXIT_FAILURE);
    }

    //-- creat epoll
    int fd_epoll = epoll_create1 (0);
    if (fd_epoll < 0) {
        syslog (LOG_CRIT, "[master] error while epoll_create1: %s", strerror (errno));
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
                epoll_delete_event (fd_epoll, events[i].data.fd);
                shutdown (events[i].data.fd, SHUT_RDWR);
                //close (events[i].data.fd);

                syslog (LOG_ERR, "[master] error EPOLLERR or EPOLLHUP");
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

                        syslog (LOG_INFO, "[master] send fd (%d) to the worker %d",
                                fd_client, next_worker);

                        close (fd_client);
                    } else {
                        syslog (LOG_INFO, "[master] error while 'accept': %s",
                                strerror (errno));
                    }
                }
            }
        }
    }
}

void
set_nonblock (int fd) {
    if (fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        syslog (LOG_CRIT, "[set_nonblock] error while set_nonblock: %s", strerror (errno));
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
            syslog (LOG_CRIT, "[get_workers_status] error while 'waitpid': %s", strerror (errno));
            return;
        }

        if (WIFEXITED (status)) {
            syslog (LOG_INFO, "[get_workers_status] child exited with status of %d", WEXITSTATUS(status));
        } else {
            syslog (LOG_DEBUG, "[get_workers_status] child did not exit successfully");
        }
        return;
    }
}
