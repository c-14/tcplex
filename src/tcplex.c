#define _POSIX_C_SOURCE 201112L
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h> // for getpid/daemon/getopt
#include <sys/types.h>
#include <sys/socket.h>
#include <sysexits.h>

#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/util.h>

#ifndef evbuffer_add_buffer_reference
#define evbuffer_add_buffer_reference(x, y) evbuffer_add_buffer(x, y)
#endif

struct tplexee {
	struct bufferevent *client;
	struct bufferevent *server;
	struct event_base *base;
	struct evdns_base *dns;
	struct tplexee *next;
	struct tplexee *head;
	struct plexes *br;
};

struct plexes {
	struct tplexee *plex;
	struct plexes *next;
	struct plexes *prev;
	struct plex_data *br;
};

struct plex_data {
	struct event_base *base;
	struct evdns_base *dns;
	struct event **int_ev;
	struct event **term_ev;
	struct event **serv_ev;
	evutil_socket_t serv;
	int numhosts;
	char **hosts;
	struct plexes *connections; 
};

static inline void sigresetraise(int signum)
{
	struct sigaction act;
	act.sa_handler = SIG_DFL;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);

	if (sigaction(signum, &act, NULL) != 0) {
		/* TODO: Switch to log/exit program here */
		perror("sigaction");
	}
	kill(getpid(), signum);
}

struct event *sigregister(struct event_base *base, evutil_socket_t signal, event_callback_fn cb, void *arg)
{
	struct event *ev;

	if ((ev = evsignal_new(base, signal, cb, arg)) == NULL) {
		return NULL;
	}
	if (evsignal_add(ev, NULL) == -1) {
		return NULL;
	}

	return ev;
}

void safe_event_free(struct event *ev)
{
	if (ev != NULL)
		event_free(ev);
}

void safe_bufferevent_free(struct bufferevent **bev)
{
	if (*bev) {
		bufferevent_free(*bev);
		*bev = NULL;
	}
}

void free_tplexee(struct tplexee *plex)
{
	struct tplexee *next;

	if (plex == NULL)
		return;

	safe_bufferevent_free(&plex->head->client);
	while (plex != NULL) {
		next = plex->next;
		safe_bufferevent_free(&plex->server);
		plex->server = NULL;
		free(plex);
		plex = next;
	}
}

void cleanup(struct plex_data *data)
{
	struct plexes *next, *conn = data->connections;

	while (conn != NULL) {
		next = conn->next;
		free_tplexee(conn->plex);
		free(conn);
		conn = next;
	}

	if (data->serv != 0 && evutil_closesocket(data->serv) == -1)
		perror("close");

	safe_event_free(*(data->int_ev));
	safe_event_free(*(data->term_ev));
	safe_event_free(*(data->serv_ev));
	evdns_base_free(data->dns, 0);
	event_base_free(data->base);
}

void exit_cb(evutil_socket_t a, short b, void *arg)
{
	struct plex_data *data = arg;
	(void)a;
	(void)b;

	event_base_loopbreak(data->base);
	cleanup(data);
	sigresetraise(SIGTERM);
}

void s_readcb(struct bufferevent *bev, void *ctx)
{
	struct tplexee *plex = ctx;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *output = bufferevent_get_output(plex->client);

	evbuffer_add_buffer_reference(output, input);
	evbuffer_drain(input, -1);
}


void c_readcb(struct bufferevent *bev, void *ctx)
{
	struct tplexee *plex = ctx;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *output;

	if (!plex->server)
		return;

	output = bufferevent_get_output(plex->server);

	evbuffer_add_buffer_reference(output, input);
	evbuffer_drain(input, -1);
}

void tplex(struct tplexee *plex)
{
	struct evbuffer *input = bufferevent_get_input(plex->client);
	struct evbuffer *output = bufferevent_get_output(plex->server);

	evbuffer_add_buffer_reference(output, input);
	evbuffer_drain(input, -1);
}

void c_eventcb(struct bufferevent *bev, short events, void *ctx)
{
	struct tplexee *plex = ctx;

	(void)bev;
	if (events & BEV_EVENT_ERROR) {
		fputs("Got an error from client\n", stderr);
		/* fprintf(stderr, "Got an error from client %s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())); */
	}
	if (plex->br->prev)
		plex->br->prev->next = plex->br->next;
	if (plex->br->next)
		plex->br->next->prev = plex->br->prev;
	if (plex->br->br->connections == plex->br)
		plex->br->br->connections = plex->br->next;
	free(plex->br);
	free_tplexee(plex->head);
}

