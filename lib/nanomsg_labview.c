/*
 * NANOMSG_LABVIEW :: an interface between NANOMSG and LabVIEW
 * This helper DLL handles clean-up to prevent crashing and hanging
 * in LabVIEW because of NANOMSG's designed behaviour.
 * nanomsg_labview.c is based on zmq_labview.c created by Martijn Jasperse.
 *
 * Created by Chen Yucong, slaoub@gmail.com
 */

#define NANOMSG_VERSION_MAJOR	1
#define NANOMSG_VERSION_MINOR	0
#define NANOMSG_VERSION_PATCH	0

#define BONZAI_INLINE
#include "bonzai.h"

#ifdef _WIN32
/*
 * careful with windows headers
 * http://msdn.microsoft.com/en-us/library/aa383745.aspx
 */
#define WIN32_LEAN_AND_MEAN
#define NTDDI_VERSION		NTDDI_WINXPSP1
#define _WIN32_WINNT		_WIN32_WINNT_WINXP 
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <nanomsg/nn.h>
#include <extcode.h>

#define USE_SOCKET_MUTEX	0
#define ERROR_BASE		NN_HAUSNUMERO	/* base error number in LV */
#define ECRIT			1097

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#pragma comment(lib, "user32.lib")	/* required when linking to labview */
#pragma warning(disable: 4996)
/* critical section/mutex check */
volatile int CRITERR = 0;
#define CRITCHECK	do { if (CRITERR > 0) return -1097; } while (0)
#else
#define EXPORT
#define CRITCHECK
#endif

#include "debug.c"

/* to simplify error handling, return -errno on error */
#ifdef _WIN32
#define RET0(x)		((CRITERR > 0) ? -1097 : ((x >= 0) ? 0 : -nn_errno()))
#else
#define RET0(x)		((x >= 0) ? 0 : -nn_errno())
#endif

/* standard typedefs */
#ifndef _extcode_H
typedef char** UHandle;
#endif
typedef unsigned long u32;

void basic_free(void *data, void *hint) { free(data); }

#ifdef _WIN64			/* 64-bit windows */
#define LVALIGNMENT 8
#elif !defined(_WIN32)		/* 32-bit linux */
#define LVALIGNMENT 4
#else				/* 32-bit windows */
#define LVALIGNMENT 1
#endif

/* pointer alignment helper function */
#if LVALIGNMENT > 1
void* LVALIGN( void *x )
{
	return (void*)(((uintptr_t)x + LVALIGNMENT - 1) & (~LVALIGNMENT + 1));
}
#else
#define LVALIGN(x) (x)
#endif

#if USE_SOCKET_MUTEX
#ifdef _WIN32
typedef HANDLE mutex_t;

mutex_t create_mutex()
{
	mutex_t m = CreateMutex(NULL /* default security */,
				FALSE /* unowned */,
				NULL /* unnamed */);
	return m;
}

void destroy_mutex(mutex_t m)
{
	if (m)
		CloseHandle(m);
}

int acquire_mutex(mutex_t m)
{
	DWORD ret;

	if (!m)
		return -ECRIT;
	ret = WaitForSingleObject(m, INFINITE);

	/* error means mutex was destroyed */
	return (ret == WAIT_OBJECT_0) ? 0 : -ECRIT;
}

int release_mutex(mutex_t m)
{
	return ReleaseMutex(m) ? -ECRIT : 0;
}
#else
typedef pthread_mutex_t mutex_t;

mutex_t create_mutex()
{
	mutex_t m = 0;
	int ret = pthread_mutex_init(&m, NULL);
	return (ret == 0) ? m : 0;
}

void destroy_mutex(mutex_t m)
{
	if (m)
		pthread_mutex_destroy(&m);
}

int acquire_mutex(mutex_t m)
{
	return pthread_mutex_lock(&m); /* error means mutex was destroyed */
}

int release_mutex(mutex_t m)
{
	return pthread_mutex_unlock(&m);
}
#endif
#else
typedef int mutex_t;

mutex_t create_mutex()		{ return 0; }
void destroy_mutex(mutex_t m)	{ return; }
int acquire_mutex(mutex_t m)	{ return 0; }
int release_mutex(mutex_t m)	{ return 0; }
#endif 

typedef struct {
	uint32_t tag;
	uint32_t id;
	int flags;
	int ipv6;
} ctx_t;

typedef struct {
	void *ctx;
	bonzai *socks;
	bonzai *inst;
	int flags;
} ctx_obj;

typedef struct {
	int sock;
	ctx_obj *ctx;
	int flags;
	int eid;
	mutex_t mutex;
} sock_obj;

bonzai *allinst = NULL;
bonzai *validobj = NULL;

#define FLAG_BLOCKING	1
#define FLAG_INTERRUPT	2

