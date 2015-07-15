#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ev.h>

#include "SkipDB.h"

#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (const typeof( ((type *)0)->member )*)(ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

#define PATH_MAX 128
#define COMMAND_LEN 32
#define KEY_LEN 128
#define BUF_MAX 2048

typedef struct _skipd_server {
    ev_io io;
    int fd;
    struct sockaddr_un socket;
    int socket_len;

    SkipDB *db;

    int daemon;
    char db_path[PATH_MAX];
    char sock_path[PATH_MAX];
    char pid_path[PATH_MAX];
} skipd_server;

typedef struct _skipd_client {
    ev_io io_read;
    ev_io io_write;
    int fd;

    char command[COMMAND_LEN+1];
    char key[KEY_LEN+1];
    int data_len;

    int pos;
    int len;
    int max;
    skipd_server* server;
} skipd_client;

int setnonblock(int fd);
static void not_blocked(EV_P_ ev_periodic *w, int revents);

static void client_release(EV_P_ skipd_client* client) {
    ev_io_stop(EV_A_ &client->io_read);
    ev_io_stop(EV_A_ &client->io_write);
    close(client->fd);

    if(NULL != client->buf) {
        free(client->buf);
    }

    free(client);
}

// This callback is called when client data is available
static void client_read(EV_P_ ev_io *w, int revents) {
    skipd_client* client = container_of(w, skipd_client, io_read);
    int n;
    if(NULL == client->buf) {
        client->buf = (char*)malloc(BUF_MAX);
        client->len = 0;
        client->pos = 0;
        client->max = BUF_MAX;
    }

    assert(0 == client->pos);
    n = recv(client->fd, client->buf, client->max, 0);
    if (0 == n) {
        // an orderly disconnect
        puts("orderly disconnect");
        client_release(client);
        return;
    } else if(n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return
        } else {
            puts("recv error");
            client_release(client);
            return;
        }
    }
    printf("socket recv len=%d\n", n);
    client->len = n;

    //process command
    ;
}

static void client_write(EV_P_ ev_io *w, int revents) {
    skipd_client* client = container_of(w, skipd_client, io_read);
    int n;
}

static struct skipd_client* client_new(int fd) {
    int opt = 1;
    struct sock_ev_client* client = realloc(NULL, sizeof(skipd_client));
    client->fd = fd;
    client->buf = NULL;

    setsockopt(remotefd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
    setnonblock(client->fd);
    ev_io_init(&client->io_read, client_read, client->fd, EV_READ);
    ev_io_init(&client->io_write, client_write, client->fd, EV_WRITE);

    return client;
}

// This callback is called when data is readable on the unix socket.
static void server_cb(EV_P_ ev_io *w, int revents) {
    puts("unix stream socket has become readable");

    int client_fd;
    struct skipd_client* client;

    skipd_server* server = container_of(w, skipd_server, io);

    while (1) {
        client_fd = accept(server->fd, NULL, NULL);
        if( client_fd == -1 ) {
            if( errno != EAGAIN && errno != EWOULDBLOCK ) {
                fprintf(stderr, "accept() failed errno=%i (%s)",  errno, strerror(errno));
                exit(EXIT_FAILURE);
            }
            break;
        }

        puts("accepted a client");
        client = client_new(client_fd);
        client->server = server;
        client->index = array_push(&server->clients, client);
        ev_io_start(EV_A_ &client->io_read);
    }
}

// Simply adds O_NONBLOCK to the file descriptor of choice
int setnonblock(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

int unix_socket_init(struct sockaddr_un* socket_un, char* sock_path, int max_queue) {
    int fd;
    int opt = 1;

    unlink(sock_path);

    // Setup a unix socket listener.
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (-1 == fd) {
        perror("echo server socket");
        exit(EXIT_FAILURE);
    }

    setsockopt(fd, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Set it non-blocking
    if (-1 == setnonblock(fd)) {
        perror("echo server socket nonblock");
        exit(EXIT_FAILURE);
    }

    // Set it as unix socket
    socket_un->sun_family = AF_UNIX;
    strcpy(socket_un->sun_path, sock_path);

    return fd;
}

int server_init(skipd_server* server, int max_queue) {
    server->db = SkipDB_new();
    SkipDB_setPath_(server->db, server->db_path);
    SkipDB_open(server->db);

    server->fd = unix_socket_init(&server->socket, server->sock_path, max_queue);
    server->socket_len = sizeof(server->socket.sun_family) + strlen(server->socket.sun_path);

    if (-1 == bind(server->fd, (struct sockaddr*) &server->socket, server->socket_len)) {
        perror("echo server bind");
        exit(EXIT_FAILURE);
    }

    if (-1 == listen(server->fd, max_queue)) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    return 0;
}

static struct option options[] = {
    { "help",	no_argument,		NULL, 'h' },
    { "db_path", required_argument,	NULL, 'd' },
    { "sock_path", required_argument,	NULL, 's' },
    { "daemon", 	required_argument,	NULL, 'D' },
    { NULL, 0, 0, 0 }
};

int skipd_daemonize(const char *lock_path)
{
	pid_t sid, parent;
	int fd;
	char buf[10];
	int n, ret;
	struct sigaction act;

	/* already a daemon */
	if (getppid() == 1)
		return 1;

	fd = open(lock_path, O_RDONLY);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n) {
			n = atoi(buf);
			ret = kill(n, 0);
			if (ret >= 0) {
				fprintf(stderr,
				     "Daemon already running from pid %d\n", n);
				exit(1);
			}
			fprintf(stderr,
			    "Removing stale lock file %s from dead pid %d\n",
								 _lock_path, n);
			unlink(lock_path);
		}
	}

	/* Trap signals that we expect to recieve */
	signal(SIGCHLD, child_handler);	/* died */
	signal(SIGUSR1, child_handler); /* was happy */
	signal(SIGALRM, child_handler); /* timeout daemonizing */

	/* Fork off the parent process */
	pid_daemon = fork();
	if (pid_daemon < 0) {
		fprintf(stderr, "unable to fork daemon, code=%d (%s)",
		    errno, strerror(errno));
		exit(1);
	}

	/* If we got a good PID, then we can exit the parent process. */
	if (pid_daemon > 0) {

		/*
		 * Wait for confirmation signal from the child via
		 * SIGCHILD / USR1, or for two seconds to elapse
		 * (SIGALRM).  pause() should not return.
		 */
		alarm(2);

		pause();
		/* should not be reachable */
		exit(1);
	}

	/* At this point we are executing as the child process */
	parent = getppid();
	pid_daemon = getpid();

	/* Cancel certain signals */
	signal(SIGCHLD, SIG_DFL); /* A child process dies */
	signal(SIGTSTP, SIG_IGN); /* Various TTY signals */
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGHUP, SIG_IGN); /* Ignore hangup signal */

