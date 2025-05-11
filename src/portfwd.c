/*
 * Single-threaded TCP port forwarder
 * (c) Emil Mikulic, 2000-2003.
 *
 * Everything here is covered by the GNU GPL.
 *
 *   Linux: gcc -o portfwd portfwd.c
 * FreeBSD: gcc -o portfwd portfwd.c
 * Solaris: gcc -o portfwd portfwd.c -lxnet
 *   Win32: cl portfwd.c wsock32.lib (assumes MSVC)
 *
 * $Id: portfwd.c,v 1.6 2004/10/09 09:08:18 emikulic Exp $
 *
 * ---------- >
 * 2000-11-30 - first try
 * 2001-02-13 - fork() to handle multiple connections
 * 2001-08-24 - multiple connections in a single thread,
 *              REUSEADDR, backlogging (try streaming something),
 *              runs under Win32
 * 2001-09-15 - correct behaviour when connection limit is hit
 *              (ignore sockin instead of open-closing it)
 * 2001-12-12 - got rid of constant alloc/free for backlog
 * 2002-07-22 - renamed poll() so it compiles under Solaris
 * 2002-12-15 - rewrote poll loop to fix 100% CPU problem
 *              (thanks to sCurge for pointing it out to me)
 * 2002-12-28 - fixed rare segfault (thanks to PsychoAnt for spotting
 *              it and providing debug help) and improved efficiency
 * 2003-02-19 - code cleanup
 */

#include <sys/types.h>

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# include <winsock2.h>
# define vsnprintf _vsnprintf
# define MSG_DONTWAIT 0
# define socklen_t int
# undef IN
# undef OUT
#else
# include <netinet/in.h>
# include <arpa/inet.h>
# include <sys/time.h>
# include <sys/socket.h>
# include <unistd.h>
# define INVALID_SOCKET -1
# define SOCKET int
# define closesocket close
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKLOG_SIZE 65530

#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

/*
 * IN: connection from client to portfwd
 * OUT: connection from portfwd to server
 */
typedef enum {IN, OUT} direction;

/* Globals */
static SOCKET	*conn_in = NULL,
		*conn_out = NULL,
		 sockin = INVALID_SOCKET;

static int	 max_connections = 10,
		 active_connections = 0,
		*backlog_in_size = NULL,
		*backlog_out_size = NULL,
		*backlog_in_pos = NULL,
		*backlog_out_pos = NULL,
		 verbose = 0,
		 localport,
		 remoteport;

static char	**backlog_in = NULL,
		**backlog_out = NULL,
		 *remotehost;



/* Handling fatal errors */
static int error_line;

#define ERR error_line=__LINE__,error
static void error(const char *format, ...)
{
	#define ERRSIZE 512
	va_list va;
	char buf[ERRSIZE];
	int ret;

	va_start(va, format);
	ret = vsnprintf(buf, ERRSIZE, format, va);
	if (ret == -1 || ret >= ERRSIZE)
		strcpy(buf, "error message too long");
	va_end(va);

	fprintf(stderr, "line %d: %s\n", error_line, buf);
	exit(EXIT_FAILURE);
	#undef ERRSIZE
}



#ifdef _WIN32
static void init_winsock(void)
{
	WORD ver;
	WSADATA wsadata;

	ver = MAKEWORD(2,0);

	if (WSAStartup(ver, &wsadata) != 0)
		ERR("Couldn't initialise WinSock");
}
#endif