#define CHECK_INTERNAL(x,y,z,m)						\
do {									\
	if (!(x) || !(y) || (bonzai_find(validobj,x) < 0)) {		\
		if (m) {						\
			DEBUGMSG("SANITY FAIL %s@%i -- %p (%p)",	\
					__FUNCTION__, __LINE__, y, x);	\
		}							\
		return -z;						\
	}								\
} while (0)
#define CHECK_SOCK(x)	CHECK_INTERNAL(x, ((x->sock) >= 0), ENOTSOCK, 1);
#define CHECK_CTX(x)	CHECK_INTERNAL(x, x->ctx, EINVAL, 1);

EXPORT int lvnanomsg_close(sock_obj *sockobj, int flags)
{
	int ret, i;
	int sock;
	ctx_obj *ctxobj;

	/* validate */
	CHECK_SOCK(sockobj);
	sock = sockobj->sock;
	ctxobj = sockobj->ctx;
	DEBUGMSG("CLOSE socket %d (%p)", sock, sockobj);

	if (flags) {
		const int linger = 0;
		/* set the linger time to zero */
		nn_setsockopt(sock, NN_SOL_SOCKET, NN_LINGER, &linger, sizeof(linger));
	}
	/* don't proceed if the socket is currently in use */
#if USE_SOCKET_MUTEX
	acquire_mutex(sockobj->mutex);
	destroy_mutex(sockobj->mutex); /* should cancel any waiting acquires */
	sockobj->mutex = 0;
#endif
	/* close the socket */
	ret = nn_close(sock);
	/* clean up */
	bonzai_clip(validobj, sockobj);
	free(sockobj);
	/* remove from context */
	i = bonzai_find(ctxobj->socks, sockobj);
	DEBUGMSG("  FOUND at pos %i of %p (%p)", i, ctxobj->ctx, ctxobj);
	if (i >= 0)
		ctxobj->socks->elem[i] = NULL;
	//if ( bonzai_clip( ctxobj->socks, sockobj ) < 0 ) ret = -EFAULT;

	return RET0(ret);
}

EXPORT int lvnanomsg_ctx_destroy(ctx_obj **pinstdata, ctx_obj *ctxobj, int flags)
{
	void *ctx;

	/* validate */
	CHECK_CTX(ctxobj);
	ctx = ctxobj->ctx;
	DEBUGMSG("TERM on %p (%p)", ctxobj->ctx, ctxobj);

	/* store a pointer in the instance in case of interrupt */
	if (pinstdata)
		*pinstdata = ctxobj;

	/* attempt to prevent hanging by closing all non-blocking sockets */
	if (flags) {
		sock_obj *sockobj = NULL;
		int i;
		bonzai *tree = ctxobj->socks;

		DEBUGMSG("  SUB %u sockets",tree->n);
		/* signify to sockets blocking to this context that it's being terminated */
		ctxobj->flags |= FLAG_INTERRUPT;
		for (i = 0; i < tree->n; ++i) {
			/* note we MUST NOT close blocking sockets; they are in use in another thread */
			if (((sockobj = tree->elem[i])) && !(sockobj->flags & FLAG_BLOCKING)) {
				DEBUGMSG("  TERMCLOSE %i = %d (%p)", i, sockobj->sock, sockobj);
				lvnanomsg_close(sockobj, 1);
			}
		}
	}

	/* close the context -- this may hang!! */
	free(ctx);
	/* we succeeded, clean up */
	bonzai_free(ctxobj->socks);		/* kill list of sockets */
	bonzai_clip(ctxobj->inst, ctxobj);	/* remove from owning instance */
	bonzai_clip(validobj, ctxobj);		/* remove from list of valid objects */
	bonzai_sort(validobj);			/* remove NULLs from list of objects */
	free(ctxobj);				/* free memory associated */
	DEBUGMSG("  TERM complete");

	return 0;
}

EXPORT int lvnanomsg_ctx_destroy_abort(ctx_obj **pinstdata)
{
	/* aborting a term call because some sockets are not closed */
	if (pinstdata && *pinstdata) {
		DEBUGMSG("INTERRUPT term on %p", pinstdata[0]->ctx);
		/* close all non-blocking sockets then close context */
		pinstdata[0]->flags |= FLAG_INTERRUPT;
		lvnanomsg_ctx_destroy(NULL, *pinstdata, 1);
	}

	return 0;
}

EXPORT int lvnanomsg_ctx_create_reserve(bonzai** pinstdata)
{
	/* allocate a tree to track sockets associated with THIS labview instance */
	static int ninits = 0;
	
	++ninits;
	*pinstdata = bonzai_init((void *)ninits);
	bonzai_grow(allinst, *pinstdata);
	DEBUGMSG("RESERVE call #%i to %p", ninits, *pinstdata);

	return 0;
}

