/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <sys/syscall.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sys/param.h>
#ifdef HAVE_SYS_UIO_H /* 'struct iovec' for OpenBSD */
#include <sys/uio.h>
#endif /* HAVE_SYS_UIO_H */
#ifdef HAVE_CAP_NG_H
#include <cap-ng.h>
#endif /* HAVE_CAP_NG_H */

#include "common/sockaddr.h"
#include "common/mempattern.h"
#include "common/mempool.h"
#include "knot/knot.h"
#include "knot/server/udp-handler.h"
#include "libknot/nameserver/name-server.h"
#include "knot/stat/stat.h"
#include "knot/server/server.h"
#include "libknot/packet/wire.h"
#include "libknot/consts.h"
#include "libknot/packet/pkt.h"
#include "knot/server/zones.h"
#include "knot/server/notify.h"
#include "libknot/dnssec/cleanup.h"

/* Buffer identifiers. */
enum {
	RX = 0,
	TX = 1,
	NBUFS = 2
};

/* FD_COPY macro compat. */
#ifndef FD_COPY
#define FD_COPY(src, dest) memcpy((dest), (src), sizeof(fd_set))
#endif

/* Mirror mode (no answering). */
/* #define MIRROR_MODE 1 */

/* PPS measurement. */
/* #define MEASURE_PPS 1 */

/* PPS measurement */
#ifdef MEASURE_PPS

/* Not thread-safe, used only for RX thread. */
static struct timeval __pps_t0, __pps_t1;
volatile static unsigned __pps_rx = 0;
static inline void udp_pps_begin()
{
	gettimeofday(&__pps_t0, NULL);
}

static inline void udp_pps_sample(unsigned n, unsigned thr_id)
{
	__pps_rx += n;
	if (thr_id == 0) {
		gettimeofday(&__pps_t1, NULL);
		if (time_diff(&__pps_t0, &__pps_t1) >= 1000.0) {
			unsigned pps = __pps_rx;
			memcpy(&__pps_t0, &__pps_t1, sizeof(struct timeval));
			__pps_rx = 0;
			log_server_info("RX rate %u p/s.\n", pps);
		}
	}
}
#else
static inline void udp_pps_begin() {}
static inline void udp_pps_sample(unsigned n, unsigned thr_id) {}
#endif


/* Answering context. */
struct answer_ctx
{
	server_t *srv;
	mm_ctx_t *mm;
	unsigned slip;
};

/*! \brief RRL reject procedure. */
static size_t udp_rrl_reject(const knot_nameserver_t *ns,
                             const knot_pkt_t *packet,
                             uint8_t* resp, size_t rlen,
                             uint8_t rcode, unsigned *slip)
{
	int n_slip = conf()->rrl_slip; /* Check SLIP. */
	if (n_slip > 0 && n_slip == ++*slip) {
		knot_ns_error_response_from_query(ns, packet, rcode, resp, &rlen);
		switch(rcode) { /* Do not set TC=1 to some RCODEs. */
		case KNOT_RCODE_FORMERR:
		case KNOT_RCODE_REFUSED:
		case KNOT_RCODE_SERVFAIL:
		case KNOT_RCODE_NOTIMPL:
			break;
		default:
			knot_wire_set_tc(resp); /* Set TC=1 */
			break;
		}

		*slip = 0; /* Restart SLIP interval. */
		return rlen;
	}

	return 0; /* Discard response. */
}

int udp_handle(struct answer_ctx *ans, int fd, sockaddr_t *addr,
               struct iovec *rx, struct iovec *tx)
{
#ifdef DEBUG_ENABLE_BRIEF
	char strfrom[SOCKADDR_STRLEN];
	memset(strfrom, 0, sizeof(strfrom));
	sockaddr_tostr(addr, strfrom, sizeof(strfrom));
	dbg_net("udp: received %zd bytes from '%s@%d'.\n", rx->iov_len,
	        strfrom, sockaddr_portnum(addr));
#endif

