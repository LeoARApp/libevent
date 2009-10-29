/*
 * Copyright (c) 2009 Niels Provos, Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>

#ifdef HAVE_CONFIG_H
#include "event-config.h"
#endif

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#endif
#include <errno.h>
#ifdef _EVENT_HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef _EVENT_HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef _EVENT_HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include "mm-internal.h"
#include "util-internal.h"
#include "log-internal.h"
#ifdef WIN32
#include "iocp-internal.h"
#endif

struct evconnlistener_ops {
	int (*enable)(struct evconnlistener *);
	int (*disable)(struct evconnlistener *);
	void (*destroy)(struct evconnlistener *);
	evutil_socket_t (*getfd)(struct evconnlistener *);
};

struct evconnlistener {
	const struct evconnlistener_ops *ops;
	evconnlistener_cb cb;
	void *user_data;
	unsigned flags;
};

struct evconnlistener_event {
	struct evconnlistener base;
	struct event listener;
};

#ifdef WIN32
struct evconnlistener_iocp {
	struct evconnlistener base;
	evutil_socket_t fd;
	struct event_iocp_port *port;
	int n_accepting;
	struct accepting_socket **accepting;
};
#endif

static int event_listener_enable(struct evconnlistener *);
static int event_listener_disable(struct evconnlistener *);
static void event_listener_destroy(struct evconnlistener *);
static evutil_socket_t event_listener_getfd(struct evconnlistener *);

static const struct evconnlistener_ops evconnlistener_event_ops = {
	event_listener_enable,	
	event_listener_disable,	
	event_listener_destroy,	
	event_listener_getfd
};

static void listener_read_cb(evutil_socket_t, short, void *);

struct evconnlistener *
evconnlistener_new(struct event_base *base,
    evconnlistener_cb cb, void *ptr, unsigned flags, int backlog,
    evutil_socket_t fd)
{
	struct evconnlistener_event *lev;
	if (backlog > 0) {
		if (listen(fd, backlog) < 0)
			return NULL;
	} else if (backlog < 0) {
		if (listen(fd, 128) < 0)
			return NULL;
	}
	lev = mm_calloc(1, sizeof(struct evconnlistener_event));
	if (!lev)
		return NULL;
	lev->base.ops = &evconnlistener_event_ops;
	lev->base.cb = cb;
	lev->base.user_data = ptr;
	lev->base.flags = flags;
	event_assign(&lev->listener, base, fd, EV_READ|EV_PERSIST,
	    listener_read_cb, lev);
	evconnlistener_enable(&lev->base);
	return &lev->base;
}

struct evconnlistener *
evconnlistener_new_bind(struct event_base *base, evconnlistener_cb cb,
    void *ptr, unsigned flags, int backlog, const struct sockaddr *sa,
    int socklen)
{
	evutil_socket_t fd;
	int on = 1;
	int family = sa ? sa->sa_family : AF_UNSPEC;

	if (backlog == 0)
		return NULL;
	fd = socket(family, SOCK_STREAM, 0);
	if (fd == -1)
		return NULL;
	if (evutil_make_socket_nonblocking(fd) < 0)
		return NULL;

#ifndef WIN32
	if (flags & LEV_OPT_CLOSE_ON_EXEC) {
		if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
			EVUTIL_CLOSESOCKET(fd);
			return NULL;
		}
	}
#endif

	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void*)&on, sizeof(on));
	if (flags & LEV_OPT_REUSEABLE) {
		evutil_make_listen_socket_reuseable(fd);
	}

	if (sa) {
		if (bind(fd, sa, socklen)<0) {
			EVUTIL_CLOSESOCKET(fd);
			return NULL;
		}
	}

	return evconnlistener_new(base, cb, ptr, flags, backlog, fd);
}

void
evconnlistener_free(struct evconnlistener *lev)
{
	lev->ops->destroy(lev);
	mm_free(lev);
}

static void
event_listener_destroy(struct evconnlistener *lev)
{
	struct evconnlistener_event *lev_e =
	    EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
	
	event_del(&lev_e->listener);
	if (lev->flags & LEV_OPT_CLOSE_ON_FREE)
		EVUTIL_CLOSESOCKET(event_get_fd(&lev_e->listener));
}

int
evconnlistener_enable(struct evconnlistener *lev)
{
	return lev->ops->enable(lev);
}

int
evconnlistener_disable(struct evconnlistener *lev)
{
	return lev->ops->disable(lev);
}

static int
event_listener_enable(struct evconnlistener *lev)
{
	struct evconnlistener_event *lev_e =
	    EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
	return event_add(&lev_e->listener, NULL);
}

static int
event_listener_disable(struct evconnlistener *lev)
{
	struct evconnlistener_event *lev_e =
	    EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
	return event_del(&lev_e->listener);
}

struct event_base *
evconnlistener_get_base(struct evconnlistener *lev)
{
	/* XXXX UPCAST. */
	struct evconnlistener_event *lev_e =
	    EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
	return event_get_base(&lev_e->listener);
}