EXPORT int lvnanomsg_ctx_create(bonzai** pinstdata, ctx_obj** ctxptr)
{
	/* create a context and data structures to track it */
	void *ctx;
	*ctxptr = NULL;
	CRITCHECK;

	/* create a new context */
	ctx = calloc(sizeof(ctx_t), 1);
	if ( !ctx )
		return -1; /* failed */

	/* success! */
	*ctxptr = calloc(sizeof(ctx_obj), 1);
	if (!(*ctxptr))
		return -1;

	ctxptr[0]->socks = bonzai_init(ctx);
	ctxptr[0]->inst = *pinstdata;
	ctxptr[0]->ctx = ctx;
	bonzai_grow(validobj, *ctxptr);		/* is a valid object */
	bonzai_grow(*pinstdata, *ctxptr);	/* belongs to an inst */
	DEBUGMSG("INIT context %p (%p); %i objs", ctx, *ctxptr, validobj->n);

	return 0;
}

EXPORT int lvnanomsg_ctx_create_unreserve(bonzai** pinstdata)
{
	/* kill all contexts associated with labview instance */
	int i;
	bonzai *insttree = *pinstdata;	/* contexts associated with this instance */
	ctx_obj *ctxobj;

	CRITCHECK;
	bonzai_clip(allinst, insttree);	/* stop tracking this inst */
	if (insttree == NULL)
		return 0;	/* nothing to do */
	DEBUGMSG("UNRESERVE instance %p -> %i items", insttree, insttree->n);
	for (i = 0; i < insttree->n; ++i) {
		if ((ctxobj = insttree->elem[i]))
			/* eliminate this context */
			lvnanomsg_ctx_destroy( NULL, ctxobj, 1);
	}
	/* free the data structure */
	bonzai_free(insttree);
	/* reset the instance pointer */
	DEBUGMSG("  UNRESERVE complete %p", insttree);
	*pinstdata = NULL;

	return 0;
}

EXPORT int lvnanomsg_socket(ctx_obj *ctxobj, sock_obj **sockptr, int type, int linger)
{
	int ret = 0;
	int sock;

	*sockptr = NULL;
	DEBUGMSG("CREATE socket from %p (%p), %u existing",
		 ctxobj->ctx, ctxobj, ctxobj->socks->n);
	/* validate */
	CHECK_CTX(ctxobj);

	/* avoid issue #574 (https://zeromq.jira.com/browse/LIBZMQ-574) */
	if (ctxobj->socks->n >= 512)
		return -EMFILE;
	/* try to create a socket */
	sock = nn_socket(AF_SP, type);
	if (sock < 0) {
		ret = sock;
		goto out;
	}

	/* success! */
	ret = nn_setsockopt(sock, NN_SOL_SOCKET, NN_LINGER, &linger, sizeof(int));
	/* track objects */
	sockptr[0] = calloc(sizeof(sock_obj), 1);
	sockptr[0]->ctx = ctxobj;
	sockptr[0]->sock = sock;
#if USE_SOCKET_MUTEX
	sockptr[0]->mutex = create_mutex();
#endif
	bonzai_grow(ctxobj->socks, *sockptr);
	bonzai_grow(validobj, *sockptr);
	DEBUGMSG("  SOCKET complete %d (%p); %i objs", sock, *sockptr, validobj->n);

out:
	return RET0(ret);
}

EXPORT int lvnanomsg_poll(bonzai **pinstdata, sock_obj **sockobjs, int *events,
			  int n, long timeout, unsigned int *nevents)
{
	int ret = 0, i;
	struct nn_pollfd *items;

	items = calloc(n, sizeof(struct nn_pollfd));
	if (nevents)
		*nevents = 0;
	if (pinstdata)
		*pinstdata = bonzai_init(NULL);

	for (i = 0 ; i < n; ++i) {
		CHECK_SOCK(sockobjs[i]);	/* may leak mem if breaks */
		items[i].fd = sockobjs[i]->sock;
		items[i].events = events[i];
		items[i].revents = 0;
		if (timeout != 0) {
			bonzai_grow(*pinstdata, sockobjs[i]);
			sockobjs[i]->flags |= FLAG_BLOCKING;	/* a blocking call */
		}
	}

	ret = nn_poll(items, n, timeout);
	if ((ret < 0) && (nn_errno() == ETERM)) {
		ret = -ETERM;
		DEBUGMSG("POLL ETERM");
	}

	if (pinstdata) {
		bonzai_free(*pinstdata);
		*pinstdata = NULL;
	}

	for (i = 0 ; i < n; ++i) {
		events[i] = items[i].revents;
		if (nevents && events[i])
			++*nevents;
		sockobjs[i]->flags &= ~FLAG_BLOCKING;	/* no longer blocking */
		/* did the owning context get terminated? */
		if ((ret == -ETERM) && (sockobjs[i]->ctx->flags & FLAG_INTERRUPT)) {
			DEBUGMSG("  POLL CLOSE %d (%p)",
						sockobjs[i]->sock, sockobjs[i]);
			lvnanomsg_close(sockobjs[i], 1);
		}
	}

	free(items);
	DEBUGMSG("  POLL ret %d", ret);

	return ret;
}