/* TODO - make outgoing connection before accepting incoming one */
static void accept_incoming(void)
{
	struct sockaddr_in addrin, addrout;
	socklen_t sin_size;
	int i, curr=-1;
	SOCKET incoming, outgoing;

	sin_size = (socklen_t)sizeof(struct sockaddr);
	incoming = accept(sockin, (struct sockaddr *)&addrin,
			&sin_size);
	if (incoming < 0)
	{
		printf("accept() freaked out.\n");
		return;
	}

	active_connections++;
	if (verbose)
		printf("Got a connection from %s:%u. active=%d\n",
			inet_ntoa(addrin.sin_addr),
			ntohs(addrin.sin_port),
			active_connections);

	if (active_connections > max_connections)
	{
		printf("ERROR: Maximum limit reached."
			"This should not happen!\n");
		closesocket(incoming);
		active_connections--;
		return;
	}

	/* enqueue */
	for (i=0; (i<max_connections) && (curr<0); i++)
		if ((conn_in[i] == INVALID_SOCKET) &&
			(conn_out[i] == INVALID_SOCKET))
				curr = i;

	if (curr == -1) ERR("couldn't enqueue connection");

	/* create the outgoing socket */
	outgoing = socket(AF_INET, SOCK_STREAM, 0);
	if (outgoing < 0)
		ERR("problem creating outgoing socket");

	addrout.sin_family = AF_INET;
	addrout.sin_port = htons(remoteport);
	addrout.sin_addr.s_addr = inet_addr(remotehost);
	memset(&(addrout.sin_zero), 0, 8);

	/* connect to the remote server */
	if (connect(outgoing, (struct sockaddr *)&addrout,
				sizeof(struct sockaddr)) < 0)
		ERR("problem connect()ing outgoing socket");

	conn_in[curr] = incoming;
	conn_out[curr] = outgoing;
}



static void kill_connection(const int n)
{
	closesocket(conn_in[n]);
	closesocket(conn_out[n]);
	backlog_in_size[n] = backlog_out_size[n] =
		backlog_in_pos[n] = backlog_out_pos[n] = 0;

	conn_in[n] = INVALID_SOCKET;
	conn_out[n] = INVALID_SOCKET;

	active_connections--;
	if (verbose)
		printf("Connection %d closed. active=%d\n", n,
			active_connections);
}



static void add_backlog(const int n, const direction dir,
	const char *buf, const int bufsize)
{
	char **backlog    = (dir==IN)?backlog_in     :backlog_out;
	int *backlog_size = (dir==IN)?backlog_in_size:backlog_out_size;
	int *backlog_pos  = (dir==IN)?backlog_in_pos :backlog_out_pos;

	if (backlog_pos[n] + backlog_size[n] + bufsize > BACKLOG_SIZE)
		ERR("Backlog for connection %d exceeded %d bytes.\n",
			n, BACKLOG_SIZE);

	memcpy(backlog[n] + backlog_pos[n] + backlog_size[n],
		buf, (size_t )bufsize);
	backlog_size[n] += bufsize;

	if (verbose)
		printf("Backlogged %d bytes (%d total) for connection %d\n",
			bufsize, backlog_size[n], n);
}



static void flush_backlog(const int n, const direction dir)
{
	SOCKET *conn      = (dir==IN)?conn_in        :conn_out;
	char **backlog    = (dir==IN)?backlog_in     :backlog_out;
	int *backlog_size = (dir==IN)?backlog_in_size:backlog_out_size;
	int *backlog_pos  = (dir==IN)?backlog_in_pos :backlog_out_pos;
	int sent;

	sent = (int)send(conn[n], backlog[n] + backlog_pos[n],
		backlog_size[n], MSG_DONTWAIT);

	if (sent < 1)
	{
		if (verbose)
		{
			printf("send() returned %d. ", sent);
			if (sent == -1) printf("errno=%d ", errno);
			printf("\n");
		}
		kill_connection(n);
		return;
	}

	if (verbose) printf("connection %d: sent %d of %d backlog\n",
		n, sent, backlog_size[n]);

	backlog_size[n] -= sent;

	if (backlog_size[n] == 0)
		backlog_pos[n] = 0;
	else
		backlog_pos[n] += sent;
}



static void bounce(const SOCKET src, const SOCKET dest,
	const int n, const direction dir)
{
	char buf[BACKLOG_SIZE];
	int recvd, sent;

	recvd = (int)recv(src, buf, BACKLOG_SIZE, 0);
	if (recvd < 1)
	{
		if (verbose)
		{
			printf("recv() returned %d. ", recvd);
			if (recvd == -1) printf("errno=%d ", errno);
			printf("\n");
		}
		kill_connection(n);
		return;
	}

	if (verbose)
	{
		printf("connection %d: recvd %d and ", n, recvd);
		fflush(stdout);
	}

	sent = (int)send(dest, buf, recvd, MSG_DONTWAIT);
	if (sent < 1)
	{
		if (verbose)
		{
			printf("send() returned %d. ", sent);
			if (sent == -1) printf("errno=%d ", errno);
			printf("\n");
		}
		kill_connection(n);
		return;
	}
	if (verbose) printf("sent %d\n", sent);
	if (sent < recvd) add_backlog(n, dir, buf+sent, recvd-sent);
}