void s_eventcb(struct bufferevent *bev, short events, void *ctx)
{
	struct tplexee *plex = ctx;

	if (events & BEV_EVENT_EOF || events & BEV_EVENT_ERROR) {
		if (plex->server == bev && plex->next) {
			struct tplexee *tmp;
			plex->server = plex->next->server;
			bufferevent_enable(plex->server, EV_READ);
			bufferevent_setcb(plex->server, s_readcb, NULL, s_eventcb, plex);
			bufferevent_setcb(plex->client, c_readcb, NULL, c_eventcb, plex);
			tmp = plex->next;
			plex->next = plex->next->next;
			free(tmp);
		} else if (plex->server != bev) {
			struct tplexee *prev, *find = plex;
			while (find->server != bev) {
				prev = find;
				find = find->next;
			}
			prev->next = find->next;
			free(plex);
		} else {
			fputs("Can't replace server, no substitutes left.\n", stderr);
			plex->server = NULL;
		}
	}
	bufferevent_free(bev);
}

void connectcb(struct bufferevent *bev, short events, void *ctx)
{
	struct tplexee *plex = ctx;

	if (plex->server) {
		struct tplexee *next;
		if ((next = malloc(sizeof(struct tplexee))) == NULL) {
			perror("malloc");
			exit(EX_SOFTWARE);
		}
		next->client = plex->client;
		next->server = bev;
		next->next = plex->next;
		next->head = plex;
		plex->next = next;
	} else if (events & BEV_EVENT_CONNECTED) {
		plex->server = bev;
		bufferevent_enable(plex->server, EV_READ);
		bufferevent_enable(plex->client, EV_READ);
		bufferevent_setcb(plex->server, s_readcb, NULL, s_eventcb, plex);
		bufferevent_setcb(plex->client, c_readcb, NULL, c_eventcb, plex);
		tplex(plex);
	} else if (events & BEV_EVENT_ERROR) {
		bufferevent_free(bev);
	}
}

static int create_plexer(int family, int socktype, int protocol, struct sockaddr *addr, socklen_t addrlen, evutil_socket_t *serv)
{
	evutil_socket_t sock;
	int yes = 1;

	sock = socket(family, socktype, protocol);
	if (evutil_make_socket_nonblocking(sock) == -1) {
		if (evutil_closesocket(sock) == -1)
			perror("close");
		return -1;
	}

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
		perror("setsockopt");
		if (evutil_closesocket(sock) == -1)
			perror("close");
		return -1;
	} 

	if (bind(sock, addr, addrlen) == -1) {
		perror("bind");
		printf("%d\n", errno);
		if (evutil_closesocket(sock) == -1)
			perror("close");
		return -1;
	}

	if (listen(sock, 3) == -1) {
		perror("listen");
		if (evutil_closesocket(sock) == -1)
			perror("close");
		return -1;
	}

	*serv = sock;
	return 0;
}

void create_plexer_cb(int err, struct evutil_addrinfo *addr, void *ptr)
{
	if (err != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", evutil_gai_strerror(err));
	} else {
		if (create_plexer(addr->ai_family, addr->ai_socktype, addr->ai_protocol, addr->ai_addr, addr->ai_addrlen, ptr) != 0)
			exit(EX_TEMPFAIL);
		evutil_freeaddrinfo(addr);
		addr = NULL;
	}
}