	/* Change the file mode mask */
	umask(0);

	/* Create a new SID for the child process */
	sid = setsid();
	if (sid < 0) {
		fprintf(stderr,
			"unable to create a new session, code %d (%s)",
			errno, strerror(errno));
		exit(1);
	}

	/*
	 * Change the current working directory.  This prevents the current
	 * directory from being locked; hence not being able to remove it.
	 */
	if (chdir("/") < 0) {
		fprintf(stderr,
			"unable to change directory to %s, code %d (%s)",
			"/", errno, strerror(errno));
		exit(1);
	}

	/* Redirect standard files to /dev/null */
	if (!freopen("/dev/null", "r", stdin))
		fprintf(stderr, "unable to freopen() stdin, code %d (%s)",
						       errno, strerror(errno));

	if (!freopen("/dev/null", "w", stdout))
		fprintf(stderr, "unable to freopen() stdout, code %d (%s)",
						       errno, strerror(errno));

	if (!freopen("/dev/null", "w", stderr))
		fprintf(stderr, "unable to freopen() stderr, code %d (%s)",
						       errno, strerror(errno));

	/* Tell the parent process that we are A-okay */
	kill(parent, SIGUSR1);

	act.sa_handler = lws_daemon_closing;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGTERM, &act, NULL);

	/* return to continue what is now "the daemon" */

	return 0;
}

int main(int argc, char **argv)
{
    int n = 0, daemon = 0, max_queue = 128;
    skipd_server server = {0};
    struct ev_periodic every_few_seconds;
    EV_P  = ev_default_loop(0);

    strcpy(server.sock_path, "/tmp/.skipd_server_sock");
    while (n >= 0) {
            n = getopt_long(argc, argv, "hd:D:s:", options, NULL);
            if (n < 0)
                    continue;
            switch (n) {
            case 'D':
                //TODO fix me if optarg is bigger than PATH_MAX
                strcpy(server.pid_path, optarg);
                daemon = 1;
                break;
            case 'd':
                strcpy(server.db_path, optarg);
                break;
            case 's'
                strcpy(server.sock_path, optarg);
                break;
            case 'h':
                fprintf(stderr, "Usage: webshell -c config_path [-d <log bitfield>] [-f <pid file>]\n");
                exit(1);
                break;
            }
    }

    if(daemon && skipd_daemonize(server.pid_path)) {
        fprintf(stderr, "Failed to daemonize\n");
        return 1;
    }

    // Create unix socket in non-blocking fashion
    server_init(&server, max_queue);

    // To be sure that we aren't actually blocking
    ev_periodic_init(&every_few_seconds, not_blocked, 0, 5, 0);
    ev_periodic_start(EV_A_ &every_few_seconds);

    // Get notified whenever the socket is ready to read
    ev_io_init(&server.io, server_cb, server.fd, EV_READ);
    ev_io_start(EV_A_ &server.io);

    // Run our loop, ostensibly forever
    puts("unix-socket-echo starting...\n");
    ev_loop(EV_A_ 0);

    // This point is only ever reached if the loop is manually exited
    close(server.fd);
    return EXIT_SUCCESS;
}

static void not_blocked(EV_P_ ev_periodic *w, int revents) {
  puts("I'm not blocked");
}