EXPORT int lvnanomsg_poll_abort(bonzai **pinstdata)
{
	bonzai *tree;
	ctx_obj *ctxobj;
	int i;

	/* aborting a poll call */
	if (!pinstdata || !*pinstdata)
		return 0;
	tree = *pinstdata;
	DEBUGMSG("INTERRUPT POLL, %i items", tree->n);

	/* terminate each context, provided it's not already terminating */
	for (i = 0; i < tree->n; ++i) {
		if (((ctxobj = ((sock_obj*)tree->elem[i])->ctx))
		    && !(ctxobj->flags & FLAG_INTERRUPT)) {
			DEBUGMSG("  POLLINT kill %p (%p)", ctxobj->ctx, ctxobj);
			lvnanomsg_close((sock_obj*)tree->elem[i], 1);
		}
	}

	return 0;
}


EXPORT int lvnanomsg_recvmsg(sock_obj **pinstdata, sock_obj *sockobj,
			     char **h, const int lenvec[], const int size, int *flags)
{
	int ret = 0, n, i;
	struct nn_msghdr hdr;
	struct nn_iovec *iovec = NULL;
	UHandle ptr;
	bonzai *list;

	CRITCHECK;
	list = bonzai_init(NULL);

	iovec = (struct nn_iovec *)malloc(sizeof(struct nn_iovec) * size);
	if (!iovec)
		return -ENOMEM;

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iovec;
	hdr.msg_iovlen = size;


	/* clear input handle */
	if (acquire_mutex(sockobj->mutex) != 0)
		return -ECRIT;

	for (i = 0; i < size; i++) {
		/* get the next message part */
		ptr = DSNewHClr(4);
		DSSetHSzClr(ptr, 4);
		if (!ptr) {
			/* shit, out of memory */
			ret = -ENOBUFS;
			break;
		}

		DSSetHandleSize(ptr, lenvec[i] + 4);
		iovec->iov_base = *ptr + 4;
		iovec->iov_len = *(u32*)*ptr = lenvec[i];
		iovec++;

		/* add it to the stack */
		bonzai_grow(list, (void*)ptr);
	}

	ret = nn_recvmsg(sockobj->sock, &hdr, flags ? *flags : 0);

	release_mutex(sockobj->mutex);

	/* turn stack into compatible array of handles */
	n = list->n;
	DSSetHandleSize(h, 8 + n * sizeof(void*));
	memcpy(LVALIGN(*h + 4), list->elem, n * sizeof(void*));
	*(u32*)*h = n;
	bonzai_free(list);
	free(hdr.msg_iov);

	return RET0(ret);
}

EXPORT int lvnanomsg_recv(sock_obj **pinstdata, sock_obj *sockobj,
			  UHandle h, int *flags)
{
	int ret = 0;
	int sock;
	void *msg = NULL;

	DSSetHSzClr(h, 4); /* clear the output handle */
	CHECK_SOCK(sockobj);
	/* is this already blocking? */
	if (sockobj->flags & FLAG_BLOCKING)
		return -EINPROGRESS;
	sock = sockobj->sock;
	/* prepare for blocking call */
	if (pinstdata)
		*pinstdata = sockobj;

	if (acquire_mutex(sockobj->mutex) != 0)
		return -ECRIT;
	sockobj->flags |= FLAG_BLOCKING;
	DEBUGMSG("RECV on %d into %p", sockobj->sock, *h);
	ret = nn_recv(sock, &msg, NN_MSG, flags ? *flags : 0);
	DEBUGMSG("  RECV ret %d", ret);
	sockobj->flags &= ~FLAG_BLOCKING;
	release_mutex(sockobj->mutex);

	/* was the call terminated? */
	if ((ret < 0) && (nn_errno() == ETERM)) {
		DEBUGMSG("  TERM during RECV on %d", sockobj->sock);
		/* if it was an interrupt, we MUST close */
		if (sockobj->ctx->flags & FLAG_INTERRUPT)
			lvnanomsg_close(sockobj, 1);
	}
	if (pinstdata)
		*pinstdata = NULL;

	/* was it success? */
	if (ret >= 0) {
		int l = ret;	
		DSSetHandleSize(h, l + 4);
		*( u32* )*h = l;
		memcpy(*h + 4, msg, l);
		nn_freemsg(msg);
	}

	return RET0(ret);
}

EXPORT int lvnanomsg_recv_timeout(sock_obj **pinstdata, sock_obj *sockobj,
				  UHandle h, int *flags, long timeout)
{
	int ret = 0, evt = NN_POLLIN;	/* poll for input */
	bonzai *polltree = NULL;

	CHECK_SOCK(sockobj);
	ret = lvnanomsg_poll(&polltree, &sockobj, &evt, 1, timeout, NULL);
	if (ret < 0)
		return RET0(ret);	/* failed */
	else if (ret == 0)
		return -EAGAIN;		/* timed out */

	else 
		return lvnanomsg_recv(pinstdata, sockobj, h, flags);
}