evutil_socket_t
evconnlistener_get_fd(struct evconnlistener *lev)
{
	return lev->ops->getfd(lev);
}

static evutil_socket_t
event_listener_getfd(struct evconnlistener *lev)
{
	struct evconnlistener_event *lev_e =
	    EVUTIL_UPCAST(lev, struct evconnlistener_event, base);
	return event_get_fd(&lev_e->listener);
}

static void
listener_read_cb(evutil_socket_t fd, short what, void *p)
{
	struct evconnlistener *lev = p;
	int err;
	while (1) {
		struct sockaddr_storage ss;
		socklen_t socklen = sizeof(ss);

		evutil_socket_t new_fd = accept(fd, (struct sockaddr*)&ss, &socklen);
		if (new_fd < 0)
			break;

		if (!(lev->flags & LEV_OPT_LEAVE_SOCKETS_BLOCKING))
			evutil_make_socket_nonblocking(new_fd);

		lev->cb(lev, new_fd, (struct sockaddr*)&ss, (int)socklen,
		    lev->user_data);
	}
	err = evutil_socket_geterror(fd);
	if (EVUTIL_ERR_ACCEPT_RETRIABLE(err))
		return;
	event_sock_warn(fd, "Error from accept() call");
}

#ifdef WIN32
struct accepting_socket {
	struct event_overlapped overlapped;
	SOCKET s;
	struct evconnlistener_iocp *lev;
	int buflen;
	char addrbuf[1]; /*XXX */
};

static void accepted_socket_cb(struct event_overlapped *o, uintptr_t key,
    ev_ssize_t n);

static struct accepting_socket *
new_accepting_socket(struct evconnlistener_iocp *lev, int family)
{
	struct accepting_socket *res;
	int addrlen;
	int buflen;
	if (family == AF_INET)
		addrlen = sizeof(struct sockaddr_in);
	else if (family == AF_INET6)
		addrlen = sizeof(struct sockaddr_in6);
	else
		return NULL;
	buflen = (addrlen+16)*2;

	res = mm_calloc(1,sizeof(struct accepting_socket)-1+buflen);

	if (!res)
		return NULL;

	event_overlapped_init(&res->overlapped, accepted_socket_cb);
	res->s = INVALID_SOCKET;
	res->lev = lev;
	res->buflen = buflen;
	return res;
}

static int
start_accepting(struct accepting_socket *as)
{
	const struct win32_extension_fns *ext =
	    event_get_win32_extension_fns();
	int family = AF_INET; /* XXXX */
	SOCKET s = socket(family, SOCK_STREAM, 0);
	DWORD pending = 0;
	if (s == INVALID_SOCKET)
		return -1;

	setsockopt(s,
	    SOL_SOCKET,
	    SO_UPDATE_ACCEPT_CONTEXT,
	    (char *)&as->lev->fd,
	    sizeof(&as->lev->fd));

	if (!(as->lev->base.flags & LEV_OPT_LEAVE_SOCKETS_BLOCKING))
		evutil_make_socket_nonblocking(s);

	if (event_iocp_port_associate(as->lev->port, s, 1) < 0)
		return -1;

	as->s = s;

	if (ext->AcceptEx(as->lev->fd, s, as->addrbuf, 0,
		as->buflen/2, as->buflen/2,
		&pending, &as->overlapped.overlapped)) {
		/* Immediate success! */
		accepted_socket_cb(&as->overlapped, 1, 0);
		return 0;
	} else {
		int err = WSAGetLastError();
		if (err == ERROR_IO_PENDING)
			return 0;
		/* XXXX log the error */
		return -1;
	}
}

