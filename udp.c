/* Copyright (c) 2007-2009, UNINETT AS
 * Copyright (c) 2012-2013, 2017, NORDUnet A/S */
/* See LICENSE for licensing information. */

#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#ifdef SYS_SOLARIS9
#include <fcntl.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <ctype.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <regex.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include "radsecproxy.h"
#include "hostport.h"

#ifdef RADPROT_UDP
#include "debug.h"
#include "util.h"

static void setprotoopts(struct commonprotoopts *opts);
static char **getlistenerargs();
void *udpserverrd(void *arg);
int clientradputudp(struct server *server, unsigned char *rad, int radlen);
void addclientudp(struct client *client);
void addserverextraudp(struct clsrvconf *conf);
void udpsetsrcres();
void initextraudp();

static const struct protodefs protodefs = {
    "udp",
    NULL, /* secretdefault */
    SOCK_DGRAM, /* socktype */
    "1812", /* portdefault */
    REQUEST_RETRY_COUNT, /* retrycountdefault */
    10, /* retrycountmax */
    REQUEST_RETRY_INTERVAL, /* retryintervaldefault */
    60, /* retryintervalmax */
    DUPLICATE_INTERVAL, /* duplicateintervaldefault */
    setprotoopts, /* setprotoopts */
    getlistenerargs, /* getlistenerargs */
    udpserverrd, /* listener */
    NULL, /* connecter */
    NULL, /* clientconnreader */
    clientradputudp, /* clientradput */
    addclientudp, /* addclient */
    addserverextraudp, /* addserverextra */
    udpsetsrcres, /* setsrcres */
    initextraudp /* initextra */
};

struct client_sock {
    struct sockaddr_storage *source;
    int socket;
};

static struct list *client_sock;
static struct gqueue *server_replyq = NULL;

static struct addrinfo *srcres = NULL;
static uint8_t handle;
static struct commonprotoopts *protoopts = NULL;

const struct protodefs *udpinit(uint8_t h) {
    handle = h;
    return &protodefs;
}

static void setprotoopts(struct commonprotoopts *opts) {
    protoopts = opts;
}

static char **getlistenerargs() {
    return protoopts ? protoopts->listenargs : NULL;
}

void udpsetsrcres() {
    if (!srcres)
	srcres =
            resolvepassiveaddrinfo(protoopts ? protoopts->sourcearg : NULL,
                                   AF_UNSPEC, NULL, protodefs.socktype);
}

void removeudpclientfromreplyq(struct client *c) {
    struct list_node *n;
    struct request *r;

    /* lock the common queue and remove replies for this client */
    pthread_mutex_lock(&c->replyq->mutex);
    for (n = list_first(c->replyq->entries); n; n = list_next(n)) {
	r = (struct request *)n->data;
	if (r->from == c)
	    r->from = NULL;
    }
    pthread_mutex_unlock(&c->replyq->mutex);
}

static int addr_equal(struct sockaddr *a, struct sockaddr *b) {
    switch (a->sa_family) {
    case AF_INET:
	return !memcmp(&((struct sockaddr_in*)a)->sin_addr,
		       &((struct sockaddr_in*)b)->sin_addr,
		       sizeof(struct in_addr)) &&
               (((struct sockaddr_in*)a)->sin_port == ((struct sockaddr_in*)b)->sin_port);
    case AF_INET6:
	return IN6_ARE_ADDR_EQUAL(&((struct sockaddr_in6*)a)->sin6_addr,
				  &((struct sockaddr_in6*)b)->sin6_addr) &&
                  (((struct sockaddr_in6*)a)->sin6_port == ((struct sockaddr_in6*)b)->sin6_port);
    default:
	/* Must not reach */
	return 0;
    }
}

uint16_t port_get(struct sockaddr *sa) {
    switch (sa->sa_family) {
    case AF_INET:
	return ntohs(((struct sockaddr_in *)sa)->sin_port);
    case AF_INET6:
	return ntohs(((struct sockaddr_in6 *)sa)->sin6_port);
    }
    return 0;
}