EXPORT int lvnanomsg_recv_abort(sock_obj** pinstdata)
{
	/*
	 * ABORT semantics are somewhat confusing with NANOMSG:
 	 * 1. we want to stop this socket blocking, so we need to use nn_term()
	 * 2. we need to stop nn_term() blocking since we called it from the GUI thread (THIS thread)
	 * 3. therefore we need to close all sockets associated with the context
	 * 4. we cannot close THIS socket because it's still being used in the OTHER thread (by the blocking call)
	 * 5. therefore we close all sockets but the blocking one and use the blocking thread to close that socket.
	 * GOT ALL THAT??
	 */
	sock_obj *sockobj = *pinstdata;
	/* only worry about blocking calls */
	if (!sockobj || !(sockobj->flags & FLAG_BLOCKING))
		return 0;
	
	*pinstdata = NULL;
	/* we need to term the context to get the blocking call to return */
	DEBUGMSG("INTERRUPT send/recv on %p", sockobj->sock);
	/* we rely on the function unblocking and succeeding */
	lvnanomsg_ctx_destroy(NULL, sockobj->ctx, 1);
	DEBUGMSG("  INTERRUPT done");

	return 0;
}

EXPORT int lvnanomsg_recv_multi(sock_obj **pinstdata, sock_obj *sockobj,
				char** h, int *flags)
{
	int ret = 0, n;
	UHandle ptr;
	bonzai *list;
	CRITCHECK;
	list = bonzai_init(NULL);

	/* clear input handle */
	if (acquire_mutex(sockobj->mutex) != 0)
		return -ECRIT;
	do {
		/* get the next message part */
		ptr = DSNewHClr( 4 );
		if (!ptr) {
			/* shit, out of memory */
			ret = -ENOBUFS;
			break;
		}

		ret = lvnanomsg_recv(pinstdata, sockobj, ptr, flags);

		/* add it to the stack */
		if (ret >= 0)
			bonzai_grow(list, (void*)ptr);

		if (ret < 0)
			break;;
	} while (1);
	release_mutex(sockobj->mutex);

	/* turn stack into compatible array of handles */
	n = list->n;
	DSSetHandleSize(h, 8 + n * sizeof(void*));
	memcpy(LVALIGN(*h + 4), list->elem, n * sizeof(void*));
	*(u32*)*h = n;
	bonzai_free(list);

	return ret;
}

EXPORT int lvnanomsg_recv_multi_timeout(sock_obj **pinstdata, sock_obj *sockobj,
					char** h, int *flags, long timeout)
{
	int ret = 0, evt = NN_POLLIN;	/* poll for input */
	bonzai *polltree = NULL;
	CHECK_SOCK(sockobj);
	DSSetHSzClr(h, 4);		/* in case it fails */

	ret = lvnanomsg_poll(&polltree, &sockobj, &evt, 1, timeout, NULL);
	if (ret < 0)
		return RET0( ret );	/* failed */
	if (ret == 0)
		return -EAGAIN;		/* timed out */

	return lvnanomsg_recv_multi(pinstdata, sockobj, h, flags);
}

EXPORT int lvnanomsg_sendmsg(sock_obj *sockobj, char** h, int *flags)
{
	struct nn_msghdr hdr;
	struct nn_iovec *iovec = NULL;
	char ***ptr = (char***)LVALIGN(*h + 4);
	int ret = 0, n;
	int size = *(u32*)*h;
	
	CHECK_SOCK(sockobj);

	iovec = (struct nn_iovec *)malloc(sizeof(struct nn_iovec) * size);
	if (!iovec)
		return -ENOMEM;
	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iovec;
	hdr.msg_iovlen = size;

	if (acquire_mutex(sockobj->mutex) != 0)
		return -ECRIT;
	for (n = size; n > 0; ++ptr, --n) {
		UHandle htmp = (UHandle)*ptr;
		if (h) {
			const int l = *(u32*)*htmp;
			void *msg = nn_allocmsg(l, 0);

			if (msg == NULL)
				return  -ENOBUFS;
			
			memcpy(msg, *htmp + 4, l);
			iovec->iov_base = msg;
			iovec->iov_len = l;
		}
		iovec++;
	}
	release_mutex(sockobj->mutex);

	ret = nn_sendmsg(sockobj->sock, &hdr, flags ? *flags : 0);
	free(hdr.msg_iov);

	return RET0(ret);
}