static void
accepted_socket_cb(struct event_overlapped *o, uintptr_t key, ev_ssize_t n)
{
	/* Run this whole thing deferred unless some MT flag is set */

	struct sockaddr *sa_local=NULL, *sa_remote=NULL;
	int socklen_local=0, socklen_remote=0;
	struct accepting_socket *as =
	    EVUTIL_UPCAST(o, struct accepting_socket, overlapped);
	const struct win32_extension_fns *ext =
	    event_get_win32_extension_fns();

	EVUTIL_ASSERT(ext->GetAcceptExSockaddrs);

	ext->GetAcceptExSockaddrs(as->addrbuf, 0,
	    as->buflen/2, as->buflen/2,
	    &sa_local, &socklen_local,
	    &sa_remote, &socklen_remote);

	as->s = INVALID_SOCKET;

	as->lev->base.cb(&as->lev->base, as->s, sa_remote, socklen_remote,
	    as->lev->base.user_data);

	/* Avoid stack overflow XXXX */
	start_accepting(as);
}


static int
iocp_listener_enable(struct evconnlistener *lev)
{
	/* XXXX */
	return 0;
}
static int
iocp_listener_disable(struct evconnlistener *lev)
{
	/* XXXX */
	return 0;
}
static void
iocp_listener_destroy(struct evconnlistener *lev)
{
	/* XXXX */
}
static evutil_socket_t
iocp_listener_getfd(struct evconnlistener *lev)
{
	struct evconnlistener_iocp *lev_iocp =
	    EVUTIL_UPCAST(lev, struct evconnlistener_iocp, base);	
	return lev_iocp->fd;
}

static const struct evconnlistener_ops evconnlistener_iocp_ops = {
	iocp_listener_enable,	
	iocp_listener_disable,	
	iocp_listener_destroy,	
	iocp_listener_getfd
};

struct evconnlistener *
evconnlistener_new_async(struct event_base *base,
    evconnlistener_cb cb, void *ptr, unsigned flags, int backlog,
    evutil_socket_t fd); /* XXXX Use or export this. */

struct evconnlistener *
evconnlistener_new_async(struct event_base *base,
    evconnlistener_cb cb, void *ptr, unsigned flags, int backlog,
    evutil_socket_t fd)
{
	struct sockaddr_storage ss;
	int socklen = sizeof(ss);
	struct evconnlistener_iocp *lev;
	/* XXXX duplicate code */
	if (backlog > 0) {
		if (listen(fd, backlog) < 0)
			return NULL;
	} else if (backlog < 0) {
		if (listen(fd, 128) < 0)
			return NULL;
	}
	if (getsockname(fd, (struct sockaddr*)&ss, &socklen)) {
		event_sock_warn(fd, "getsockname");
		return NULL;
	}
	lev = mm_calloc(1, sizeof(struct evconnlistener_event));
	if (!lev) {
		event_warn("calloc");
		return NULL;
	}
	lev->base.ops = &evconnlistener_iocp_ops;
	lev->base.cb = cb;
	lev->base.user_data = ptr;
	lev->base.flags = flags;

	lev->fd = fd;

	lev->n_accepting = 1;
	lev->accepting = mm_calloc(1, sizeof(struct accepting_socket *));
	if (!lev->accepting) {
		event_warn("calloc");
		mm_free(lev);
		closesocket(fd);
		return NULL;
	}
	lev->accepting[0] = new_accepting_socket(lev, ss.ss_family);
	if (!lev->accepting[0]) {
		event_warnx("Couldn't create accepting socket");
		mm_free(lev->accepting);
		mm_free(lev);
		closesocket(fd);
		return NULL;
	}

	if (!start_accepting(lev->accepting[0])) {
		event_warnx("Couldn't start accepting on socket");
		/* XXX free everything */
		return NULL;
	}

	return &lev->base;
}

#endif