static void broken_pipe(const int signum)
{
	/* In Linux we can pass MSG_NOSIGNAL to send() and recv()
	 * to suppress a SIGPIPE signal.  Other systems can't, so
	 * we catch it instead.
	 */
}



static void term_signal(const int signum)
{
	int i;

	if (verbose) printf("Caught a SIGTERM.  Shutting down.\n");

	for (i=0; i<max_connections; i++)
	{
		if (conn_in[i] != INVALID_SOCKET)
		{
			shutdown(conn_in[i], 2);
			closesocket(conn_in[i]);
		}

		if (conn_out[i] != INVALID_SOCKET)
		{
			shutdown(conn_out[i], 2);
			closesocket(conn_out[i]);
		}

		free(backlog_in[i]);
		free(backlog_out[i]);
	}

	closesocket(sockin);

	free(conn_in);
	free(conn_out);
	free(backlog_in);
	free(backlog_in_size);
	free(backlog_in_pos);
	free(backlog_out);
	free(backlog_out_size);
	free(backlog_out_pos);

#ifdef _WIN32
	WSACleanup();
#endif

	exit(EXIT_SUCCESS);
}



static int valid_socket(const int i)
{
	if (conn_in[i] != INVALID_SOCKET && conn_out[i] != INVALID_SOCKET)
		return 1;
	else if (
	(conn_in[i] == INVALID_SOCKET && conn_out[i] != INVALID_SOCKET) ||
	(conn_in[i] != INVALID_SOCKET && conn_out[i] == INVALID_SOCKET)
	)
		ERR("internal inconsistency! in[%d]=%d, out[%d]=%d",
			i, conn_in[i], i, conn_out[i]);

	/* else both are INVALID */
	return 0;
}