EXPORT int lvnanomsg_send(sock_obj *sockobj, const UHandle h, int *flags)
{
	int ret = 0;
	void *msg;

	CHECK_SOCK(sockobj);

	if (acquire_mutex(sockobj->mutex) != 0)
		return -ECRIT;
	if (h) {
		const int l = *(u32*)*h;
		msg = nn_allocmsg(l, 0);
		if (msg == NULL) {
			/* oh shit we're out of memory */
			return -ENOBUFS;
		}
		memcpy(msg, *h + 4, l);
	}
	ret = nn_send(sockobj->sock, &msg, NN_MSG, flags ? *flags : 0);
	release_mutex(sockobj->mutex);
	if (flags)
		*flags = 0; /* unused */

	return RET0(ret);
}

EXPORT int lvnanomsg_send_multi(sock_obj *sockobj, char** h, int *flags)
{
	int ret = 0, n;
	char ***ptr = (char***)LVALIGN(*h + 4);
	CHECK_SOCK(sockobj);

	for (n = *(u32*)*h; n > 0; ++ptr, --n) {
		/* mutex locking is inside here */
		ret = lvnanomsg_send(sockobj, (UHandle)*ptr, flags);
		if (ret < 0)
			break;
	}

	return ret;
}

EXPORT int lvnanomsg_device(sock_obj *sockobj1, sock_obj *sockobj2)
{
	int ret;
	int s1 = sockobj1->sock;
	int s2 = sockobj2->sock;

	ret = nn_device(s1, s2);

	return RET0(ret);
}

EXPORT int lvnanomsg_get_monitor_event(sock_obj** pinstdata, sock_obj *s,
					int *intval, UHandle strval)
{
	UHandle buffer;
	int ret, evtnum;
	int id = 0, flags = 0;
	uint32_t len;

	/* we use a fake buffer so we can use lvnanomsg_recv and have protected abort semantics */
	buffer = DSNewHClr(4);
	DEBUGMSG("MONITOR %p blocking for recv", s->sock);
	ret = lvnanomsg_recv(pinstdata, s, buffer, &flags);
	DEBUGMSG( "  MONITOR got ret %d", ret );
	/* if it failed, give up */
	if ( ret < 0 )
		return ret;

	/* make sure it went as expected (ensure multi-part message, check payload size) */
	len = **(uint32_t**)buffer;
	if (!(flags & 1) || (len != 6)) {
		DEBUGMSG("  UNEXPECTED MON package flag %i got %u bytes", flags, len);
		DSDisposeHandle(buffer);
		return -ECRIT;
	}

	/* reinterpret the payload */
	evtnum = *(uint16_t*)(*(char**)buffer + 4);	/* first 2 bytes are event type */
	*intval = *(int32_t*)(*(char**)buffer + 6);	/* next 4 bytes are int value */
	/* get the event type */
	ret = evtnum;
	while ( ret >>= 1 ) ++id;
	DSDisposeHandle(buffer);

	/* second frame is the address */
	flags = 0;
	ret = lvnanomsg_recv(pinstdata, s, strval, &flags);
	/* pop a debug message */
	DEBUGMSG("  SOCKET monitor got message, %i (%i), %s", evtnum, id, evt->data.connected.addr);

	/* return the event type */
	return id + 1;
}

typedef struct {
	sock_obj *sockobj;
	LVUserEventRef *event;
} ReceiverData;

#ifdef _WIN32
DWORD WINAPI lvnanomsg_receiver_thread(LPVOID param)
#else
void* lvnanomsg_receiver_thread(void* param)
#endif
{
	int ret, err, flags = 0;
	ReceiverData *data = (ReceiverData*)param;
	struct nn_pollfd poller;

	poller.fd = data->sockobj->sock;
	poller.events = NN_POLLIN;
	
	while ((ret = nn_poll(&poller, 1, -1)) > 0) {
		if (poller.revents & NN_POLLIN) {
			/* call RECV_MULTI to get all messages */
			UHandle buffer = DSNewHClr(4);
			// data->sockobj->flags &= ~FLAG_BLOCKING;	/* unmask as blocking */
			ret = lvnanomsg_recv(NULL, data->sockobj, buffer, &flags);
			// data->sockobj->flags |= FLAG_BLOCKING;	/* mask as blocking */
			DEBUGMSG("  Poller recv ret %i", ret);
			if (ret < 0)
				continue; // ? TODO
			/* post as LV event */
			err = PostLVUserEvent(*data->event, &buffer);
			DEBUGMSG("  Poller post event ret %i", err);
		}
	}

	return 0;
}

EXPORT int lvnanomsg_start_receiver(LVUserEventRef *evt, sock_obj *sockobj)
{
#ifdef _WIN32
	HANDLE thread;
#else
	pthread_t thread;
#endif

	ReceiverData *data = calloc(sizeof(ReceiverData), 1);
	data->event = evt;
	data->sockobj = sockobj;

#ifdef _WIN32
	thread = CreateThread(NULL, 0, lvnanomsg_receiver_thread, data, 0, NULL);
	/* did the function call succeed? */
	if (thread == NULL)
		return -CRITERR;
	/* is the thread currently running? */
	if (WaitForSingleObject(thread,0) != WAIT_TIMEOUT)
		return -EINVAL;
	/* TerminateThread */
#else
	int tid = pthread_create(&thread, NULL, lvnanomsg_receiver_thread, data);
	if (tid != 0)
		return -EINVAL;
	/* pthread_cancel with pthread_cleanup_push */
#endif

	/* success */
	return 0;
}