	int res = KNOT_EOK;
	int rcode = KNOT_RCODE_NOERROR;
	knot_nameserver_t *ns = ans->srv->nameserver;
	rrl_table_t *rrl = ans->srv->rrl;
	knot_packet_type_t qtype = KNOT_QUERY_INVALID;

	/* The packet MUST contain at least DNS header.
	 * If it doesn't, it's not a DNS packet and we should discard it.
	 */
	if (rx->iov_len < KNOT_WIRE_HEADER_SIZE) {
		return KNOT_EFEWDATA;
	}

#ifdef MIRROR_MODE
	memcpy(tx->iov_base, rx->iov_base, rx->iov_len);
	knot_wire_set_qr(tx->iov_base);
	tx->iov_len = rx->iov_len;
	return KNOT_EOK;
#endif

	knot_pkt_t *query = knot_pkt_new(rx->iov_base, rx->iov_len, ans->mm);
	if (query == NULL) {
		dbg_net("udp: failed to create packet\n");
		int ret = knot_ns_error_response_from_query_wire(ns, rx->iov_base, rx->iov_len,
		                                            KNOT_RCODE_SERVFAIL,
		                                            tx->iov_base, &tx->iov_len);
		return ret;
	}

	/* Parse query. */
	rcode = knot_ns_parse_packet(query, &qtype);
	if (rcode < KNOT_RCODE_NOERROR) {
		dbg_net("udp: failed to parse packet\n");
		rcode = KNOT_RCODE_SERVFAIL;
	}

	/* Handle query. */
	switch(qtype) {
	case KNOT_QUERY_NORMAL:
		res = zones_normal_query_answer(ns, query, addr, tx->iov_base,
		                                &tx->iov_len, NS_TRANSPORT_UDP);
		break;
	case KNOT_QUERY_AXFR:
		/* RFC1034, p.28 requires reliable transfer protocol.
		 * Bind responds with FORMERR.
		 */
		res = knot_ns_error_response_from_query(ns, query,
		                                        KNOT_RCODE_FORMERR, tx->iov_base,
		                                        &tx->iov_len);
		break;
	case KNOT_QUERY_IXFR:
		/* According to RFC1035, respond with SOA. */
		res = zones_normal_query_answer(ns, query, addr,
		                                tx->iov_base, &tx->iov_len,
		                                NS_TRANSPORT_UDP);
		break;
	case KNOT_QUERY_NOTIFY:
		res = notify_process_request(ns, query, addr,
		                             tx->iov_base, &tx->iov_len);
		break;

	case KNOT_QUERY_UPDATE:
		res = zones_process_update(ns, query, addr, tx->iov_base, &tx->iov_len,
		                           fd, NS_TRANSPORT_UDP);
		break;

	/* Do not issue response to incoming response to avoid loops. */
	case KNOT_RESPONSE_AXFR: /*!< Processed in XFR handler. */
	case KNOT_RESPONSE_IXFR: /*!< Processed in XFR handler. */
	case KNOT_RESPONSE_NORMAL:
	case KNOT_RESPONSE_NOTIFY:
	case KNOT_RESPONSE_UPDATE:
		res = KNOT_EOK;
		tx->iov_len = 0;
		break;
	/* Unknown opcodes */
	default:
		res = knot_ns_error_response_from_query(ns, query,
		                                        rcode, tx->iov_base,
		                                        &tx->iov_len);
		break;
	}

	/* Process RRL. */
	if (knot_unlikely(rrl != NULL) && rrl->rate > 0) {
		rrl_req_t rrl_rq;
		memset(&rrl_rq, 0, sizeof(rrl_req_t));
		rrl_rq.w = tx->iov_base; /* Wire */
		rrl_rq.query = query;

		rcu_read_lock();
		rrl_rq.flags = query->flags;
		if (rrl_query(rrl, addr, &rrl_rq, query->zone) != KNOT_EOK) {
			tx->iov_len = udp_rrl_reject(ns, query, tx->iov_base,
			                           	KNOT_WIRE_MAX_PKTSIZE,
			                           	knot_wire_get_rcode(query->wire),
			                           	&ans->slip);
		}
		rcu_read_unlock();
	}