static int parse_listener(char *arg, struct evdns_base *dns, evutil_socket_t *serv)
{
	if (*arg == '[') {
		struct sockaddr_in6 sa;
		char *end, *c = strchr(arg, ']');
		unsigned long int i = strtoul(c + 2, &end, 10);
		ptrdiff_t len;
		char *name;

		if (c == NULL) {
			fputs("Host must be in form <host>:<port>\n", stderr);
			return EX_USAGE;
		}

		len = c - arg - 1;
		name = malloc(len + 1);
		if (name == NULL) {
			return EX_OSERR;
		} else if (*end != '\0') {
			fprintf(stderr, "Cannot parse %s as port.\n", c);
			free(name);
			return EX_USAGE;
		} else if (i <= 0 || i > 65535) {
			fprintf(stderr, "Port must be between 0 and 65535, not %lu\n", i);
			free(name);
			return EX_USAGE;
		}

		memset(&sa, 0, sizeof(sa));
		sa.sin6_family = AF_INET6;
		sa.sin6_port = htons(i);

		strncpy(name, arg + 1, len);
		name[len] = '\0';
		if (inet_pton(AF_INET6, name, &(sa.sin6_addr)) != 1) {
			fprintf(stderr, "Not a valid IPv6 address: %s\n", name);
		}
		free(name);
		create_plexer(PF_INET6, SOCK_STREAM, IPPROTO_TCP, (struct sockaddr*)&sa, sizeof(sa), serv);
	} else {
		struct evutil_addrinfo hints;
		char *c = strchr(arg, ':');
		ptrdiff_t len;
		char *name;

		if (c == NULL) {
			fputs("Host must be in form <host>:<port>\n", stderr);
			return EX_USAGE;
		}

		len = c - arg;
		name = malloc(len + 1);
		if (name == NULL) {
			return EX_OSERR;
		}


		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		strncpy(name, arg, len);
		name[len] = '\0';
		evdns_getaddrinfo(dns, name, c + 1, &hints, create_plexer_cb, serv);
		free(name);
	}

	return 0;
}

static int create_plexee(struct tplexee *plex, int family, int socktype, int protocol, struct sockaddr *addr, socklen_t addrlen)
{
	evutil_socket_t sock;
	struct bufferevent *bev;

	sock = socket(family, socktype, protocol);
	if (evutil_make_socket_nonblocking(sock) == -1 ||
			(bev = bufferevent_socket_new(plex->base, sock, BEV_OPT_CLOSE_ON_FREE)) == NULL) {
		if (evutil_closesocket(sock) == -1)
			perror("close");
		perror("socket_new");
		return -1;
	}

	if (bufferevent_socket_connect(bev, addr, addrlen) == -1) {
		perror("connect");
		bufferevent_free(bev);
		return -1;
	}

	bufferevent_setcb(bev, NULL, NULL, connectcb, plex);
	return 0;
}

void create_plexees(int err, struct evutil_addrinfo *addr, void *ptr)
{
	struct tplexee *plex = ptr;

	if (err != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", evutil_gai_strerror(err));
	} else {
		struct evutil_addrinfo *ai;

		for (ai = addr; ai != NULL; ai = ai->ai_next) {
			create_plexee(plex, ai->ai_family, ai->ai_socktype, ai->ai_protocol, ai->ai_addr, ai->ai_addrlen);
		}
		evutil_freeaddrinfo(addr);
		addr = NULL;
	}
}