#ifdef _WIN32
char ERRPATH[MAX_PATH] = ".";

LONG WINAPI intercept_exception( PEXCEPTION_POINTERS ExceptionInfo )
{
	PEXCEPTION_RECORD rec = ExceptionInfo->ExceptionRecord;
	// are we reasonably certain this exception came from zmq?
	// (magic exception code number comes from libzmq/src/err.cpp)
	DEBUGMSG("ASSERT FAIL, %x (%i)", rec->ExceptionCode, rec->NumberParameters);
	if (rec->ExceptionCode == 0x40000015 && rec->NumberParameters == 1)
	{
		InterlockedIncrement( &CRITERR ); // critical error has occurred; let other functions know
		if ( CRITERR == 1 )
		{
			char buffer[512];
			fflush(stderr);
			_snprintf(buffer, sizeof(buffer),
				"NanoMSG has encountered a critical error and has aborted.\n\n"
				"Please post the contents of the log file\n    %s\nand a "
				"description of your code to the project forum at URL.\n\n"
				"Any further calls will fail immediately; please save your work "
				"and restart LabVIEW.\n\nERROR: Assertion failure (%s)",
				ERRPATH, (LPCSTR)rec->ExceptionInformation[0]);
			MessageBox(NULL, buffer, "Nanomsg Abort", MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_TOPMOST);
		}
		// force code execution to continue --
		// this is potentially an extremely bad idea, since NANOMSG is in an undefined state
		// assume the worst that can happen is a segfault, which LabVIEW can handle
		// this is better than an abort, which kills the LabVIEW environment completely
		rec->ExceptionFlags = 0; // force continue
		//LeaveCriticalSection( &CRITSEC );
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	return EXCEPTION_EXECUTE_HANDLER;
}
#else
/*
 * GCC enables definition of constructor/destructor via attributes
 * ---
 * based on http://tdistler.com/2007/10/05/implementing-dllmain-in-a-linux-shared-library
 */
void __attribute__ ((constructor)) lvnanomsg_loadlib(void);
void __attribute__ ((destructor)) lvnanomsg_unloadlib(void);
#endif

void lvnanomsg_loadlib()
{
	DEBUGMSG("ATTACH library");
	allinst = bonzai_init(NULL);
	validobj = bonzai_init(NULL);
}

void lvnanomsg_unloadlib()
{
	DEBUGMSG("DETACH library");
	bonzai_free(allinst);
	bonzai_free(validobj);
}

#ifdef _WIN32
FILE *oldstderr = NULL;
	
// Constructor/destructor on Win32 implemented via DllMain
// ---
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	switch (fdwReason)
	{
		case DLL_PROCESS_ATTACH:
			CRITERR = 0;
			// Add an exception handler for libzmq assert fails
			SetUnhandledExceptionFilter(intercept_exception);
			// Create a temporary log file
			GetTempPath(MAX_PATH-16, ERRPATH);
			strcat(ERRPATH, "lvnanomsg-err.log");
			// Rebind STDERR to this file
			oldstderr = freopen(ERRPATH, "w", stderr);
			// Normal initialisation
			lvnanomsg_loadlib();
			break;
		case DLL_PROCESS_DETACH:
			// Cleanup
			lvnanomsg_unloadlib();
			break;
	}
	return TRUE;
}
#elif !defined(__GNUC__)
#error Unsupported compiler, check implementation of constructor/destructor
#endif