	knot_pkt_free(&query);

	return res;
}

/* Check for sendmmsg syscall. */
#ifdef HAVE_SENDMMSG
  #define ENABLE_SENDMMSG 1
#else
  #ifdef SYS_sendmmsg
    #define ENABLE_SENDMMSG 1
  #endif
#endif

/*! \brief Pointer to selected UDP master implementation. */
static void* (*_udp_init)(void) = 0;
static int (*_udp_deinit)(void *) = 0;
static int (*_udp_recv)(int, void *) = 0;
static int (*_udp_handle)(struct answer_ctx *, void *) = 0;
static int (*_udp_send)(void *) = 0;

/* UDP recvfrom() request struct. */
struct udp_recvfrom {
	int fd;
	sockaddr_t addr;
	struct msghdr msg[NBUFS];
	struct iovec iov[NBUFS];
	uint8_t buf[NBUFS][KNOT_WIRE_MAX_PKTSIZE];
};

static void *udp_recvfrom_init(void)
{
	struct udp_recvfrom *rq = malloc(sizeof(struct udp_recvfrom));
	if (rq == NULL) {
		return NULL;
	}

	memset(rq->msg, 0, NBUFS * sizeof(struct msghdr));
	memset(rq->iov, 0, NBUFS * sizeof(struct iovec));
	sockaddr_prep(&rq->addr);
	for (unsigned i = 0; i < NBUFS; ++i) {
		rq->iov[i].iov_base = rq->buf + i;
		rq->iov[i].iov_len = KNOT_WIRE_MAX_PKTSIZE;
		rq->msg[i].msg_name = &rq->addr;
		rq->msg[i].msg_namelen = rq->addr.len;
		rq->msg[i].msg_iov = &rq->iov[i];
		rq->msg[i].msg_iovlen = 1;
		rq->msg[i].msg_control = NULL;
		rq->msg[i].msg_controllen = 0;
	}
	return rq;
}

static int udp_recvfrom_deinit(void *d)
{
	struct udp_recvfrom *rq = (struct udp_recvfrom *)d;
	free(rq);
	return 0;
}

static int udp_recvfrom_recv(int fd, void *d)
{
	struct udp_recvfrom *rq = (struct udp_recvfrom *)d;
	int ret = recvmsg(fd, &rq->msg[RX], MSG_DONTWAIT);
	if (ret > 0) {
		rq->fd = fd;
		rq->iov[RX].iov_len = ret;
		return 1;
	}

	return 0;
}

static int udp_recvfrom_handle(struct answer_ctx *ans, void *d)
{
	struct udp_recvfrom *rq = (struct udp_recvfrom *)d;

	/* Prepare TX address. */
	rq->addr.len = rq->msg[RX].msg_namelen;
	rq->msg[TX].msg_namelen = rq->addr.len;
	rq->iov[TX].iov_len = KNOT_WIRE_MAX_PKTSIZE;

	/* Process received pkt. */
	int ret = udp_handle(ans, rq->fd, &rq->addr, &rq->iov[RX], &rq->iov[TX]);
	if (ret != KNOT_EOK) {
		rq->iov[TX].iov_len = 0;
	}

	/* Reset RX buflen. */
	rq->iov[RX].iov_len = KNOT_WIRE_MAX_PKTSIZE;

	return ret;
}

static int udp_recvfrom_send(void *d)
{
	struct udp_recvfrom *rq = (struct udp_recvfrom *)d;
	int rc = 0;
	if (rq->iov[TX].iov_len > 0) {
		rc = sendmsg(rq->fd, &rq->msg[TX], 0);
	}

	/* Reset address lengths. */
	sockaddr_prep(&rq->addr);
	rq->msg[RX].msg_namelen = rq->addr.len;
	rq->msg[TX].msg_namelen = rq->addr.len;

	/* Return number of packets sent. */
	if (rc > 1) {
		return 1;
	}

	return 0;
}