/* exactly one of client and server must be non-NULL */
/* return who we received from in *client or *server */
/* return from in sa if not NULL */
int radudpget(int s, struct client **client, struct server **server, unsigned char **buf) {
    int cnt, len;
    unsigned char init_buf[4];
    struct sockaddr_storage from;
    struct sockaddr *fromcopy;
    socklen_t fromlen = sizeof(from);
    struct clsrvconf *p;
    struct list_node *node;
    struct client *c = NULL;
    struct timeval now;
    char tmp[INET6_ADDRSTRLEN];

    for (;;) {
        if (*buf) {
            free(*buf);
            *buf = NULL;
        }

        cnt = recvfrom(s, init_buf, 4, MSG_PEEK | MSG_TRUNC, (struct sockaddr *)&from, &fromlen);
        if (cnt == -1) {
            debug(DBG_ERR, "radudpget: recv failed - %s", strerror(errno));
            continue;
        }

        p = client
            ? find_clconf(handle, (struct sockaddr *)&from, NULL, NULL)
            : find_srvconf(handle, (struct sockaddr *)&from, NULL);
        if (!p) {
            debug(DBG_WARN, "radudpget: got packet from wrong or unknown UDP peer %s, ignoring", addr2string((struct sockaddr *)&from, tmp, sizeof(tmp)));
            if (recv(s, init_buf, 4, 0) == -1)
                debug(DBG_ERR, "radudpget: recv failed - %s", strerror(errno));
            continue;
        }

        len = get_checked_rad_length(init_buf);
        if (len <= 0) {
            debug(DBG_WARN, "radudpget: invalid message length: %d", -len);
            if (recv(s, init_buf, 4, 0) == -1)
                debug(DBG_ERR, "radudpget: recv failed - %s", strerror(errno));
            continue;
        }
        if (len > 4096) {
            debug(DBG_WARN, "radudpget: length too big");
            if (recv(s, init_buf, 4, 0) == -1)
                debug(DBG_ERR, "radudpget: recv failed - %s", strerror(errno));
            continue;
        }

        *buf = malloc(len);
        if (!*buf) {
            debug(DBG_ERR, "radudpget: malloc failed");
            if (recv(s, init_buf, 4, 0) == -1)
                debug(DBG_ERR, "radudpget: recv failed - %s", strerror(errno));
            continue;
        }

        cnt = recv(s, *buf, len, MSG_TRUNC);
        debug(DBG_DBG, "radudpget: got %d bytes from %s", cnt, addr2string((struct sockaddr *)&from, tmp, sizeof(tmp)));

        if (cnt < len) {
            debug(DBG_WARN, "radudpget: packet smaller than length field in radius header");
            continue;
        }
        if (cnt > len)
            debug(DBG_DBG, "radudpget: packet was padded with %d bytes", cnt - len);

        if (client) {
            *client = NULL;
            pthread_mutex_lock(p->lock);
            for (node = list_first(p->clients); node;) {
                c = (struct client *)node->data;
                node = list_next(node);
                if (s != c->sock)
                    continue;
                gettimeofday(&now, NULL);
                if (!*client && addr_equal((struct sockaddr *)&from, c->addr)) {
                    c->expiry = now.tv_sec + 60;
                    *client = c;
                }
                if (c->expiry >= now.tv_sec)
                    continue;

                debug(DBG_DBG, "radudpget: removing expired client (%s)", addr2string(c->addr, tmp, sizeof(tmp)));
                removeudpclientfromreplyq(c);
                c->replyq = NULL; /* stop removeclient() from removing common udp replyq */
                removelockedclient(c);
                break;
            }
            if (!*client) {
                fromcopy = addr_copy((struct sockaddr *)&from);
                if (!fromcopy) {
                    pthread_mutex_unlock(p->lock);
                    continue;
                }
                c = addclient(p, 0);
                if (!c) {
                    free(fromcopy);
                    pthread_mutex_unlock(p->lock);
                    continue;
                }
                c->sock = s;
                c->addr = fromcopy;
                gettimeofday(&now, NULL);
                c->expiry = now.tv_sec + 60;
                *client = c;
            }
            pthread_mutex_unlock(p->lock);
        } else if (server)
            *server = p->servers;
        break;
    }
    return len;
}

int clientradputudp(struct server *server, unsigned char *rad, int radlen) {
    struct clsrvconf *conf = server->conf;
    struct addrinfo *ai;
    char tmp[INET6_ADDRSTRLEN];

    if (radlen <= 0) {
        debug(DBG_ERR, "clientradputudp: invalid buffer (length)");
        return 0;
    }

    ai = ((struct hostportres *)list_first(conf->hostports)->data)->addrinfo;
    if (sendto(server->sock, rad, radlen, 0, ai->ai_addr, ai->ai_addrlen) >= 0) {
	debug(DBG_DBG, "clienradputudp: sent UDP of length %zu to %s port %d", radlen, addr2string(ai->ai_addr, tmp, sizeof(tmp)), port_get(ai->ai_addr));
	return 1;
    }

    debug(DBG_WARN, "clientradputudp: send failed");
    return 0;
}

void *udpclientrd(void *arg) {
    struct server *server;
    unsigned char *buf;
    int *s = (int *)arg;
    int len = 0;

    for (;;) {
        server = NULL;
        len = radudpget(*s, NULL, &server, &buf);
        replyh(server, buf, len);
        buf = NULL;
    }
}

void *udpserverrd(void *arg) {
    struct request *rq;
    int *sp = (int *)arg;

    for (;;) {
        rq = newrequest();
        if (!rq) {
            sleep(5); /* malloc failed */
            continue;
        }
        rq->buflen = radudpget(*sp, &rq->from, NULL, &rq->buf);
        rq->udpsock = *sp;
        gettimeofday(&rq->created, NULL);
        radsrv(rq);
    }
    free(sp);
    return NULL;
}