static int parse_host(char *arg, struct tplexee *plex)
{
	if (*arg == '[') {
		struct sockaddr_in6 sa;
		char *end, *c = strchr(arg, ']');
		unsigned long int i = strtoul(c + 2, &end, 10);
		ptrdiff_t len;
		char *name;

		if (c == NULL) {
			fputs("Host must be in form <host>:<port>\n", stderr);
			return EX_USAGE;
		}

		len = c - arg - 1;
		name = malloc(len + 1);
		if (name == NULL) {
			return EX_OSERR;
		} else if (*end != '\0') {
			fprintf(stderr, "Cannot parse %s as port.\n", c);
			return EX_USAGE;
		} else if (i <= 0 || i > 65535) {
			fprintf(stderr, "Port must be between 0 and 65535, not %lu\n", i);
			return EX_USAGE;
		}

		memset(&sa, 0, sizeof(sa));
		sa.sin6_family = AF_INET6;
		sa.sin6_port = htons(i);

		strncpy(name, arg + 1, len);
		name[len] = '\0';
		if (inet_pton(AF_INET6, name, &(sa.sin6_addr)) != 1) {
			fprintf(stderr, "Not a valid IPv6 address: %s\n", name);
		}
		free(name);
		create_plexee(plex, PF_INET6, SOCK_STREAM, IPPROTO_TCP, (struct sockaddr*)&sa, sizeof(sa));
	} else {
		struct evutil_addrinfo hints;
		char *c = strchr(arg, ':');
		ptrdiff_t len;
		char *name;

		if (c == NULL) {
			fputs("Host must be in form <host>:<port>\n", stderr);
			return EX_USAGE;
		}

		len = c - arg;
		name = malloc(len + 1);
		if (name == NULL) {
			return EX_OSERR;
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		strncpy(name, arg, len);
		name[len] = '\0';
		evdns_getaddrinfo(plex->dns, name, c + 1, &hints, create_plexees, plex);
		free(name);
	}

	return 0;
}

void setup_plexees(struct bufferevent *bev, struct plex_data *data)
{
	struct plexes *conn;
	struct tplexee *plex;

	if ((conn = malloc(sizeof(struct plexes))) == NULL ||
			(plex = malloc(sizeof(struct tplexee))) == NULL) {
		perror("malloc");
		exit(EX_SOFTWARE);
	}
	plex->base = data->base;
	plex->dns = data->dns;
	plex->client = bev;
	plex->server = NULL;
	plex->next = NULL;
	plex->head = plex;
	plex->br = conn;
	conn->plex = plex;
	conn->next = data->connections;
	if (conn->next)
		conn->next->prev = conn;
	conn->prev = NULL;
	conn->br = data;
	data->connections = conn;

	for (int i = 0; i < data->numhosts; i++) {
		if (parse_host(data->hosts[i], plex) != 0) {
			free(plex);
			exit(EX_SOFTWARE);
		}
	}
}

void eventcb(struct bufferevent *bev, short events, void *ctx)
{
	(void)ctx;
	if (events & BEV_EVENT_EOF) {
		/* TODO: replace with log */
		fputs("Got EOF in connection during accept\n", stderr);
	} else if (events & BEV_EVENT_ERROR) {
		/* TODO: replace with log */
		/* fprintf(stderr, "Got an error during accept: %s\n", evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR())); */
	}
	bufferevent_free(bev);
}

void do_accept(evutil_socket_t serv, short event, void *arg)
{
	struct plex_data *data = arg;
	struct sockaddr_storage ss;
	socklen_t slen = sizeof(ss);
	int fd = accept(serv, (struct sockaddr*)&ss, &slen);

	(void)event;
	if (fd < 0) {
		perror("accept");
	} else {
		struct bufferevent *bev;
		if (evutil_make_socket_nonblocking(fd) == -1 ||
				(bev = bufferevent_socket_new(data->base, fd, BEV_OPT_CLOSE_ON_FREE)) == NULL) {
			if (evutil_closesocket(fd) == -1)
				perror("close");
			perror("socket_new");
			return;
		}
		bufferevent_setcb(bev, NULL, NULL, eventcb, data);
		setup_plexees(bev, data);
	}
}

static void usage(bool err)
{
	fputs("usage: tcplex [-d] [-h] [-v] [-l <host:port> ] <host:port ...>\n", err ? stderr : stdout);
}

int main(int argc, char *argv[])
{
	struct event_base *base;
	struct event *int_event = NULL, *term_event = NULL, *serv_event = NULL;
	struct plex_data data;
	char *listener = "localhost:7463";
	int i;

	if (argc == 1) {
		usage(true);
		fputs("\tAt least two hosts required\n", stderr);
		return EX_USAGE;
	}

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
			if (daemon(1, 0) != 0) {
				perror("daemon");
				return EX_OSERR;
			}
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(false);
			return EX_OK;
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			puts("tcplex version 0.1");
			return EX_OK;
		} else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--listen") == 0) {
			listener = argv[++i];
		} else if (i + 1 >= argc) {
			usage(true);
			fputs("\tAt least two hosts required\n", stderr);
			return EX_USAGE;
		} else {
			break;
		}
	}

	if ((base = event_base_new()) == NULL) {
		return EX_SOFTWARE;
	}

	if ((data.dns = evdns_base_new(base, 1)) == NULL) {
		event_base_free(base);
		return EX_SOFTWARE;
	}

	data.base = base;
	data.int_ev = &int_event;
	data.term_ev = &term_event;
	data.serv_ev = &serv_event;
	data.serv = 0;
	data.hosts = argv + i;
	data.numhosts = argc - i;
	data.connections = NULL;

	if ((int_event = sigregister(base, SIGINT, exit_cb, &data)) == NULL ||
			(term_event = sigregister(base, SIGTERM, exit_cb, &data)) == NULL) {
		cleanup(&data);
		return EX_SOFTWARE;
	}

	if (parse_listener(listener, data.dns, &data.serv) != 0 ||
			(serv_event = event_new(base, data.serv, EV_READ|EV_PERSIST, do_accept, &data)) == NULL ||
						event_add(serv_event, NULL) == -1) {
		cleanup(&data);
		return EX_SOFTWARE;
	}

	event_base_dispatch(base);

	cleanup(&data);
	return EX_OK;
}