EXPORT int lvnanomsg_errcode(int err)
{
	switch (err) {
		/*
		 * compatibility codes in nanomsg/nn.h
		 *
		 * note that the values associated with these err names
		 * are not consistent between platforms, which is why
		 * translation is required
		 */
		case ENOTSUP:		return ERROR_BASE + 1;
		case EPROTONOSUPPORT:	return ERROR_BASE + 2;
		case ENOBUFS:		return ERROR_BASE + 3;
		case ENETDOWN:		return ERROR_BASE + 4;
		case EADDRINUSE:	return ERROR_BASE + 5;
		case EADDRNOTAVAIL:	return ERROR_BASE + 6;
		case ECONNREFUSED:	return ERROR_BASE + 7;
		case EINPROGRESS:	return ERROR_BASE + 8;
		case ENOTSOCK:		return ERROR_BASE + 9;
		case EAFNOSUPPORT:	return ERROR_BASE + 10;
		case EPROTO:		return ERROR_BASE + 11;
		case EAGAIN:		return ERROR_BASE + 12;
		case EBADF:		return ERROR_BASE + 13;
		case EINVAL:		return ERROR_BASE + 14;
		case EMFILE:		return ERROR_BASE + 15;
		case EFAULT:		return ERROR_BASE + 16;
		case EACCES:		return ERROR_BASE + 17;
		case ENETRESET:		return ERROR_BASE + 18;
		case ENETUNREACH:	return ERROR_BASE + 19;
		case EHOSTUNREACH:	return ERROR_BASE + 20;
		case ENOTCONN:		return ERROR_BASE + 21;
		case EMSGSIZE:		return ERROR_BASE + 22;
		case ETIMEDOUT:		return ERROR_BASE + 23;
		case ECONNABORTED:	return ERROR_BASE + 24;
		case ECONNRESET:	return ERROR_BASE + 25;
		case ENOPROTOOPT:	return ERROR_BASE + 26;
		case EISCONN:		return ERROR_BASE + 27;
		case ESOCKTNOSUPPORT:	return ERROR_BASE + 28;	/* custom err code */
		/* map errno.h to labview numbers */
		case EBUSY:		return ERROR_BASE + 40;
		case ENODEV:		return ERROR_BASE + 41;
		case EINTR:		return ERROR_BASE + 42;
		case ENOENT:		return ERROR_BASE + 43;
		case ENOMEM:		return ERROR_BASE + 44;
		/* native nanomsg error codes */
		case ETERM: 		return ERROR_BASE + 53;
		case EFSM: 		return ERROR_BASE + 54;
		/* labview codes */
		case ECRIT:		return ECRIT;		/* library error/segfault */
		/* unknown code */
		default:		return ERROR_BASE + 0;
	}
}


void nn_version(int *_major, int *_minor, int *_patch)
{
	*_major = NANOMSG_VERSION_MAJOR;
	*_minor = NANOMSG_VERSION_MINOR;
	*_patch = NANOMSG_VERSION_PATCH;
}

EXPORT void lvnanomsg_version(int *major, int *minor, int *patch)
{
	nn_version(major, minor, patch);	
}

/* DIRECT WRAPPERS */

EXPORT int lvnanomsg_setsockopt(sock_obj *s, int level, int opt,
				const void *val, size_t len)
{
	int ret;

	CHECK_SOCK(s);
	if (acquire_mutex(s->mutex) != 0) 
		return -ECRIT;
	ret = nn_setsockopt(s->sock, level, opt, val, len);
	release_mutex(s->mutex);

	return RET0(ret);
}

EXPORT int lvnanomsg_getsockopt(sock_obj *s, int level, int opt,
				void *val, size_t *len)
{
	int ret;

	CHECK_SOCK(s);
	if (acquire_mutex(s->mutex) != 0)
		return -ECRIT;
	ret = nn_getsockopt(s->sock, level, opt, val, len);
	release_mutex(s->mutex);

	return RET0(ret);
}

EXPORT int lvnanomsg_bind(sock_obj *s, const char *addr)
{
	int ret;

	CHECK_SOCK(s);
	if (acquire_mutex(s->mutex) != 0)
		return -ECRIT;
	DEBUGMSG("BINDing %d to %s", s->sock, addr);
	ret = nn_bind(s->sock, addr);
	s->eid = ret;
	release_mutex(s->mutex);

	return RET0(ret);
}

EXPORT int lvnanomsg_connect(sock_obj *s, const char *addr)
{
	int ret;

	CHECK_SOCK(s);
	if (acquire_mutex(s->mutex) != 0)
		return -ECRIT;
	ret = nn_connect(s->sock, addr);
	s->eid = ret;
	release_mutex(s->mutex);

	return RET0(ret);
}

EXPORT int lvnanomsg_shutdown(sock_obj *s)
{
	int ret;

	CHECK_SOCK(s);
	if (acquire_mutex(s->mutex) != 0)
		return -ECRIT;
	ret = nn_shutdown(s->sock, s->eid);
	release_mutex(s->mutex);

	return RET0(ret);
}

EXPORT uint64_t lvnanomsg_get_statistic(sock_obj *s, int statistic, uint64_t *result)
{
	int ret = 0;
	uint64_t val;

	CHECK_SOCK(s);
	if (result)
		*result = 0;

	if (acquire_mutex(s->mutex) != 0)
		return -ECRIT;
	val = nn_get_statistic(s->sock, statistic);
	release_mutex(s->mutex);

	if (result)
		*result = val;
	if (val == ((uint64_t)-1))
		ret = -1;
		
	return RET0(ret);
}

EXPORT int lvnanomsg_ctx_check(ctx_obj *x)
{
	CHECK_INTERNAL(x, x->ctx, EINVAL, 0);
	return 0;
}

EXPORT int lvnanomsg_sock_check(sock_obj *x)
{
	CHECK_INTERNAL(x, x->sock, ENOTSOCK, 0);
	return 0;
}