#ifdef HAVE_RECVMMSG

/*! \brief Pointer to selected UDP send implementation. */
static int (*_send_mmsg)(int, sockaddr_t *, struct mmsghdr *, size_t) = 0;

/*!
 * \brief Send multiple packets.
 *
 * Basic, sendmsg() based implementation.
 */
int udp_sendmsg(int sock, sockaddr_t * addrs, struct mmsghdr *msgs, size_t count)
{
	int sent = 0;
	for (unsigned i = 0; i < count; ++i) {
		if (sendmsg(sock, &msgs[i].msg_hdr, 0) > 0) {
			++sent;
		}
	}

	return sent;
}

#ifdef ENABLE_SENDMMSG
/*! \brief sendmmsg() syscall interface. */
#ifndef HAVE_SENDMMSG
static inline int sendmmsg(int fd, struct mmsghdr *mmsg, unsigned vlen,
                           unsigned flags)
{
	return syscall(SYS_sendmmsg, fd, mmsg, vlen, flags, NULL);
}
#endif /* HAVE_SENDMMSG */

/*!
 * \brief Send multiple packets.
 *
 * sendmmsg() implementation.
 */
int udp_sendmmsg(int sock, sockaddr_t *_, struct mmsghdr *msgs, size_t count)
{
	UNUSED(_);
	return sendmmsg(sock, msgs, count, 0);
}
#endif /* ENABLE_SENDMMSG */

/* UDP recvmmsg() request struct. */
struct udp_recvmmsg {
	int fd;
	sockaddr_t *addrs;
	char *iobuf[NBUFS];
	struct iovec *iov[NBUFS];
	struct mmsghdr *msgs[NBUFS];
	unsigned rcvd;
	mm_ctx_t mm;
};

static void *udp_recvmmsg_init(void)
{
	mm_ctx_t mm;
	mm_ctx_mempool(&mm, sizeof(struct udp_recvmmsg));

	struct udp_recvmmsg *rq = mm.alloc(mm.ctx, sizeof(struct udp_recvmmsg));
	memcpy(&rq->mm, &mm, sizeof(mm_ctx_t));

	/* Initialize addresses. */
	rq->addrs = mm.alloc(mm.ctx, sizeof(sockaddr_t) * RECVMMSG_BATCHLEN);
	for (unsigned i = 0; i < RECVMMSG_BATCHLEN; ++i) {
		sockaddr_prep(rq->addrs + i);
	}

	/* Initialize buffers. */
	for (unsigned i = 0; i < NBUFS; ++i) {
		rq->iobuf[i] = mm.alloc(mm.ctx, KNOT_WIRE_MAX_PKTSIZE * RECVMMSG_BATCHLEN);
		rq->iov[i] = mm.alloc(mm.ctx, sizeof(struct iovec) * RECVMMSG_BATCHLEN);
		rq->msgs[i] = mm.alloc(mm.ctx, sizeof(struct mmsghdr) * RECVMMSG_BATCHLEN);
		memset(rq->msgs[i], 0, sizeof(struct mmsghdr) * RECVMMSG_BATCHLEN);
		for (unsigned k = 0; k < RECVMMSG_BATCHLEN; ++k) {
			rq->iov[i][k].iov_base = rq->iobuf[i] + k * KNOT_WIRE_MAX_PKTSIZE;
			rq->iov[i][k].iov_len = KNOT_WIRE_MAX_PKTSIZE;
			rq->msgs[i][k].msg_hdr.msg_iov = rq->iov[i] + k;
			rq->msgs[i][k].msg_hdr.msg_iovlen = 1;
			rq->msgs[i][k].msg_hdr.msg_name = rq->addrs + k;
			rq->msgs[i][k].msg_hdr.msg_namelen = rq->addrs[k].len;
		}
	}

	return rq;
}