void *udpserverwr(void *arg) {
    struct gqueue *replyq = (struct gqueue *)arg;
    struct request *reply;
    struct sockaddr_storage to;

    for (;;) {
	pthread_mutex_lock(&replyq->mutex);
	while (!(reply = (struct request *)list_shift(replyq->entries))) {
	    debug(DBG_DBG, "udp server writer, waiting for signal");
	    pthread_cond_wait(&replyq->cond, &replyq->mutex);
	    debug(DBG_DBG, "udp server writer, got signal");
	}
	/* do this with lock, udpserverrd may set from = NULL if from expires */
	if (reply->from)
	    memcpy(&to, reply->from->addr, SOCKADDRP_SIZE(reply->from->addr));
	pthread_mutex_unlock(&replyq->mutex);
	if (reply->from) {
	    if (sendto(reply->udpsock, reply->replybuf, reply->replybuflen, 0, (struct sockaddr *)&to, SOCKADDR_SIZE(to)) < 0)
		debug(DBG_WARN, "udpserverwr: send failed");
	}
	debug(DBG_DBG, "udpserverwr: refcount %d", reply->refcount);
	freerq(reply);
    }
}

void addclientudp(struct client *client) {
    client->replyq = server_replyq;
}

void addserverextraudp(struct clsrvconf *conf) {
    struct addrinfo *source = NULL, *tmpaddrinfo;
    struct list_node *entry;
    char tmp[32];

    assert(list_first(conf->hostports) != NULL);

    if(conf->source) {
        source = resolvepassiveaddrinfo(conf->source, AF_UNSPEC, NULL, protodefs.socktype);
        if(!source)
            debug(DBG_WARN, "addserver: could not resolve source address to bind for server %s, using default", conf->name);
    }

    if (client_sock == NULL) {
        client_sock = list_create();
    }
    for (tmpaddrinfo = source ? source : srcres; tmpaddrinfo; tmpaddrinfo = tmpaddrinfo->ai_next) {
        if (tmpaddrinfo->ai_family == AF_UNSPEC || tmpaddrinfo->ai_family == ((struct hostportres *)list_first(conf->hostports)->data)->addrinfo->ai_family) {
            for(entry = list_first(client_sock); entry; entry = list_next(entry)){
                if (memcmp(tmpaddrinfo->ai_addr, ((struct client_sock*)entry->data)->source, tmpaddrinfo->ai_addrlen) == 0) {
                    conf->servers->sock = ((struct client_sock*)entry->data)->socket;
                    debug(DBG_DBG, "addserverextraudp: reusing existing socket #%d (%s) for server %s", conf->servers->sock, addr2string(tmpaddrinfo->ai_addr, tmp, sizeof(tmp)), conf->name);
                    break;
                }
            }
            if (conf->servers->sock < 0) {
                struct client_sock* cls = malloc(sizeof(struct client_sock));
                if (!cls)
                    debugx(1,DBG_ERR, "addserverextraudp: malloc failed");
                cls->socket = bindtoaddr(tmpaddrinfo, tmpaddrinfo->ai_family,0);
                cls->source = malloc(sizeof(struct sockaddr_storage));
                if (!cls->source)
                    debugx(1,DBG_ERR, "addserverextraudp: malloc failed");
                memcpy(cls->source, tmpaddrinfo->ai_addr, tmpaddrinfo->ai_addrlen);
                debug(DBG_DBG, "addserverextraudp: creating new socket #%d (%s) for server %s", cls->socket, addr2string((struct sockaddr *)cls->source, tmp, sizeof(tmp)), conf->name);
                if (!list_push(client_sock, cls))
                    debugx(1,DBG_ERR, "addserverextraudp: malloc failed");
                conf->servers->sock = cls->socket;
                break;
            }
        }
    }
    if (conf->servers->sock < 0)
        debugx(1, DBG_ERR, "addserver: failed to create client socket for server %s", conf->name);

    if (source)
        freeaddrinfo(source);
}

void initextraudp() {
    pthread_t clth, srvth;
    struct list_node *entry;

    if (srcres) {
	freeaddrinfo(srcres);
	srcres = NULL;
    }

    for (entry = list_first(client_sock); entry; entry = list_next(entry)) {
        debug(DBG_DBG, "initextraudp: spinning up clientrd thread for socket #%d", ((struct client_sock*)entry->data)->socket);
        if (pthread_create(&clth, &pthread_attr, udpclientrd, (void *)&((struct client_sock*)entry->data)->socket))
            debugx(1, DBG_ERR, "pthread_create failed");
    }

    if (find_clconf_type(handle, NULL)) {
	server_replyq = newqueue();
	if (pthread_create(&srvth, &pthread_attr, udpserverwr, (void *)server_replyq))
	    debugx(1, DBG_ERR, "pthread_create failed");
    }
}
#else
const struct protodefs *udpinit(uint8_t h) {
    return NULL;
}
#endif

/* Local Variables: */
/* c-file-style: "stroustrup" */
/* End: */