static void poll_conn(void)
{
	int select_ret, i;
	fd_set r1_fd, w1_fd, r2_fd, w2_fd;
	struct timeval timeout;
	SOCKET max_fd;

	/* stage 1: check for read/write-ability of all fds */
	FD_ZERO(&w1_fd);
	FD_ZERO(&r1_fd);
	max_fd = 0;
	for (i=0; i<max_connections; i++)
	if (valid_socket(i))
	{
		FD_SET(conn_in[i], &w1_fd);
		FD_SET(conn_in[i], &r1_fd);
		FD_SET(conn_out[i], &w1_fd);
		FD_SET(conn_out[i], &r1_fd);

		max_fd = max(max_fd, max(conn_in[i], conn_out[i]));
	}

	if (max_fd)
	{
		/* poll! */
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		select_ret = select(max_fd+1, &r1_fd, &w1_fd, NULL, &timeout);
#ifdef DEBUG
		if (verbose)
		{
			printf("stage 1 poll returned %d\n", select_ret);
			printf("selected in stage 1: ");
			show_state(r1_fd, w1_fd);
			printf("\n");
		}
#endif
		if (select_ret == -1) ERR("select() error in stage 1");
	}

	/* stage 2: blocking poll */
	FD_ZERO(&w2_fd);
	FD_ZERO(&r2_fd);
	max_fd = 0;

	/* stage 2: poll sockin if we can accept another connection */
	if (active_connections < max_connections)
	{
		FD_SET(sockin, &r2_fd);
		max_fd = max(max_fd, sockin);
	}

	for (i=0; i<max_connections; i++)
	if (valid_socket(i))
	{
		/* poll matching write socket for backlogged sockets */
		if (backlog_in_size[i]) FD_SET(conn_in[i], &w2_fd);
		if (backlog_out_size[i]) FD_SET(conn_out[i], &w2_fd);

		/* poll matching read sockets for writeable sockets
		 * without a backlog */
		if (FD_ISSET(conn_in[i], &w1_fd) &&
		    !backlog_in_size[i])
			FD_SET(conn_out[i], &r2_fd);

		if (FD_ISSET(conn_out[i], &w1_fd) &&
		    !backlog_out_size[i])
			FD_SET(conn_in[i], &r2_fd);

		/* poll matching write sockets for readable sockets */
		if (FD_ISSET(conn_in[i], &r1_fd))
			FD_SET(conn_out[i], &w2_fd);

		if (FD_ISSET(conn_out[i], &r1_fd))
			FD_SET(conn_in[i], &w2_fd);

		max_fd = max(max_fd, max(conn_in[i], conn_out[i]));
	}

	/* poll! (indefinitely) */
	select_ret = select(max_fd+1, &r2_fd, &w2_fd, NULL, NULL);
	if (select_ret == 0)
		ERR("select()'s infinite timeout just timed out.");
	if (select_ret == -1) ERR("select() error in stage 2");

#ifdef DEBUG
	if (verbose)
	{
		printf("stage 2 poll returned %d\n", select_ret);
		printf("selected in stage 2: ");
		show_state(r2_fd, w2_fd);
		printf("\n");
	}
#endif

	/* handle incoming connection if there is one */
	if (FD_ISSET(sockin, &r2_fd)) accept_incoming();

	/* merge fd_sets */
	for (i=0; i<=max_fd; i++)
	{
		if (FD_ISSET(i, &r1_fd)) FD_SET(i, &r2_fd);
		if (FD_ISSET(i, &w1_fd)) FD_SET(i, &w2_fd);
	}

	for (i=0; i<max_connections; i++)
	{
		/* flush backlogs, if any */
		if (conn_in[i] != INVALID_SOCKET &&
			backlog_in_size[i] && FD_ISSET(conn_in[i], &w2_fd))
		{
			FD_CLR(conn_in[i], &w2_fd);
			flush_backlog(i, IN);
		}
		if (conn_out[i] != INVALID_SOCKET &&
			backlog_out_size[i] && FD_ISSET(conn_out[i], &w2_fd))
		{
			FD_CLR(conn_out[i], &w2_fd);
			flush_backlog(i, OUT);
		}

		/* plain forwarding */
		if (valid_socket(i) &&
			FD_ISSET(conn_in[i], &r2_fd) &&
			FD_ISSET(conn_out[i], &w2_fd) )
			bounce(conn_in[i], conn_out[i], i, OUT);

		if (valid_socket(i) &&
			FD_ISSET(conn_out[i], &r2_fd) &&
			FD_ISSET(conn_in[i], &w2_fd) )
			bounce(conn_out[i], conn_in[i], i, IN);
	}
}