static int udp_recvmmsg_deinit(void *d)
{
	struct udp_recvmmsg *rq = (struct udp_recvmmsg *)d;
	if (rq) {
		mp_delete(rq->mm.ctx);
	}

	return 0;
}

static int udp_recvmmsg_recv(int fd, void *d)
{
	struct udp_recvmmsg *rq = (struct udp_recvmmsg *)d;
	int n = recvmmsg(fd, rq->msgs[RX], RECVMMSG_BATCHLEN, MSG_DONTWAIT, NULL);
	if (n > 0) {
		rq->fd = fd;
		rq->rcvd = n;
	}
	return n;
}

static int udp_recvmmsg_handle(struct answer_ctx *st, void *d)
{
	struct udp_recvmmsg *rq = (struct udp_recvmmsg *)d;

	/* Handle each received msg. */
	int ret = 0;
	for (unsigned i = 0; i < rq->rcvd; ++i) {
		struct iovec *rx = rq->msgs[RX][i].msg_hdr.msg_iov;
		struct iovec *tx = rq->msgs[TX][i].msg_hdr.msg_iov;
		rx->iov_len = rq->msgs[RX][i].msg_len; /* Received bytes. */
		rq->addrs[i].len = rq->msgs[RX][i].msg_hdr.msg_namelen;

		ret = udp_handle(st, rq->fd, rq->addrs + i, rx, tx);
		if (ret != KNOT_EOK) { /* Do not send. */
			tx->iov_len = 0;
		}

		rq->msgs[TX][i].msg_len = tx->iov_len;
		rq->msgs[TX][i].msg_hdr.msg_namelen = rq->addrs[i].len;
	}

	return KNOT_EOK;
}

static int udp_recvmmsg_send(void *d)
{
	struct udp_recvmmsg *rq = (struct udp_recvmmsg *)d;
	int rc = _send_mmsg(rq->fd, rq->addrs, rq->msgs[TX], rq->rcvd);
	for (unsigned i = 0; i < rq->rcvd; ++i) {
		/* Reset buffer size and address len. */
		struct iovec *rx = rq->msgs[RX][i].msg_hdr.msg_iov;
		struct iovec *tx = rq->msgs[TX][i].msg_hdr.msg_iov;
		rx->iov_len = KNOT_WIRE_MAX_PKTSIZE; /* Reset RX buflen */
		tx->iov_len = KNOT_WIRE_MAX_PKTSIZE;

		sockaddr_prep(rq->addrs + i);
		rq->msgs[RX][i].msg_hdr.msg_namelen = rq->addrs[i].len;
		rq->msgs[TX][i].msg_hdr.msg_namelen = rq->addrs[i].len;
	}
	return rc;
}
#endif /* HAVE_RECVMMSG */

/*! \brief Initialize UDP master routine on run-time. */
void __attribute__ ((constructor)) udp_master_init()
{
	/* Initialize defaults. */
	_udp_init = udp_recvfrom_init;
	_udp_deinit = udp_recvfrom_deinit;
	_udp_recv = udp_recvfrom_recv;
	_udp_send = udp_recvfrom_send;
	_udp_handle = udp_recvfrom_handle;

	/* Optimized functions. */
#ifdef HAVE_RECVMMSG
	/* Check for recvmmsg() support. */
	if (dlsym(RTLD_DEFAULT, "recvmmsg") != 0) {
		recvmmsg(0, NULL, 0, 0, 0);
		if (errno != ENOSYS) {
			_udp_init = udp_recvmmsg_init;
			_udp_deinit = udp_recvmmsg_deinit;
			_udp_recv = udp_recvmmsg_recv;
			_udp_send = udp_recvmmsg_send;
			_udp_handle = udp_recvmmsg_handle;
		}
	}

	/* Check for sendmmsg() support. */
	_send_mmsg = udp_sendmsg;
#ifdef ENABLE_SENDMMSG
	sendmmsg(0, 0, 0, 0); /* Just check if syscall exists */
	if (errno != ENOSYS) {
		_send_mmsg = udp_sendmmsg;
	}
#endif /* ENABLE_SENDMMSG */
#endif /* HAVE_RECVMMSG */
}