int main(int argc, char **argv)
{
	struct sockaddr_in addrin;
	int i, sockopt;

	/* usage */
	if (argc < 3)
	{
		printf(
"TCP Port Forwarder\n(c) 2000-2003, Emil Mikulic.\n\n"
"usage: %s <src port> <remote ip>:<port> [-max <x>] [-v]\n"
"By default, the maximum number of connections is %d.\n"
"Verbosity is enabled using -v.\n\n", argv[0], max_connections);
		return EXIT_SUCCESS;
	}

	/* arg1: local port */
	localport = atoi(argv[1]);
	if (localport == 0)
	{
		printf("'%s' is a silly local port to use.\n", argv[1]);
		return EXIT_FAILURE;
	}

	/* arg2: target */
	remotehost = argv[2];
	for (i=0; i<(int)strlen(argv[2]); i++)
	{
		if (argv[2][i] == ':') {
			argv[2][i] = 0;
			i++;
			remoteport = atoi(argv[2]+i);
			break;
		}
	}

	if (argv[2][i] == 0)
	{
		printf("You didn't specify a remote port!\n");
		return EXIT_FAILURE;
	}

	if (remoteport == 0)
	{
		printf("'%s' is a silly remote port to use.\n", argv[2]+i);
		return EXIT_FAILURE;
	}

	/* arg3: max */
	if (argc > 3)
	{
		if (strcmp(argv[3],"-v") == 0)
		{
			verbose = 1;
		}
		else if (strcmp(argv[3],"-max") != 0)
		{
			printf("Unrecognised argument '%s'\n", argv[3]);
			return EXIT_FAILURE;
		}
		else
		{
			if (argc < 5)
			{
				printf("You didn't specify the maximum \
number of connections.\n");
				return EXIT_FAILURE;
			}
			max_connections = atoi(argv[4]);
			if (max_connections < 1 || max_connections > 65535)
			{
				printf("'%s' is a silly maximum.\n", argv[4]);
				return EXIT_FAILURE;
			}

			if (argc > 5 && strcmp(argv[5],"-v") == 0)
				verbose = 1;
		}
	}

#ifdef _WIN32
	init_winsock();
#endif

	if (verbose)
		printf("Forwarding port %d to %s:%d.\n",
			localport, remotehost, remoteport);

	conn_in = (SOCKET*)malloc(max_connections * sizeof(SOCKET));
	conn_out = (SOCKET*)malloc(max_connections * sizeof(SOCKET));
	backlog_in = (char**)malloc(max_connections * sizeof(char*));
	backlog_in_size = (int*)malloc(max_connections * sizeof(int));
	backlog_in_pos = (int*)malloc(max_connections * sizeof(int));
	backlog_out = (char**)malloc(max_connections * sizeof(char*));
	backlog_out_size = (int*)malloc(max_connections * sizeof(int));
	backlog_out_pos = (int*)malloc(max_connections * sizeof(int));

	if (conn_in == NULL
	 || conn_out == NULL
	 || backlog_in == NULL
	 || backlog_in_size == NULL
	 || backlog_in_pos == NULL
	 || backlog_out == NULL
	 || backlog_out_size == NULL
	 || backlog_out_pos == NULL
	 )
		ERR("Can't allocate enough memory to initialize.");

	for (i=0; i<max_connections; i++)
	{
		conn_in[i] = conn_out[i] = INVALID_SOCKET;
		backlog_in[i] = (char*)malloc(BACKLOG_SIZE);
		backlog_out[i] = (char*)malloc(BACKLOG_SIZE);
		backlog_in_size[i] = backlog_out_size[i] =
			backlog_in_pos[i] = backlog_out_pos[i] = 0;

		if (!backlog_in[i] || !backlog_out[i])
			ERR("Can't allocate enough memory for backlogs.\n\
Try decreasing the number of maximum connections.");
	}

	(void) signal(SIGTERM, term_signal);
	(void) signal(SIGINT, term_signal);
#ifndef _WIN32
	(void) signal(SIGPIPE, broken_pipe);
#endif

	/* create the incoming socket */
	sockin = socket(AF_INET, SOCK_STREAM, 0);
	if (sockin < 0) ERR("problem creating incoming socket");

	/* fill out a sockaddr struct */
	addrin.sin_family = AF_INET;
	addrin.sin_port = htons(localport);
	addrin.sin_addr.s_addr = INADDR_ANY;
	memset(&(addrin.sin_zero), 0, 8);

	/* reuse address */
	sockopt = 1;
	if (setsockopt(sockin, SOL_SOCKET, SO_REUSEADDR, (char*)&sockopt,
				sizeof(sockopt)) < 0)
		ERR("can't REUSEADDR");

	/* bind sockin to the incoming port */
	if (bind(sockin, (struct sockaddr *)&addrin,
		sizeof(struct sockaddr)) < 0)
	{
#ifndef _WIN32
		if (getuid() != 0 && localport < 1024)
			ERR("problem binding incoming socket\n\
You need to be root to bind to a port under 1024.");
		else
#endif
			ERR("problem binding incoming socket\n\
Maybe it's already in use?");
	}

	/* listen on the socket */
	if (listen(sockin, max_connections) < 0)
		ERR("problem listen()ing to incoming socket");

	if (verbose) printf("Waiting for connections...\n");

	while (1) poll_conn();

	/* UNREACHABLE */
	return EXIT_FAILURE;
}