int udp_reader(iohandler_t *h, dthread_t *thread)
{

	iostate_t *st = (iostate_t *)thread->data;

	/* Prepare structures for bound sockets. */
	unsigned thr_id = dt_get_id(thread);
	void *rq = _udp_init();
	ifacelist_t *ref = NULL;

	/* Create memory pool context. */
	mm_ctx_t mm;
	mm_ctx_mempool(&mm, 2 * sizeof(knot_pkt_t));

	/* Create UDP answering context. */
	struct answer_ctx ans_ctx;
	ans_ctx.srv = h->server;
	ans_ctx.slip = 0;
	ans_ctx.mm = &mm;

	/* Chose select as epoll/kqueue has larger overhead for a
	 * single or handful of sockets. */
	fd_set fds;
	FD_ZERO(&fds);
	int minfd = 0, maxfd = 0;
	int rcvd = 0;

	udp_pps_begin();

	/* Loop until all data is read. */
	for (;;) {

		/* Check handler state. */
		if (knot_unlikely(st->s & ServerReload)) {
			st->s &= ~ServerReload;
			maxfd = 0;
			minfd = INT_MAX;
			FD_ZERO(&fds);

			rcu_read_lock();
			ref_release((ref_t *)ref);
			ref = h->server->ifaces;
			if (ref) {
				iface_t *i = NULL;
				WALK_LIST(i, ref->l) {
					int fd = i->fd[IO_UDP];
					FD_SET(fd, &fds);
					maxfd = MAX(fd, maxfd);
					minfd = MIN(fd, minfd);
				}
			}
			rcu_read_unlock();
		}

		/* Cancellation point. */
		if (dt_is_cancelled(thread)) {
			break;
		}

		/* Wait for events. */
		fd_set rfds;
		FD_COPY(&fds, &rfds);
		int nfds = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (nfds <= 0) {
			if (errno == EINTR) continue;
			break;
		}
		/* Bound sockets will be usually closely coupled. */
		for (unsigned fd = minfd; fd <= maxfd; ++fd) {
			if (FD_ISSET(fd, &rfds)) {
				while ((rcvd = _udp_recv(fd, rq)) > 0) {
					_udp_handle(&ans_ctx, rq);
					_udp_send(rq);
					mp_flush(mm.ctx);
					udp_pps_sample(rcvd, thr_id);
				}
			}
		}
	}

	_udp_deinit(rq);
	ref_release((ref_t *)ref);
	mp_delete(mm.ctx);
	return KNOT_EOK;
}

int udp_master(dthread_t *thread)
{
	unsigned cpu = dt_online_cpus();
	if (cpu > 1) {
		unsigned cpu_mask[2];
		cpu_mask[0] = dt_get_id(thread) % cpu;
		cpu_mask[1] = (cpu_mask[0] + 2) % cpu;
		dt_setaffinity(thread, cpu_mask, 2);
	}

	/* Drop all capabilities on all workers. */
#ifdef HAVE_CAP_NG_H
        if (capng_have_capability(CAPNG_EFFECTIVE, CAP_SETPCAP)) {
                capng_clear(CAPNG_SELECT_BOTH);
                capng_apply(CAPNG_SELECT_BOTH);
        }
#endif /* HAVE_CAP_NG_H */

	iostate_t *st = (iostate_t *)thread->data;
	if (!st) return KNOT_EINVAL;
	iohandler_t *h = st->h;
	return udp_reader(h, thread);
}

int udp_master_destruct(dthread_t *thread)
{
	knot_dnssec_thread_cleanup();
	return KNOT_EOK;
}
