/*
 * Copyright (c) 2011-2012 Yaroslav Stavnichiy <yarosla@gmail.com>
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

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <netdb.h>

#include <http_parser.h>

#include <parserutils/input/inputstream.h>
#include <parserutils/charset/mibenum.h>
#include <uchardet/uchardet.h>

//#define WITH_SSL

#ifdef WITH_SSL
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#endif

#include <ev.h>

enum { DBG_INFO = 1, DBG_DEBUG, DBG_BODY, DBG_MAX };

#define VERSION "1.1"

/****************************************************************************************
 * List operations
 ****************************************************************************************/

struct cd_list {
	struct cd_list *prev;
	struct cd_list *next;
};

static inline void cd_list_add(struct cd_list *p, struct cd_list *list)
{
	p->prev = list->prev;
	p->next = list;
	p->next->prev = p;
	p->prev->next = p;
}

static inline void cd_list_del(struct cd_list *list)
{
	list->prev->next = list->next;
	list->next->prev = list->prev;
	list->prev = 0;
	list->next = 0;
}

static inline void cd_list_init(struct cd_list *list)
{
	list->prev = list;
	list->next = list;
}

static inline void cd_list_del_init(struct cd_list *list)
{
	cd_list_del(list);
	cd_list_init(list);
}

static inline int cd_list_empty(const struct cd_list *list)
{
	return (list->next == list);
}

#ifndef offsetof
        #define offsetof(TYPE, MEMBER) ((ULONG_PTR) &((TYPE*)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) \
                ((type *)(((char *)(ptr)) - offsetof(type,member)))
#endif

#define cd_list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define cd_list_first_entry(ptr, type, member) \
	cd_list_entry((ptr)->next, type, member)

#define cd_list_for_each_entry(typeof_pos, pos, head, member) \
	for (pos = cd_list_entry((head)->next, typeof_pos, member); \
			&pos->member != (head); \
			pos = cd_list_entry(pos->member.next, typeof_pos, member))

#define cd_list_for_each_entry_safe(typeof_pos, pos, n, head, member) \
	for (pos = cd_list_entry((head)->next, typeof_pos, member), \
			n = cd_list_entry(pos->member.next, typeof_pos, member); \
			&pos->member != (head); \
			pos = n, n = cd_list_entry(n->member.next, typeof_pos, member))

/****************************************************************************************
 * Atomic operations
 ****************************************************************************************/

typedef long long atomic_val_t;
typedef struct { volatile atomic_val_t val; } atomic_t;

atomic_t reqs_in_flight;	// number of sent HTTP packets
int concurrency_max;		// max reached concurrency
atomic_t concurrency_sum;	// sum of concurrency values to calc average value later

static inline void atomic_set(atomic_t *p, atomic_val_t val)
{
	p->val = val;
}

static inline atomic_val_t atomic_get(atomic_t *p)
{
	return p->val;
}

static inline atomic_val_t atomic_inc(atomic_t *p)
{
	return __sync_add_and_fetch(&p->val, 1);
}

static inline atomic_val_t atomic_add(atomic_t *p, int val)
{
	return __sync_add_and_fetch(&p->val, val);
}

static inline atomic_val_t atomic_dec(atomic_t *p)
{
	return __sync_sub_and_fetch(&p->val, 1);
}

/****************************************************************************************
 * Requests stats
 ****************************************************************************************/

#define STATS_INC(conn, name)					\
	do {							\
		atomic_inc(&conn->tdata->stats.name);		\
		atomic_inc(&conn->stats.name);			\
	} while (0)

#define STATS_ADD(conn, name, val)				\
	do {							\
		atomic_add(&conn->tdata->stats.name, (val));	\
		atomic_add(&conn->stats.name, (val));		\
	} while (0)

struct req_stats {
	atomic_t num_success;
	atomic_t num_success_prev;
	atomic_t num_fail;
	atomic_t num_fail_prev;
	atomic_t num_2xx;
	atomic_t num_bytes_received;
	atomic_t num_overhead_received;
	atomic_t num_connect;
};

static void print_stats_sep(void)
{
	printf("---------------------------------------------+-------------------+----------------------\n");
}

static void print_stats_header(void)
{
	printf("========================================================================================\n");
	printf("%8s | %8s %8s %8s %6s | %8s %8s | %10s %10s\n",
		"", "Conns", "Requests", "Success", "Failed",
		"2xx", "non-2xx", "Bytes", "Overhead");
	print_stats_sep();
}

static void print_stats_row(char *pfx, struct req_stats *stats)
{
	printf("%8s | %8lld %8lld %8lld %6lld | %8lld %8lld | %10lld %10lld\n",
		pfx,
		atomic_get(&stats->num_connect),
		atomic_get(&stats->num_success) + atomic_get(&stats->num_fail),
		atomic_get(&stats->num_success),
		atomic_get(&stats->num_fail),
		atomic_get(&stats->num_2xx),
		atomic_get(&stats->num_success) - atomic_get(&stats->num_2xx),
		atomic_get(&stats->num_bytes_received),
		atomic_get(&stats->num_overhead_received));
}

/****************************************************************************************
 * Structures
 ****************************************************************************************/

#if (__SIZEOF_POINTER__ == 8)
typedef uint64_t int_to_ptr;
#else
typedef uint32_t int_to_ptr;
#endif

#define MEM_GUARD 128

#define MAX_DOMAINS_NUMBER 1024

#define REQ_DELIM ("\r\n")
#define REQ_DELIM_LEN 2

enum comm_press_mode { PM_NUMBER, PM_TIME };

struct common_config {
	int debug_level;

	int tot_domains_number;
	int concurrency;
	int thread_concurrency_limit;
	int num_connections;
	int num_requests;
	int num_threads;

	int need_to_stop;
	enum comm_press_mode press_mode;

	int progress_step;
	int secure;
	const char* ssl_cipher_priority;

	int keep_alive:1;
	int quiet:1;
	int save_cookies:1;
	int include_non2xx:1;

	char _padding1[MEM_GUARD];
	volatile int request_counter;
	char _padding2[MEM_GUARD];

	int range;
	int range_start;
	int range_end;
	double sleep_time;
	int percentile;
};

enum body_parser_parsing_state {PS_NORMAL, PS_DIGITS};

struct body_parser {
	parserutils_inputstream *stream;
	parserutils_inputstream *regexp;
	int found;
	int enabled;
	enum body_parser_parsing_state pstate;
	int got_digit;
};

struct config {
	struct addrinfo *saddr;
	const char* uri_path;
	const char* uri_host;
	char* url;
	char request_headers[4096];
	int request_headers_length;
	char* request_body;
	size_t request_body_length;
	int secure;

	struct req_stats stats;

#ifdef WITH_SSL
	gnutls_certificate_credentials_t ssl_cred;
	gnutls_priority_t priority_cache;
#endif
};

static struct common_config common_config;
struct body_parser body_parser;
static struct config config[MAX_DOMAINS_NUMBER];
static char host_buf[1024];

enum nxweb_chunked_decoder_state_code {CDS_CR1 = -2, CDS_LF1 = -1, CDS_SIZE = 0, CDS_LF2, CDS_DATA};

typedef struct nxweb_chunked_decoder_state {
	enum nxweb_chunked_decoder_state_code state;
	unsigned short final_chunk:1;
	unsigned short monitor_only:1;
	int64_t chunk_bytes_left;
} nxweb_chunked_decoder_state;

enum connection_state {C_CONNECTING, C_HANDSHAKING, C_WRITING, C_READING_HEADERS, C_READING_BODY, C_THROTTLED};

typedef struct connection {
	struct cd_list throttled_list;

	struct ev_loop* loop;
	struct thread_config* tdata;
	int fd;
	int idx; //domain index in case of multiple domains
	int socket_id;	// global socket ID
	ev_io watch_read;
	ev_io watch_write;
	ev_timer watch_resume_write;
	ev_tstamp last_activity;

	ev_tstamp tstamp_request_sent;
	ev_tstamp tstamp_response_first_byte;

	nxweb_chunked_decoder_state cdstate;

#ifdef WITH_SSL
	gnutls_session_t session;
#endif

	int write_pos;
	int read_pos;
	int bytes_to_read;
	int bytes_received;
	int alive_count;

	struct req_stats stats;

	int keep_alive:1;
	int chunked:1;
	int done:1;
	int secure:1;
	int in_use:1;
	int yield:1;

	int status_code;

	char buf[32768];
	int cookie_len;
	char* cookies;
	char* body_ptr;

	enum connection_state state;
} connection;

typedef struct thread_config {
	pthread_t tid;
	connection *conns;
	struct cd_list throttled_conns;
	int id;
	int num_conn;
	int reqs_in_flight;
	struct ev_loop* loop;
	ev_tstamp start_time;
	ev_timer watch_heartbeat;

	int shutdown_in_progress;

	struct req_stats stats;
	ev_tstamp avg_req_time;

	long long request_counter;

#ifdef WITH_SSL
	_Bool ssl_identified;
	_Bool ssl_dhe;
	_Bool ssl_ecdh;
	gnutls_kx_algorithm_t ssl_kx;
	gnutls_credentials_type_t ssl_cred;
	int ssl_dh_prime_bits;
# ifdef GNUTLS3
	gnutls_ecc_curve_t ssl_ecc_curve;
# endif
	gnutls_protocol_t ssl_protocol;
	gnutls_certificate_type_t ssl_cert_type;
	gnutls_x509_crt_t ssl_cert;
	gnutls_compression_method_t ssl_compression;
	gnutls_cipher_algorithm_t ssl_cipher;
	gnutls_mac_algorithm_t ssl_mac;
#endif
} thread_config;

#define CLASS_NUM 5

struct stat_class {
	int begin;
	int end;
	atomic_t *counter;
};

struct stat_codes {
	struct stat_class codes[CLASS_NUM];
	atomic_t counter;
	atomic_t out_of_class;
};

static struct stat_codes stat_codes;

/****************************************************************************************
 * Logging
 ****************************************************************************************/

void nxweb_die(const char* fmt, ...)
{
	va_list ap;
	fprintf(stderr, "FATAL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static inline const char* get_current_time(char* buf, int max_buf_size)
{
	time_t t;
	struct tm tm;
	time(&t);
	localtime_r(&t, &tm);
	strftime(buf, max_buf_size, "%T", &tm); // %T=%H:%M:%S
	return buf;
}

void nxweb_log_error(connection *conn, const char* fmt, ...)
{
	char cur_time[32];
	va_list ap;
	int socket_id = 0;

	int conn_tdata_id = (conn == NULL) ? 0 : (conn->tdata->id - 1);

	if (conn)
		socket_id = conn->socket_id;

	get_current_time(cur_time, sizeof(cur_time));
	flockfile(stderr);
	fprintf(stderr, "%s %s [%2d:%2u] ", cur_time, "ERROR", conn_tdata_id, socket_id);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	funlockfile(stderr);
}

void nxweb_dbg(int level, connection *conn, const char *fmt, ...)
{
	static const char* level_str[] = {"", " INFO", "DEBUG", "BODY", NULL};
	char cur_time[32];
	va_list ap;
	char buf[1024];
	int n = 0;
	int socket_id = 0;

	if (level > DBG_MAX)
		return;

	if (level > common_config.debug_level)
		return;

	if (conn)
		socket_id = conn->socket_id;

	get_current_time(cur_time, sizeof(cur_time));
	n += snprintf(buf, sizeof(buf) - n, "%s %s [%2d:%2u] ",
		cur_time, level_str[level], conn->tdata->id - 1, socket_id);

	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);

	fprintf(stdout, "%s", buf);
}

/****************************************************************************************
* Percentile calculation
****************************************************************************************/

#define P_UPPER_BOUND 10	// in seconds
#define P_RANGE 100000		// discretization between 0 and P_UPPER_BOUND

atomic_t percentile[P_RANGE+1];

/* register a request time in storage for percentile calculation */
void reg_percentile_val(double val, atomic_t* p)
{
	double range = P_RANGE;
	double step = P_UPPER_BOUND / range;
	int idx = val / step;

	if (idx > P_RANGE)
		idx = P_RANGE;
	atomic_inc(&p[idx]);
}

/* returns a total number of requests registered in perecentile */
atomic_val_t get_num_registered(atomic_t* p)
{
	atomic_val_t num = 0;
	int i = 0;
	for (i = 0; i <= P_RANGE; i++)
		num += atomic_get(&p[i]);
	return num;
}

/* returns percentile value in seconds */
double get_percentile(int x, atomic_t* p) // p - %
{
	atomic_val_t val_num = get_num_registered(p);
	double th = val_num * (100 - x) / 100.0;
	atomic_val_t threshold = (atomic_val_t) (th - (atomic_val_t) th >= 0.5) ? th + 1 : th;

	int idx = P_RANGE;
	while(idx > 0) {
		threshold -= atomic_get(&p[idx]);
		if (threshold <= 0)
			break;
		idx--;
	}

	return idx * P_UPPER_BOUND / (double) P_RANGE;
}

void clear_percentile(atomic_t* p)
{
	int i = 0;
	for (i = 0; i <= P_RANGE; i++)
		atomic_set(&p[i], 0);
}

void reg_summary_percentile_val(double val)
{
	reg_percentile_val(val, percentile);
}

double get_summary_percentile(int x)
{
	if (!(x > 0 && x < 100))
		nxweb_die("Percentile value should be between 1 and 99");

	return get_percentile(x, percentile);
}

/* percentile calculation stuff for intermediate reports */
atomic_t tmp_percentile[P_RANGE+1];

void reg_tmp_percentile_val(double val)
{
	reg_percentile_val(val, tmp_percentile);
}

void clear_tmp_percentile()
{
	clear_percentile(tmp_percentile);
}

double get_tmp_percentile(int x) // x - %
{
	if (!(x > 0 && x < 100))
		nxweb_die("Percentile value should be between 1 and 99");

	double res = get_percentile(x, tmp_percentile);
	clear_tmp_percentile();
	return res;
}

/****************************************************************************************
 * HTTP-level callbacks
 ****************************************************************************************/

static void unthrottle(connection *conn, thread_config *tdata);
/* Called when first byte of response received */
static void http_response_first_byte(connection *conn)
{
	nxweb_dbg(DBG_DEBUG, conn, "%2d ... rcv %3d %-20s HTTP header received, CL: %d%s\n",
		atomic_get(&reqs_in_flight),
		conn->status_code,
		config[conn->idx].url,
		conn->bytes_to_read, conn->keep_alive ? ", KA" : "");

	conn->tstamp_response_first_byte = conn->last_activity;
	reg_summary_percentile_val(conn->tstamp_response_first_byte - conn->tstamp_request_sent);
	reg_tmp_percentile_val(conn->tstamp_response_first_byte - conn->tstamp_request_sent);
}

/* Called when last byte of response received */
static void http_response_last_byte(connection *conn)
{
	unthrottle(conn, conn->tdata);

	nxweb_dbg(DBG_INFO, conn, "%2d --- rcv %3d %-20s TTFB: %.3f, TTLB: %.3f, CL: %d%s\n",
		atomic_get(&reqs_in_flight),
		conn->status_code,
		config[conn->idx].url,
		conn->tstamp_response_first_byte - conn->tstamp_request_sent,
		conn->last_activity - conn->tstamp_request_sent,
		conn->bytes_to_read,
		conn->keep_alive ? ", KA" : "");
}

/* Called before sending new request to the server */
static void http_before_request_send(struct ev_loop *loop, connection *conn)
{
}

/* Called after sending new request to the server */
static void http_after_request_send(struct ev_loop *loop, connection *conn)
{
	nxweb_dbg(DBG_INFO, conn, "%2d +++ snd GET %-20s\n",
		atomic_get(&reqs_in_flight),
		config[conn->idx].url);

	conn->tstamp_request_sent = ev_now(loop);
}

static inline int conn_get(connection *conn)
{
	int in_flight;

	if (conn->yield) {
		conn->yield = 0;
		return -1;
	}

	if (conn->tdata->reqs_in_flight == common_config.thread_concurrency_limit)
		return -1;

	conn->tdata->reqs_in_flight++;
	atomic_inc(&reqs_in_flight);

	in_flight = atomic_get(&reqs_in_flight);
	if (in_flight > concurrency_max)
		concurrency_max = in_flight;

	atomic_add(&concurrency_sum, in_flight);

	conn->in_use = 1;
	nxweb_dbg(DBG_DEBUG, conn, "%d conn_get\n", in_flight);
	return 0;
}

static inline void conn_put(connection *conn, int good)
{
	if (!conn->in_use)
		return;

	conn->tdata->reqs_in_flight--;
	atomic_dec(&reqs_in_flight);
	conn->in_use = 0;
	nxweb_dbg(DBG_DEBUG, conn, "%d conn_put: state %d\n", atomic_get(&reqs_in_flight), conn->state);

	if (good) {
		switch(conn->state) {
		case C_READING_HEADERS:
		case C_READING_BODY:
			http_response_last_byte(conn);
		default:
			;
		}
	}
}

/* Say libev that we are ready to handle read or write (i.e. *wr*) */
static void conn_io_start(connection *conn, int wr)
{
	nxweb_dbg(DBG_DEBUG, conn, "start watching %s\n", (wr == EV_WRITE) ? "WRITEs" : "READs");
	ev_io_start(conn->loop, (wr == EV_WRITE) ? &conn->watch_write : &conn->watch_read);
}

/* Say libev that we are not ready to handle read or write (i.e. *wr*) */
static void conn_io_stop(connection *conn, int wr)
{
	nxweb_dbg(DBG_DEBUG, conn, "stop watching  %s\n", (wr == EV_WRITE) ? "WRITEs" : "READs");
	ev_io_stop(conn->loop, (wr == EV_WRITE) ? &conn->watch_write : &conn->watch_read);
}

static void start_write(connection* conn)
{
	conn_io_start(conn, EV_WRITE);
	ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
}

static void resume_write_cb(struct ev_loop *loop, ev_timer *w, int revents);

static void resume_write(connection* conn)
{
	if(common_config.sleep_time) {
		ev_tstamp now_ts = ev_time();
		double shed_time = (conn->tdata->request_counter + 1) * common_config.sleep_time;
		double sleep_time = shed_time - (now_ts - conn->tdata->start_time);

		if (sleep > 0) {
			// start writing in sleep seconds
			ev_timer_init(&conn->watch_resume_write, resume_write_cb, sleep_time, 0);
			ev_timer_start(conn->tdata->loop, &conn->watch_resume_write);
		}
		else
			start_write(conn);
	}
	else
		start_write(conn);
}

static void conn_throttle(connection *conn)
{
	cd_list_add(&conn->throttled_list, &conn->tdata->throttled_conns);

	conn->state = C_THROTTLED;
	nxweb_dbg(DBG_DEBUG, conn, "connection throttled!\n");
	conn_io_stop(conn, EV_WRITE);
	/*
	 * FIXME: remove me!?
	 *
	 * if (!ev_is_active(&conn->watch_read))
	 *	conn_io_start(conn, EV_READ);
	 * ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
	 */
}

static void conn_unthrottle(connection *conn)
{
	cd_list_del(&conn->throttled_list);

	conn->state = C_WRITING;
	nxweb_dbg(DBG_DEBUG, conn, "connection unthrottled!\n");
	conn->write_pos = 0;
	resume_write(conn);
}

/* Find throttled connectionss and unthrottle them */
static void unthrottle(connection *conn, thread_config *tdata)
{
	connection *c;

	if (cd_list_empty(&tdata->throttled_conns))
		return;

	c = cd_list_first_entry(&tdata->throttled_conns, connection, throttled_list);
	assert(c);
	assert(c->state == C_THROTTLED);

	if (conn)
		conn->yield = 1; // be fair and let others work
	conn_unthrottle(c);
}

/****************************************************************************************
 * Connection stats
 ****************************************************************************************/

static inline void inc_http_status(connection* conn)
{
	int code = conn->status_code;
	int cl = code / 100;
	atomic_inc(&stat_codes.counter);
	if (cl > CLASS_NUM || cl < 1 || stat_codes.codes[cl - 1].end < code) {
		nxweb_dbg(DBG_DEBUG, conn, "unknown status code %3d\n", code);
		atomic_inc(&stat_codes.out_of_class);
		return;
	}
	atomic_inc(stat_codes.codes[cl - 1].counter + code % 100);
}

static inline void inc_success(connection* conn)
{
	conn_put(conn, 1);

	switch (conn->status_code) {
	case 200: /* OK */
	case 201: /* Created */
	case 202: /* Accepted */
		/*
		 * Important.
		 *
		 * Only these 3 codes are treated as success for performance testing.
		 * Others are not ok, for instance:
		 *
		 * 204 (No Content) - it is not OK, because we are expecting data
		 *
		 * 206 (Partial content) - it is not OK, because it means we will
		 * have different number of sent and recieved packets
		 */
		STATS_INC(conn, num_2xx);
	}

	STATS_INC(conn, num_success);
	STATS_ADD(conn, num_bytes_received, conn->bytes_received);
	STATS_ADD(conn, num_overhead_received, (conn->body_ptr - conn->buf));
	inc_http_status(conn);
}

static inline void inc_fail(connection* conn)
{
	STATS_INC(conn, num_fail);
}

static inline void inc_connect(connection* conn)
{
	STATS_INC(conn, num_connect);
}

enum {ERR_AGAIN = -2, ERR_ERROR = -1, ERR_RDCLOSED = -3};

/****************************************************************************************
 * Socket and connection management
 ****************************************************************************************/

static void process_http_chunk(connection *conn, const char *buf, size_t len);

static inline ssize_t conn_read(connection* conn, void* buf, size_t size)
{
#ifdef WITH_SSL
	if (conn->secure) {
		ssize_t ret;
		ret = gnutls_record_recv(conn->session, buf, size);
		if (ret > 0) {
			process_http_chunk(conn, buf, size);
			return ret;
		}
		if (ret == GNUTLS_E_AGAIN)
			return ERR_AGAIN;
		if (ret == 0)
			return ERR_RDCLOSED;
		return ERR_ERROR;
	}
	else
#endif
	{
		ssize_t ret;
		ret = read(conn->fd, buf, size);
		if (ret > 0) {
			process_http_chunk(conn, buf, size);
			return ret;
		}
		if (ret == 0)
			return ERR_RDCLOSED;
		if (errno == EAGAIN)
			return ERR_AGAIN;
		return ERR_ERROR;
	}
}

static inline ssize_t conn_write(connection* conn, const void* buf, size_t size)
{
#ifdef WITH_SSL
	if (conn->secure) {
		ssize_t ret;
		ret = gnutls_record_send(conn->session, buf, size);
		if (ret >= 0)
			return ret;
		if (ret == GNUTLS_E_AGAIN)
			return ERR_AGAIN;
		return ERR_ERROR;
	}
	else
#endif
	{
		ssize_t ret;
		ret = write(conn->fd, buf, size);
		if (ret >= 0)
			return ret;
		if (errno == EAGAIN)
			return ERR_AGAIN;
		return ERR_ERROR;
	}
}

static inline void _nxweb_close_good_socket(int fd)
{
	//  struct linger linger;
	//  linger.l_onoff = 0; // gracefully shutdown connection
	//  linger.l_linger = 0;
	//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	//  shutdown(fd, SHUT_RDWR);
	close(fd);
}

static inline void _nxweb_close_bad_socket(int fd)
{
	struct linger linger;
	linger.l_onoff = 1;
	linger.l_linger = 0; // timeout for completing writes
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	close(fd);
}

static inline void conn_close(connection* conn, int good)
{
	conn_put(conn, good);

#ifdef WITH_SSL
	if (conn->secure)
		gnutls_deinit(conn->session);
#endif
	if (good)
		_nxweb_close_good_socket(conn->fd);
	else
		_nxweb_close_bad_socket(conn->fd);
}

static inline int setup_socket(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;
	if (fcntl(fd, F_SETFL, flags |= O_NONBLOCK) < 0)
		return -1;

	int nodelay = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)))
		return -1;

	//  struct linger linger;
	//  linger.l_onoff = 1;
	//  linger.l_linger = 10; // timeout for completing reads/writes
	//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

	return 0;
}

static int more_requests_to_run()
{
	int rc = __sync_add_and_fetch(&common_config.request_counter, 1);
	if ((common_config.press_mode == PM_NUMBER && rc > common_config.num_requests) ||
			(common_config.press_mode == PM_TIME && common_config.need_to_stop)) {
		return 0;
	}
	if (!common_config.quiet && common_config.progress_step >= 10 &&
			(rc % common_config.progress_step == 0 || rc == common_config.num_requests)) {
		printf("%d requests launched\n", rc);
	}
	return 1;
}

static void reset_body_parser();

static int open_socket(connection* conn)
{
	if (ev_is_active(&conn->watch_write))
		conn_io_stop(conn, EV_WRITE);
	if (ev_is_active(&conn->watch_read))
		conn_io_stop(conn, EV_READ);

	if (!more_requests_to_run(conn)) {
		conn->done = 1;
		ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
		return 1;
	}

	inc_connect(conn);

	nxweb_dbg(DBG_DEBUG, conn, "open socket to '%s'\n", config[conn->idx].uri_host);

	conn->fd = socket(config[conn->idx].saddr->ai_family, config[conn->idx].saddr->ai_socktype,
			config[conn->idx].saddr->ai_protocol);
	if (conn->fd == -1) {
		fprintf(stderr, "%s", strerror_r(errno, conn->buf, sizeof(conn->buf)));
		nxweb_log_error(conn, "can't open socket [%d] %s", errno, conn->buf);
		return -1;
	}
	if (setup_socket(conn->fd)) {
		nxweb_log_error(conn, "can't setup socket");
		return -1;
	}
	if (connect(conn->fd, config[conn->idx].saddr->ai_addr, config[conn->idx].saddr->ai_addrlen)) {
		if (errno != EINPROGRESS && errno != EALREADY && errno != EISCONN) {
			nxweb_log_error(conn, "can't connect %d", errno);
			return -1;
		}
	}

#ifdef WITH_SSL
	if (config[conn->idx].secure) {
		gnutls_init(&conn->session, GNUTLS_CLIENT);
		gnutls_server_name_set(conn->session, GNUTLS_NAME_DNS, config[conn->idx].uri_host,
				strlen(config[conn->idx].uri_host));
		gnutls_priority_set(conn->session, config[conn->idx].priority_cache);
		gnutls_credentials_set(conn->session, GNUTLS_CRD_CERTIFICATE, config[conn->idx].ssl_cred);
		gnutls_transport_set_ptr(conn->session, (gnutls_transport_ptr_t)(int_to_ptr)conn->fd);
	}
#endif // WITH_SSL

	conn->state = C_CONNECTING;
	conn->write_pos = 0;
	conn->alive_count = 0;
	conn->done = 0;
	if (body_parser.enabled)
		reset_body_parser();
	ev_io_set(&conn->watch_write, conn->fd, EV_WRITE);
	ev_io_set(&conn->watch_read, conn->fd, EV_READ);
	resume_write(conn);
	return 0;
}



static void rearm_socket(connection* conn)
{
	if (ev_is_active(&conn->watch_write))
		conn_io_stop(conn, EV_WRITE);
	if (ev_is_active(&conn->watch_read))
		conn_io_stop(conn, EV_READ);

	if (body_parser.enabled && !body_parser.found) {
		inc_fail(conn);
		nxweb_log_error(conn, "regular expression is not found");
	}
	else {
		inc_success(conn);
	}

	if (!common_config.keep_alive || !conn->keep_alive) {
		if (!conn->keep_alive)
			nxweb_dbg(DBG_DEBUG, conn, "reopen socket because server do not want to use Keep-Alive\n");
		conn_close(conn, 1);
		open_socket(conn);
	}
	else {
		if (!more_requests_to_run()) {
			conn_close(conn, 1);
			conn->done = 1;
			ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
			return;
		}
		conn->alive_count++;
		conn->state = C_WRITING;
		conn->write_pos = 0;
		resume_write(conn);
	}
}

/****************************************************************************************
 * HTTP parser
 ****************************************************************************************/

#ifdef WITH_SSL
static void retrieve_ssl_session_info(connection* conn)
{
	if (conn->tdata->ssl_identified)
		return; // already retrieved
	conn->tdata->ssl_identified = 1;
	gnutls_session_t session = conn->session;
	conn->tdata->ssl_kx = gnutls_kx_get(session);
	conn->tdata->ssl_cred = gnutls_auth_get_type(session);
	int dhe = (conn->tdata->ssl_kx == GNUTLS_KX_DHE_RSA || conn->tdata->ssl_kx == GNUTLS_KX_DHE_DSS);
# ifdef GNUTLS3
	int ecdh = (conn->tdata->ssl_kx == GNUTLS_KX_ECDHE_RSA || conn->tdata->ssl_kx == GNUTLS_KX_ECDHE_ECDSA);
# endif
	if (dhe)
		conn->tdata->ssl_dh_prime_bits = gnutls_dh_get_prime_bits(session);
# ifdef GNUTLS3
	if (ecdh)
		conn->tdata->ssl_ecc_curve = gnutls_ecc_curve_get(session);
# endif
	conn->tdata->ssl_dhe = dhe;
# ifdef GNUTLS3
	conn->tdata->ssl_ecdh = ecdh;
# endif
	conn->tdata->ssl_protocol = gnutls_protocol_get_version(session);
	conn->tdata->ssl_cert_type = gnutls_certificate_type_get(session);
	if (conn->tdata->ssl_cert_type == GNUTLS_CRT_X509) {
		const gnutls_datum_t *cert_list;
		unsigned int cert_list_size = 0;
		cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
		if (cert_list_size > 0) {
			gnutls_x509_crt_init(&conn->tdata->ssl_cert);
			gnutls_x509_crt_import(conn->tdata->ssl_cert, &cert_list[0], GNUTLS_X509_FMT_DER);
		}
	}
	conn->tdata->ssl_compression = gnutls_compression_get(session);
	conn->tdata->ssl_cipher = gnutls_cipher_get(session);
	conn->tdata->ssl_mac = gnutls_mac_get(session);
}
#endif // WITH_SSL

static int decode_chunked_stream(nxweb_chunked_decoder_state* decoder_state, char* buf, int* buf_len)
{
	char* p=buf;
	char* d=buf;
	char* end = buf + *buf_len;
	while (p < end) {
		char c = *p;
		switch (decoder_state->state) {
			case CDS_DATA:
				if (end - p >= decoder_state->chunk_bytes_left) {
					p += decoder_state->chunk_bytes_left;
					decoder_state->chunk_bytes_left = 0;
					decoder_state->state = CDS_CR1;
					d = p;
					break;
				}
				else {
					decoder_state->chunk_bytes_left -= (end - p);
					if (!decoder_state->monitor_only)
						*buf_len = end - buf;
					return 0;
				}
			case CDS_CR1:
				if (c != '\r')
					return -1;
				p++;
				decoder_state->state = CDS_LF1;
				break;
			case CDS_LF1:
				if (c != '\n')
					return -1;
				if (decoder_state->final_chunk) {
					if (!decoder_state->monitor_only)
						*buf_len = d - buf;
					return 1;
				}
				p++;
				decoder_state->state = CDS_SIZE;
				break;
			case CDS_SIZE: // read digits until CR2
				if (c == '\r') {
					if (!decoder_state->chunk_bytes_left) {
						// terminator found
						decoder_state->final_chunk = 1;
					}
					p++;
					decoder_state->state = CDS_LF2;
				}
				else {
					if (c >= '0' && c <= '9')
						c -= '0';
					else if (c >= 'A' && c <= 'F')
						c = c - 'A' + 10;
					else if (c >= 'a' && c <= 'f')
						c = c - 'a' + 10;
					else
						return -1;
					decoder_state->chunk_bytes_left = (decoder_state->chunk_bytes_left << 4) + c;
					p++;
				}
				break;
			case CDS_LF2:
				if (c != '\n')
					return -1;
				p++;
				if (!decoder_state->monitor_only) {
					memmove(d, p, end-p);
					end -= p - d;
					p = d;
				}
				decoder_state->state = CDS_DATA;
				break;
		}
	}
	if (!decoder_state->monitor_only)
		*buf_len = d - buf;
	return 0;
}

static char* find_end_of_http_headers(char* buf, int len, char** start_of_body)
{
	if (len < 4)
		return 0;
	char* p;
	for (p = memchr(buf + 3, '\n', len - 3); p; p = memchr(p + 1, '\n', len - (p - buf) - 1)) {
		if (*(p - 1) == '\n') {
			*start_of_body = p + 1;
			return p - 1;
		}
		if (*(p - 3) == '\r' && *(p - 2) == '\n' && *(p - 1) == '\r') {
			*start_of_body = p + 1;
			return p - 3;
		}
	}
	return 0;
}

typedef struct {
	connection* conn;
	int cookie_next:1 ;
	int cookie_starts_new:1 ;
} parser_state;


static int header_field_cb(http_parser *parser, const char *p, size_t len)
{
	parser_state* state = parser->data;
	const char cookie_header[] = "Set-Cookie";
	size_t cookie_len = sizeof(cookie_header) - 1;
	if (len == cookie_len &&
			strncmp(p, cookie_header, cookie_len) == 0)
		state->cookie_next = 1;
	else
		state->cookie_next = 0;
	return 0;
}

static int header_value_cb(http_parser *parser, const char *p, size_t len)
{
	parser_state* state = parser->data;

	if (state->cookie_next) {
		connection* conn = state->conn;
		char* end = memchr(p, ';', len);
		if (end)
			len = end - p;

		size_t total_len;
		if (!state->cookie_starts_new) {
			nxweb_dbg(DBG_DEBUG, conn, "save cookie\n");
			// cookies not starts filling, so adding header
			const char cook_header[] = "Cookie: ";
			total_len = len + sizeof(cook_header) - 1 + REQ_DELIM_LEN;
			char* cookie = malloc(total_len + 1);
			if (!cookie)
				nxweb_die("Not enough memory to save cookies");

			size_t cookie_str_size = total_len - REQ_DELIM_LEN + 1;
			snprintf(cookie, cookie_str_size, "%s%s", cook_header, p);

			if (conn->cookies) {
				free(conn->cookies);
				state->cookie_starts_new = 1;
			}
			conn->cookies = cookie;
			conn->cookie_len = total_len;
			state->cookie_starts_new = 1;
		} else {
			nxweb_dbg(DBG_DEBUG, conn, "appending cookie\n");
			size_t old_len = conn->cookie_len;
			// cookies starts filling, so just adding new value
			total_len = len + old_len + 1; /* old_cookie;new_cookie */
			conn->cookies = realloc(conn->cookies, total_len + 1);
			if (!conn->cookies)
				nxweb_die("Not enough memory to save cookies");
			conn->cookie_len = total_len;

			char* start_new_cookie = conn->cookies + old_len;
			start_new_cookie -= REQ_DELIM_LEN; // cut terminating symbols
			snprintf(start_new_cookie, len + REQ_DELIM_LEN, ";%s", p);
		}
		// terminating symbols at cookie string
		memcpy(conn->cookies + total_len - REQ_DELIM_LEN, REQ_DELIM, REQ_DELIM_LEN);
	}
	return 0;
}


static void parse_headers(connection* conn)
{
	http_parser parser;
	parser_state state;

	http_parser_settings settings = {0};

	if (common_config.save_cookies) {
		settings.on_header_field = header_field_cb;
		settings.on_header_value = header_value_cb;

		state.conn = conn;
		state.cookie_next = 0;
		state.cookie_starts_new = 0;
		parser.data = &state;
	}

	http_parser_init(&parser, HTTP_RESPONSE);

	*(conn->body_ptr - 1) = '\0';
	conn->bytes_to_read =- 1;

	http_parser_execute(&parser, &settings, conn->buf, conn->body_ptr - conn->buf);


	conn->bytes_to_read = parser.content_length;
	conn->chunked = !!(parser.flags & F_CHUNKED);
	conn->keep_alive = !!(parser.flags & F_CONNECTION_KEEP_ALIVE);
	conn->status_code = parser.status_code;

	http_response_first_byte(conn);

	if (conn->chunked) {
		conn->bytes_to_read =- 1;
		memset(&conn->cdstate, 0, sizeof(conn->cdstate));
		conn->cdstate.monitor_only = 1;
	}

	conn->bytes_received = conn->read_pos - (conn->body_ptr - conn->buf); // what already read
}

static int parse_uri(const char* uri, int idx)
{
	if (!strncmp(uri, "http://", 7))
		uri += 7;
#ifdef WITH_SSL
	if (!strncmp(uri, "https://", 8)) {
		uri += 8;
		config[idx].secure = 1;
	}
#endif
	const char* p = strchr(uri, '/');
	if (!p) {
		config[idx].uri_host = strdup(uri);
		config[idx].uri_path = "/";
		return 0;
	}
	if ((p - uri) > sizeof(host_buf) - 1)
		return -1;

	strncpy(host_buf, uri, (p - uri));
	host_buf[(p - uri)] = '\0';
	config[idx].uri_host = strdup(host_buf);
	config[idx].uri_path = strdup(p);
	size_t url_len = strlen(host_buf) + strlen(p);
	config[idx].url = (char *)malloc(url_len + 1); //+'\0'
	if (config[idx].url)
		sprintf(config[idx].url, "%s%s", host_buf, p);
	return 0;
}

/****************************************************************************************
 * Body parser
 ****************************************************************************************/

static parserutils_error detect(const uint8_t *data, size_t len, uint16_t *mibenum, uint32_t *source)
{
	parserutils_error err = PARSERUTILS_OK;

	uchardet_t det = uchardet_new();
	if (uchardet_handle_data(det, (const char *)data, len))
		err = PARSERUTILS_BADPARM;
	uchardet_data_end(det);

	const char *nm = uchardet_get_charset(det);
	uchardet_delete(det);

	*mibenum = parserutils_charset_mibenum_from_name(nm, strlen(nm));
	*source = 1;
	return err;
}

static void *myalloc(void *ptr, size_t len, void *pw)
{
	return realloc(ptr, len);
}

static void set_body_parser(const char *expr)
{
	parserutils_error err;
	err = parserutils_inputstream_create(NULL, 0, detect, &body_parser.stream);
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to create inputstream");
	err = parserutils_inputstream_create(NULL, 0, detect, &body_parser.regexp);
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to create inputstream.");
	err = parserutils_inputstream_append(body_parser.regexp, (const uint8_t *)expr, strlen(expr));
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to append data to inputstream");
	body_parser.found = 0;
	body_parser.enabled = 1;
	body_parser.pstate = PS_NORMAL;
	body_parser.got_digit = 0;
}

static void reset_body_parser()
{
	parserutils_error err;
	err = parserutils_inputstream_destroy(body_parser.stream);
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to destroy inputstream");
	err = parserutils_inputstream_create(NULL, 0, detect, &body_parser.stream);
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to create inputstream");
	body_parser.found = 0;
	body_parser.regexp->cursor = 0;
	body_parser.pstate = PS_NORMAL;
	body_parser.got_digit = 0;
}

static void parse_body_chunk(const char *buf, size_t len)
{
	parserutils_error err;
	err = parserutils_inputstream_append(body_parser.stream, (const uint8_t *)buf, len);
	if (err != PARSERUTILS_OK)
		nxweb_die("body parser - unable to append data to inputstream");

	size_t slen, rlen;
	const uint8_t *schr, *rchr;

	while (parserutils_inputstream_peek(body_parser.stream, 0, &schr, &slen) == PARSERUTILS_OK)
		switch (body_parser.pstate) {
			case PS_NORMAL:
normal_state:
				if (parserutils_inputstream_peek(body_parser.regexp, 0,
						&rchr, &rlen) == PARSERUTILS_OK) {
					if (*rchr == '\\' && rlen == 1) {
						parserutils_inputstream_advance(body_parser.regexp, 1);
						if (parserutils_inputstream_peek(body_parser.regexp, 0, &rchr, &rlen)
								== PARSERUTILS_OK && rlen == 1)
							switch (*rchr) {
								case 'd':
									body_parser.pstate = PS_DIGITS;
									parserutils_inputstream_advance(
										body_parser.regexp, 1);
									break;
								default:
									nxweb_die("unknown escape sequence");
							}
					}
					else {
						if (slen == rlen && !strncmp((const char *)schr,
								(const char *)rchr, rlen)) {
							parserutils_inputstream_advance(body_parser.stream, slen);
							parserutils_inputstream_advance(body_parser.regexp, rlen);
						}
						else {
							if (body_parser.regexp->cursor == 0) {
								parserutils_inputstream_advance(body_parser.stream,
									slen);
							}
							else {
								body_parser.regexp->cursor = 0;
								goto normal_state;
							}
						}
						break;
					}
				}
				else {
					body_parser.found = 1;
					return;
				}
			case PS_DIGITS:
				if (slen == 1 && *schr <= '9' && *schr >= '0') {
					body_parser.got_digit = 1;
					parserutils_inputstream_advance(body_parser.stream, 1);
				}
				else {
					body_parser.pstate = PS_NORMAL;
					if (body_parser.got_digit) {
						body_parser.got_digit = 0;
						goto normal_state;
					}
					else {
						parserutils_inputstream_advance(body_parser.stream, slen);
						body_parser.regexp->cursor = 0;
					}
				}
				break;
		}
}

static void fputc_html(int ch)
{
	// Delete or adjust these 2 arrays per code's goals
	// All simple-escape-sequence C11 6.4.4.4
	static const char *escapev = "\a\b\t\n\v\f\r\"\'\?\\";
	static const char *escapec = "abtnvfr\"\'\?\\";
	char *p = strchr(escapev, ch);
	if (p && *p) {
		if (*p == '\n' || *p == '\r' || *p == '\t' || *p == '\'' || *p == '\"')
			printf("%c", *p);
		else
			printf("\\%c", escapec[p - escapev]);
	} else if (isprint(ch))
		fputc(ch, stdout);
	else
		printf("\\%03o", ch); // Use octal as hex is problematic reading back
}

static void printf_html(const char *data, int length)
{
	while (length-- > 0)
		fputc_html((unsigned char) *data++);
}

static void process_http_chunk(connection *conn, const char *buf, size_t len)
{
	if ((int)len <= 0)
		return;

	if (common_config.debug_level >= DBG_BODY) {
		nxweb_dbg(DBG_BODY, conn, "========== BODY BEGIN ========== (%d bytes)\n", len);
		printf_html(buf, len);
		printf("\n");
		nxweb_dbg(DBG_BODY, conn, "=========== BODY END ===========\n");
	}

	if (body_parser.enabled && !body_parser.found)
		parse_body_chunk(buf, len);
}

/****************************************************************************************
 * libev callbacks
 ****************************************************************************************/
static void __write_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	connection *conn = ((connection*)(((char*)w) - offsetof(connection, watch_write)));

	if (conn->state == C_CONNECTING) {
		conn->last_activity = ev_now(loop);
		conn->state = conn->secure? C_HANDSHAKING : C_WRITING;
	}

#ifdef WITH_SSL
	if (conn->state == C_HANDSHAKING) {
		conn->last_activity = ev_now(loop);
		int ret = gnutls_handshake(conn->session);
		if (ret == GNUTLS_E_SUCCESS) {
			retrieve_ssl_session_info(conn);
			conn->state = C_WRITING;
			// fall through to C_WRITING
		}
		else if (ret == GNUTLS_E_AGAIN || !gnutls_error_is_fatal(ret)) {
			if (ret != GNUTLS_E_AGAIN)
				nxweb_log_error(conn, "gnutls handshake non-fatal error [%d] %s",
					ret, gnutls_strerror(ret));
			if (!gnutls_record_get_direction(conn->session)) {
				conn_io_stop(conn, EV_WRITE);
				conn_io_start(conn, EV_READ);
			}
			return;
		}
		else {
			nxweb_log_error(conn, "gnutls handshake error [%d] %s", ret, gnutls_strerror(ret));
			conn_close(conn, 0);
			inc_fail(conn);
			open_socket(conn);
			return;
		}
	}
#endif // WITH_SSL

	if (conn->state == C_WRITING) {
		struct sized_buffer {
			char* buffer;
			size_t size;
		};

		struct sized_buffer buffers[] = {
			{config[conn->idx].request_headers, config[conn->idx].request_headers_length},
			{conn->cookies, conn->cookie_len},
			{REQ_DELIM, REQ_DELIM_LEN},
			{config[conn->idx].request_body, config[conn->idx].request_body_length}
		};
		const size_t n_buffers = sizeof(buffers) / sizeof(buffers[0]);
		size_t total_size = 0;
		int i = 0;
		for (i = 0; i < n_buffers; i++) {
			total_size += buffers[i].size;
		}

		if (conn->write_pos < total_size) {
			if (!conn->write_pos) {
				if (conn_get(conn)) {
					conn_throttle(conn);
					return;
				}
				http_before_request_send(loop, conn);
			}

			char* buf = NULL;
			size_t buf_size = 0;

			size_t buffer_start = 0;
			int i = 0;
			for (i = 0; i < n_buffers; i++) {
				size_t offset = conn->write_pos - buffer_start;
				if (offset < buffers[i].size) {
					buf = buffers[i].buffer + offset;
					buf_size = buffers[i].size - offset;
					break;
				}
				buffer_start += buffers[i].size;
			}
			int bytes_sent = conn_write(conn, buf, buf_size);

			if (bytes_sent < 0) {
				if (bytes_sent != ERR_AGAIN) {
					fprintf(stderr, "%s", strerror_r(errno, conn->buf, sizeof(conn->buf)));
					nxweb_log_error(conn, "conn_write() returned %d: %d %s",
						bytes_sent, errno, conn->buf);
					conn_close(conn, 0);
					inc_fail(conn);
					open_socket(conn);
					return;
				}
				if (!conn->write_pos)
					conn_put(conn, 1);

				return;
			}

			if (bytes_sent)
				conn->last_activity = ev_now(loop);

			conn->write_pos += bytes_sent;

			if (conn->write_pos == total_size) {
				http_after_request_send(loop, conn);
			}
		}

		if (conn->write_pos == total_size) {
			conn->tdata->request_counter++;
			conn->state = C_READING_HEADERS;
			conn->read_pos = 0;
			conn_io_stop(conn, EV_WRITE);
			//ev_io_set(&conn->watch_read, conn->fd, EV_READ);
			conn_io_start(conn, EV_READ);
			ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
		}
	}
}

static void write_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	connection *conn = ((connection*)(((char*)w) - offsetof(connection, watch_write)));

	nxweb_dbg(DBG_DEBUG, conn, "write_cb enter\n");
	__write_cb(loop, w, revents);
	nxweb_dbg(DBG_DEBUG, conn, "write_cb exit\n");
}

static void __read_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	connection *conn = ((connection*)(((char*)w) - offsetof(connection, watch_read)));

#ifdef WITH_SSL
	if (conn->state == C_HANDSHAKING) {
		conn->last_activity = ev_now(loop);
		int ret = gnutls_handshake(conn->session);
		if (ret == GNUTLS_E_SUCCESS) {
			retrieve_ssl_session_info(conn);
			conn->state = C_WRITING;
			conn_io_stop(conn, EV_READ);
			conn_io_start(conn, EV_WRITE);
			return;
		}
		else if (ret == GNUTLS_E_AGAIN || !gnutls_error_is_fatal(ret)) {
			if (ret != GNUTLS_E_AGAIN)
				nxweb_log_error(conn, "gnutls handshake non-fatal error [%d] %s",
					ret, gnutls_strerror(ret));
			if (gnutls_record_get_direction(conn->session)) {
				conn_io_stop(conn, EV_READ);
				conn_io_start(conn, EV_WRITE);
			}
			return;
		}
		else {
			nxweb_log_error(conn, "gnutls handshake error [%d] %s", ret, gnutls_strerror(ret));
			conn_close(conn, 0);
			inc_fail(conn);
			open_socket(conn);
			return;
		}
	}
#endif // WITH_SSL

	if (conn->state == C_READING_HEADERS) {
		int room_avail, bytes_received;
		do {
			room_avail = sizeof(conn->buf) - conn->read_pos - 1;
			if (!room_avail) {
				// headers too long
				nxweb_log_error(conn, "response headers too long");
				conn_close(conn, 0);
				inc_fail(conn);
				open_socket(conn);
				return;
			}
			bytes_received = conn_read(conn, conn->buf + conn->read_pos, room_avail);
			if (bytes_received <= 0) {
				if (bytes_received == ERR_AGAIN)
					return;
				if (bytes_received == ERR_RDCLOSED) {
					conn_close(conn, 0);
					inc_fail(conn);
					open_socket(conn);
					return;
				}
				fprintf(stderr, "%s", strerror_r(errno, conn->buf, sizeof(conn->buf)));
				nxweb_log_error(conn, "headers [%d] conn_read() returned %d error: %d %s",
					conn->alive_count, bytes_received, errno, conn->buf);
				conn_close(conn, 0);
				inc_fail(conn);
				open_socket(conn);
				return;
			}
			conn->last_activity = ev_now(loop);
			conn->read_pos += bytes_received;
			//conn->buf[conn->read_pos]='\0';
			if (find_end_of_http_headers(conn->buf, conn->read_pos, &conn->body_ptr)) {
				parse_headers(conn);
				if (conn->bytes_to_read < 0 && !conn->chunked) {
					nxweb_log_error(conn, "response length unknown");
					conn_close(conn, 0);
					inc_fail(conn);
					open_socket(conn);
					return;
				}
				if (!conn->bytes_to_read) { // empty body
					rearm_socket(conn);
					return;
				}

				conn->state = C_READING_BODY;
				if (!conn->chunked) {
					if (conn->bytes_received >= conn->bytes_to_read) {
						rearm_socket(conn);
						return;
					}
				}
				else {
					int r = decode_chunked_stream(&conn->cdstate, conn->body_ptr,
						&conn->bytes_received);
					if (r < 0) {
						nxweb_log_error(conn, "chunked encoding error");
						conn_close(conn, 0);
						inc_fail(conn);
						open_socket(conn);
						return;
					} else if (r > 0) {
						rearm_socket(conn);
						return;
					}
				}
				ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
				return;
			}
		} while (bytes_received == room_avail);
		return;
	}

	if (conn->state == C_READING_BODY) {
		int room_avail, bytes_received, bytes_received2, r;
		conn->last_activity = ev_now(loop);
		do {
			room_avail = sizeof(conn->buf);
			if (conn->bytes_to_read > 0) {
				int bytes_left = conn->bytes_to_read - conn->bytes_received;
				if (bytes_left < room_avail)
					room_avail = bytes_left;
			}
			bytes_received = conn_read(conn, conn->buf, room_avail);
			if (bytes_received <= 0) {
				if (bytes_received == ERR_AGAIN)
					return;
				if (bytes_received == ERR_RDCLOSED) {
					nxweb_log_error(conn, "body [%d] read connection closed", conn->alive_count);
					conn_close(conn, 0);
					inc_fail(conn);
					open_socket(conn);
					return;
				}
				fprintf(stderr, "%s", strerror_r(errno, conn->buf, sizeof(conn->buf)));
				nxweb_log_error(conn, "body [%d] conn_read() returned %d error: %d %s",
					conn->alive_count, bytes_received, errno, conn->buf);
				conn_close(conn, 0);
				inc_fail(conn);
				open_socket(conn);
				return;
			}

			if (!conn->chunked) {
				conn->bytes_received += bytes_received;
				if (conn->bytes_received >= conn->bytes_to_read) {
					rearm_socket(conn);
					return;
				}
			}
			else {
				bytes_received2 = bytes_received;
				r = decode_chunked_stream(&conn->cdstate, conn->buf, &bytes_received2);
				if (r < 0) {
					nxweb_log_error(conn, "chunked encoding error after %d bytes received",
						conn->bytes_received);
					conn_close(conn, 0);
					inc_fail(conn);
					open_socket(conn);
					return;
				} else if (r > 0) {
					conn->bytes_received += bytes_received2;
					rearm_socket(conn);
					return;
				}
			}

		} while (bytes_received == room_avail);

		nxweb_dbg(DBG_DEBUG, conn, "%2d ... rcv GET %-20s (%d bytes) ...\n",
			atomic_get(&reqs_in_flight),
			config[conn->idx].url, bytes_received);

		return;
	}

	if (conn->state == C_THROTTLED) {
		char buf[1024];

		memset(buf, 0, sizeof(buf));
		int bytes_received = conn_read(conn, buf, sizeof(buf) - 1);
		if (bytes_received > 0)
			nxweb_log_error(conn, "read %d bytes in throttled state:\n%s\n", bytes_received, buf);

		if (bytes_received == ERR_RDCLOSED || bytes_received == ERR_ERROR) {

			switch(bytes_received) {
			case ERR_RDCLOSED:
				break;
			case ERR_ERROR:
				fprintf(stderr, "%s", strerror_r(errno, buf, sizeof(buf)));
				nxweb_log_error(conn, "body [%d] conn_read() returned %d error: %d %s",
					conn->alive_count, bytes_received, errno, buf);
				break;
			}
			conn_close(conn, 0);
			inc_fail(conn);
			open_socket(conn);
			return;
		}
		return;
	}
	nxweb_log_error(conn, "unhandled read, conn->state %d\n", conn->state);
}

static void read_cb(struct ev_loop *loop, ev_io *w, int revents)
{
	connection *conn = ((connection*)(((char*)w) - offsetof(connection, watch_read)));

	nxweb_dbg(DBG_DEBUG, conn, "read_cb enter\n");
	__read_cb(loop, w, revents);
	nxweb_dbg(DBG_DEBUG, conn, "read_cb exit\n");
}

static int need_stop()
{
	if ((common_config.press_mode == PM_NUMBER && common_config.request_counter > common_config.num_requests) ||
			(common_config.press_mode == PM_TIME && common_config.need_to_stop))
		return 1;
	else
		return 0;
}

static void shutdown_thread(thread_config* tdata);

static void heartbeat_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
	thread_config *tdata = ((thread_config*)(((char*)w) - offsetof(thread_config, watch_heartbeat)));

	if (need_stop()) {
		if (!tdata->shutdown_in_progress) {
			ev_tstamp now = ev_now(tdata->loop);
			tdata->avg_req_time = atomic_get(&tdata->stats.num_success) ? (now - tdata->start_time) *
				tdata->num_conn / atomic_get(&tdata->stats.num_success) : 0.1;
			if (tdata->avg_req_time > 1.)
				tdata->avg_req_time = 1.;
			tdata->shutdown_in_progress = 1;
		}
		shutdown_thread(tdata);
	}
}

static void resume_write_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
	connection *conn = ((connection*)(((char*)w) - offsetof(connection, watch_resume_write)));
	start_write(conn);
}

/****************************************************************************************
 * Threads management
 ****************************************************************************************/

static void shutdown_thread(thread_config* tdata)
{
	int i;
	connection* conn;
	ev_tstamp now = ev_now(tdata->loop);
	ev_tstamp time_limit = tdata->avg_req_time*4;
	//fprintf(stderr, "[%.6lf]", time_limit);
	for (i = 0; i < tdata->num_conn; i++) {
		conn = &tdata->conns[i];
		if (!conn->done) {
			if (ev_is_active(&conn->watch_read) || ev_is_active(&conn->watch_write)) {
				if ((now - conn->last_activity) > time_limit) {
					// kill this connection
					if (ev_is_active(&conn->watch_write))
						conn_io_stop(conn, EV_WRITE);
					if (ev_is_active(&conn->watch_read))
						conn_io_stop(conn, EV_READ);
					conn_close(conn, 0);
					inc_fail(conn);
					conn->done = 1;
					//fprintf(stderr, "*");
				}
				else {
					// don't kill this yet, but wake it up
					if (ev_is_active(&conn->watch_read))
						ev_feed_event(tdata->loop, &conn->watch_read, EV_READ);
					if (ev_is_active(&conn->watch_write))
						ev_feed_event(tdata->loop, &conn->watch_write, EV_WRITE);
					//fprintf(stderr, ".");
				}
			}
		}
	}
}

static void* thread_main(void* pdata)
{
	thread_config* tdata = (thread_config*)pdata;

	//  tdata->loop=ev_loop_new(0);
	//
	//  int i;
	//  connection* conn;
	//  for (i=0; i < tdata->stats.num_conn; i++) {
	//    conn=&tdata->conns[i];
	//    conn->tdata=tdata;
	//    conn->loop=tdata->loop;
	//    ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
	//    ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
	//    if (open_socket(conn)) return 0;
	//  }

	ev_timer_init(&tdata->watch_heartbeat, heartbeat_cb, 0.1, 0.1);
	ev_timer_start(tdata->loop, &tdata->watch_heartbeat);
	ev_unref(tdata->loop); // don't keep loop running just for heartbeat
	ev_run(tdata->loop, 0);

	ev_loop_destroy(tdata->loop);

	return 0;
}

/****************************************************************************************
 * Main
 ****************************************************************************************/

enum request_method {GET, POST, PUT, DELETE};

static void make_request(struct config *conf, const char* method,
			 const char* extra_headers, const char *data)
{
	size_t data_lenght =
		snprintf(conf->request_headers, sizeof(conf->request_headers),
			 "%s %s HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "Connection: %s\r\n"
			 "%s",
			 method, conf->uri_path,
			 conf->uri_host,
			 common_config.keep_alive ? "keep-alive" : "close",
			 extra_headers
			);
	if (data_lenght >= sizeof(conf->request_headers))
		nxweb_die("unable to prepare request, request is too large");

	if (data) {
		conf->request_body = strdup(data);
		if (!conf->request_body)
			nxweb_die("can't allocate request data");
		conf->request_body_length = strlen(conf->request_body);
	} else {
		conf->request_body = NULL;
		conf->request_body_length = 0;
	}

	conf->request_headers_length = strlen(conf->request_headers);
}


static void make_GET_request(struct config *conf)
{
	make_request(conf, "GET", "", NULL);
}


static void make_POST_request(struct config *conf, const char *data)
{
	char extra_headers[100];
	size_t data_len = strlen(data);
	snprintf(extra_headers, sizeof(extra_headers),
		 "Content-Length: %zu\r\n",
		 data_len
		);

	make_request(conf, "POST", extra_headers, data);
}


static void make_PUT_request(struct config *conf, const char *data)
{
	char extra_headers[100];
	size_t data_len = strlen(data);
	snprintf(extra_headers, sizeof(extra_headers),
		 "Content-Length: %zu\r\n",
		 data_len
		);

	make_request(conf, "PUT", extra_headers, data);
}


static void make_DELETE_request(struct config *conf)
{
	make_request(conf, "DELETE", "", NULL);
}


static int resolve_host(struct addrinfo** saddr, const char *host_and_port, int idx)
{
	char* host = strdup(host_and_port);
	char* port = strchr(host, ':');
	if (port)
		*port++ = '\0';
	else
		port = config[idx].secure ? "443":"80";

	struct addrinfo hints, *res, *res_first, *res_last;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res_first))
		goto ERR1;

	// search for an ipv4 address, no ipv6 yet
	res_last = 0;
	for (res = res_first; res; res = res->ai_next) {
		if (res->ai_family == AF_INET)
			break;
		res_last = res;
	}

	if (!res)
		goto ERR2;
	if (res != res_first) {
		// unlink from list and free rest
		res_last->ai_next = res->ai_next;
		freeaddrinfo(res_first);
		res->ai_next = 0;
	}

	free(host);
	*saddr = res;
	return 0;

ERR2:
	freeaddrinfo(res_first);
ERR1:
	free(host);
	return -1;
}

static void show_help(void)
{
	printf( "httpress <options> <url>\n"
			"  -d lvl    debug level (max is 3) (default: 0)\n"
			"  -n num    number of requests     (default: no)\n"
			"  -t sec    number of seconds to run, -n option has higher priority (default: 10)\n"
			"  -i ipaddr makes the program to send request without resolving, using specified IP address (default: no)\n"
			"  -p num    number of threads      (default: 1)\n"
			"  -c num    concurrent connections (default: 1)\n"
			"  -k        keep alive             (default: no)\n"
			"  -s        save cookies           (default: no)\n"
			"  -q        no progress indication (default: no)\n"
			"  -x regexp look for regular expression, example:\n"
			"            \"Page \\d\" - will look for expressions like \"Page 1\", \"Page 2\", etc\n"
			"  -z pri    GNUTLS cipher priority (default: NORMAL)\n"
			"  -h        show this help\n"
			"  -r range  range of urls, {} inside of url will be replaced with numbers from the range (default: no)\n"
			"  -e        account all HTTP responses (including non-2xx) when calculate final rate\n"
			"  -V        show version\n"
			"  -m        method                 (default: GET)\n"
			"  -l        payload for POST and PUT (default: "")\n"
			"  -R num    max frequency of request issuing in req/sec (default: inf)\n"
			"  -P num    percentile of request time	(1 to 99)(default: 95)\n"
			"  -I sec    print intermediate results every 'sec' seconds (default: 0)\n"
			"\n"
			"examples: httpress -n 10000 -c 100 -p 4 -k http://localhost:8080/index.html\n"
			"          httpress -n 10000 -c 100 -p 4 -k http://domain{}.localdomain/index.html -r 1-16\n\n");
}

static struct sigaction on_time_limit_achieved;
static struct sigaction on_inter_timer_expired;

static inline void time_limit_achieved_handler(int sig_num)
{
	common_config.need_to_stop = 1;
}

struct inter_res_data {
	ev_tstamp last_ts;
	thread_config **tdata;
};

static inline void inter_timer_expired_handler(int sig, siginfo_t *si, void *uc)
{
	struct inter_res_data *d = (struct inter_res_data *)si->si_value.sival_ptr;
	struct thread_config *t;
	atomic_val_t this_success = 0, this_fail = 0, total_success = 0, total_fail = 0;
	ev_tstamp now = ev_time();
	int i = 0;

	for (i = 0; i < common_config.num_threads; i++) {
		atomic_val_t success, fail;
		t = d->tdata[i];

		success = atomic_get(&t->stats.num_success);
		fail = atomic_get(&t->stats.num_fail);

		this_success += (success - atomic_get(&t->stats.num_success_prev));
		atomic_set(&t->stats.num_success_prev, success);
		this_fail += (fail - atomic_get(&t->stats.num_fail_prev));
		atomic_set(&t->stats.num_fail_prev, fail);

		total_success += success;
		total_fail += fail;
	}


	double prc = get_tmp_percentile(common_config.percentile) * 1000;	// from sec to msec
	fprintf(stdout, "total-loops: %llu; total-failed: %llu; max-latency: %.2f; loops: %llu; failed: %llu; time: %.1f; rate: { %.1f } req/sec;\n",
			total_success + total_fail, total_fail, prc, this_success + this_fail, this_fail, now - d->last_ts,
			(double)this_success / (now - d->last_ts));
	fflush(stdout);
	d->last_ts = now;
}

#define ADD(x)	atomic_add(&total->x, atomic_get(&tdata->stats.x))

static void calc_total(struct thread_config **threads, struct req_stats *total)
{
	thread_config* tdata;

	memset(total, 0, sizeof(struct req_stats));
	int i = 0;
	for (i = 0; i < common_config.num_threads; i++) {
		tdata = threads[i];

		ADD(num_success);
		ADD(num_2xx);
		ADD(num_fail);
		ADD(num_bytes_received);
		ADD(num_overhead_received);
		ADD(num_connect);
	}
}

static void print_stats(struct thread_config **threads, struct req_stats *total, double duration)
{
	thread_config* tdata;
	int i, j;

	if (common_config.quiet)
		return;

	print_stats_header();

	for (i = 0; i < common_config.num_threads; i++) {
		tdata = threads[i];
		for (j = 0; j < tdata->num_conn; j++) {
			char str[80];
			sprintf(str, "URL#%03d", tdata->conns[j].idx);
			print_stats_row(str, &tdata->conns[j].stats);
		}
	}

	if (common_config.num_threads > 1) {
		print_stats_sep();
		for (i = 0; i < common_config.num_threads; i++) {
			tdata = threads[i];

			char str[80];
			sprintf(str, "thr#%03d", tdata->id);
			print_stats_row(str, &tdata->stats);
		}
	}

	print_stats_sep();
	print_stats_row("TOTALS", total);

	if (atomic_get(&total->num_success) || atomic_get(&total->num_fail))
		printf("\nMax concurrency: %d, avg concurrency: %.1f\n", concurrency_max,
			atomic_get(&concurrency_sum) /
			(atomic_get(&total->num_success) + atomic_get(&total->num_fail) * 1.0));

	if (atomic_get(&stat_codes.counter) != atomic_get(stat_codes.codes[1].counter)) {
		printf("\n");
		for (i = 0; i < CLASS_NUM; i++)
			for (j = 0; j < stat_codes.codes[i].end - stat_codes.codes[i].begin + 1; j++) {
				long long v;
				v = atomic_get(stat_codes.codes[i].counter + j);
				if (v)
					printf("%d - %10llu  %5.1f%%\n",
						stat_codes.codes[i].begin + j,
						v, v * 100.0 / atomic_get(&stat_codes.counter));
			}

		if (atomic_get(&stat_codes.out_of_class))
			printf("unassigned - %llu  %.1f%%\n", atomic_get(&stat_codes.out_of_class),
				atomic_get(&stat_codes.out_of_class) * 100.0 / atomic_get(&stat_codes.counter));
	}

	printf("\nTRAFFIC: %lld avg bytes, %lld avg overhead, %lld bytes, %lld overhead\n",
		atomic_get(&total->num_success) ?
			atomic_get(&total->num_bytes_received) / atomic_get(&total->num_success) : 0L,
		atomic_get(&total->num_success) ?
			atomic_get(&total->num_overhead_received) / atomic_get(&total->num_success) : 0L,
		atomic_get(&total->num_bytes_received),
		atomic_get(&total->num_overhead_received));

	if (duration && atomic_get(&total->num_success)) {
		int rps = atomic_get(&total->num_success) / duration;
		int kbps = (atomic_get(&total->num_bytes_received) + atomic_get(&total->num_overhead_received)) /
			duration / 1024;

		ev_tstamp avg_req_time = atomic_get(&total->num_success) ?
				duration * common_config.num_connections /
				atomic_get(&total->num_success) * 1000 : 0;
		printf("\nTIMING:  %.1f seconds, %d rps, %d kbps, %.1f ms avg req time\n",
			duration, rps, kbps, avg_req_time);
	}

	printf("\n");
}

int main(int argc, char* argv[])
{
	common_config.num_connections = 1;
	common_config.num_threads = 1;
	common_config.num_requests = 0;
	common_config.keep_alive = 0;
	common_config.save_cookies = 0;
	common_config.quiet = 0;
	common_config.secure = 0;
	common_config.range = 0;
	common_config.request_counter = 0;
	common_config.ssl_cipher_priority = "NORMAL"; // NORMAL:-CIPHER-ALL:+AES-256-CBC:-VERS-TLS-ALL:+VERS-TLS1.0:-KX-ALL:+DHE-RSA
	common_config.need_to_stop = 0;
	common_config.press_mode = PM_TIME;
	common_config.sleep_time = 0.0;
	body_parser.enabled = 0;
	common_config.percentile = 95;
	lock_tmp_percentile();

	int idx = 0;
	for (idx = 0; idx < MAX_DOMAINS_NUMBER; ++idx) {
		config[idx].uri_path = 0;
		config[idx].uri_host = 0;
	}

	memset(&stat_codes, 0, sizeof(stat_codes));

	stat_codes.codes[0].begin = 100;
	stat_codes.codes[0].end   = 102;
	stat_codes.codes[1].begin = 200;
	stat_codes.codes[1].end   = 226;
	stat_codes.codes[2].begin = 300;
	stat_codes.codes[2].end   = 308;
	stat_codes.codes[3].begin = 400;
	stat_codes.codes[3].end   = 431;
	stat_codes.codes[4].begin = 500;
	stat_codes.codes[4].end   = 511;

	int i = 0;
	for (i = 0; i < CLASS_NUM; i++) {
		stat_codes.codes[i].counter =
			calloc(stat_codes.codes[i].end - stat_codes.codes[i].begin + 1, sizeof(atomic_t));
		if (!stat_codes.codes[i].counter)
			nxweb_die("can't allocate memory");
	}

	int c;
	unsigned int time_limit = 0;
	unsigned int inter_res = 0;
	const char *i_opt_ip4_addr = NULL;
	enum request_method method = GET;
	const char *payload = "";
	timer_t inter_timer;
	struct sigevent sev;

	while ((c = getopt(argc, argv, ":hVksql:m:n:p:c:z:r:d:t:i:x:e:R:P:I:")) != -1) {
		switch (c) {
			case 'h':
				show_help();
				return 0;
			case 'V':
				printf("version:    " VERSION "\n");
				printf("build-date: " __DATE__ " " __TIME__ "\n\n");
				return 0;
			case 'k':
				common_config.keep_alive = 1;
				break;
			case 's':
				common_config.save_cookies = 1;
				break;
			case 'q':
				common_config.quiet = 1;
				break;
			case 'l':
				payload = optarg;
				break;
			case 'm':
				if (strcmp(optarg, "GET") == 0)
					method = GET;
			        else if (strcmp(optarg, "POST") == 0)
				        method = POST;
				else if (strcmp(optarg, "PUT") == 0)
					method = PUT;
				else if (strcmp(optarg, "DELETE") == 0)
					method = DELETE;
				else
					nxweb_die("-m option value (method) must be one of GET, POST.");
				break;
			case 'n':
				common_config.num_requests = atoi(optarg);
				break;
			case 'p':
				common_config.num_threads = atoi(optarg);
				break;
			case 'c':
				common_config.concurrency = atoi(optarg);
				if (!common_config.concurrency)
					nxweb_die("-c option value (concurrency) must be > 0");
				common_config.num_connections = common_config.concurrency;
				break;
			case 'z':
				common_config.ssl_cipher_priority = optarg;
				break;
			case 'r':
				common_config.range = 1;
				int ret = sscanf(optarg, "%d-%d", &common_config.range_start, &common_config.range_end);
				if (ret != 2) {
					fprintf(stderr, "ERROR: wrong arguments passed to the '-r' option\n"
									"       correct format should be like: '-r 1-16'\n");
					show_help();
					return EXIT_FAILURE;
				}
				break;
			case 'd':
				common_config.debug_level = atoi(optarg);
				break;
			case 't':
				time_limit = atoi(optarg);
				break;
			case 'i':
				i_opt_ip4_addr = strdup(optarg);
				break;
			case 'x':
				set_body_parser(optarg);
				break;
			case 'e':
				common_config.include_non2xx = 1;
				break;
			case 'R':
				printf("Restricted request frequency mode: ");
				int max_req_freq = atoi(optarg);
				if (max_req_freq <= 0)
					nxweb_die("-R option value (request frequency) must be > 0");
				common_config.sleep_time = 1.0 / max_req_freq * (double) common_config.num_threads;
				printf("max %d req/sec\n", max_req_freq);
				break;
			case 'P':
				common_config.percentile = atoi(optarg);
				if(!(common_config.percentile > 1 && common_config.percentile < 100))
					nxweb_die("percentile value should be between 1 and 99");
				break;
			case 'I':
				inter_res = atoi(optarg);
				unlock_tmp_percentile();
				break;
			case '?':
				fprintf(stderr, "unkown option: -%c\n\n", optopt);
				show_help();
				return EXIT_FAILURE;
		}
	}

	if (common_config.num_requests) {
		common_config.press_mode = PM_NUMBER;
		if (time_limit)
			printf("WARNING: -t option is ignored\n");
	}
	else {
		if (time_limit < 1 || time_limit > 100000) {
			printf("Test time is set to 10 seconds.\n");
			time_limit = 10;
		}
		on_time_limit_achieved.sa_handler = time_limit_achieved_handler;
		on_time_limit_achieved.sa_flags = 0;
		if (sigaction(SIGALRM, &on_time_limit_achieved, NULL) == -1)
			nxweb_die("can't set global timer");
		alarm(time_limit);
	}

	if (!common_config.concurrency) {
		common_config.concurrency = 1;
		common_config.num_connections = common_config.concurrency;
	}

	int url_number = 0;
	url_number = argc - optind;

	if (url_number < 1) {
		fprintf(stderr, "missing url argument\n\n");
		show_help();
		return EXIT_FAILURE;
	}

	if (common_config.range)
		common_config.tot_domains_number =
				(common_config.range_end
				- common_config.range_start + 1)
				* url_number;
	else
		common_config.tot_domains_number = url_number;

	if (common_config.tot_domains_number > MAX_DOMAINS_NUMBER) {
		fprintf(stderr, "too big number of URLs specified (%d), the max possible number is %d\n\n.",
				common_config.tot_domains_number, MAX_DOMAINS_NUMBER);
			return EXIT_FAILURE;
	}

	if(common_config.tot_domains_number > common_config.concurrency) {
		common_config.concurrency = common_config.tot_domains_number;
		common_config.num_connections = common_config.tot_domains_number;
		fprintf(stdout, "Concurency number has been set to %d due to the number of URLs given.\n",
			common_config.concurrency);
	}

	if (common_config.press_mode == PM_NUMBER && (common_config.num_requests < 1 || common_config.num_requests > 1000000000))
		nxweb_die("wrong number of requests");

	if (common_config.press_mode == PM_NUMBER && common_config.num_connections > common_config.num_requests)
		common_config.num_requests = common_config.num_connections;

	if (common_config.num_connections < 1 || common_config.num_connections > 1000000)
		nxweb_die("wrong number of connections");

	if (common_config.num_threads < 1 || common_config.num_threads > 100000 ||
			common_config.num_threads > common_config.num_connections)
		nxweb_die("wrong number of threads");

	common_config.thread_concurrency_limit = common_config.concurrency / common_config.num_threads;
	if (!common_config.thread_concurrency_limit)
		nxweb_die("concurrency (%d) mustn't be less than threads number (%d)",
			common_config.concurrency, common_config.num_threads);

	common_config.progress_step = common_config.num_requests / 4;
	if (common_config.progress_step > 50000)
		common_config.progress_step = 50000;

	if (!common_config.range) {
		for (i = optind, idx = 0; i < argc; ++i, ++idx)
			if (parse_uri(argv[i], idx))
				nxweb_die("can't parse url: %s", argv[i]);
	} else {
		static char url_buf1[1024];
		static char url_buf2[1024];
		int config_idx = 0;

		int url_idx = 0;
		for (url_idx = 0; url_idx < url_number; url_idx++) {
			memset(url_buf1, 0, sizeof(url_buf1));
			memset(url_buf2, 0, sizeof(url_buf2));
			char* current_url = argv[optind + url_idx];
			char* from1 = current_url;
			char* to1 = strchr(current_url, '{');
			if (!to1 || (to1 - from1) > sizeof(url_buf1) - 1) {
				fprintf(stderr, "range option requires {} inside of url\n\n");
				return -1;
			}

			strncpy(url_buf1, from1, to1 - from1);
			url_buf1[to1 - from1] = '\0';
			char* from2 = strchr(current_url, '}') + 1;
			char* to2 = current_url + strlen(current_url);
			if (!to2 || (to2 - from2) > sizeof(url_buf2) - 1) {
				fprintf(stderr, "range option requires {} inside of url\n\n");
				return -1;
			}
			strncpy(url_buf2, from2, to2 - from2);
			url_buf2[to2 - from2] = '\0';

			for (i = common_config.range_start; i <= common_config.range_end; ++i) {
				char url_construct_buf[sizeof(url_buf1) +
						sizeof(url_buf2) + 10];
				snprintf(url_construct_buf, sizeof(url_construct_buf),
					"%s%d%s", url_buf1, i, url_buf2);
				printf("URL#%03d: %s\n", config_idx, url_construct_buf);

				if (parse_uri(url_construct_buf, config_idx))
					nxweb_die("can't parse url: %s", url_construct_buf);

				config_idx++;
			}
		}
	}

#ifdef WITH_SSL
	for (idx = 0; idx < common_config.tot_domains_number; ++idx) {
		if (config[idx].secure) {
			if (!common_config.secure) {
				if (common_config.num_threads > 1)
					nxweb_die("FIXME: can't do SSL in threaded mode");
				gnutls_global_init();
				common_config.secure = 1;
			}
			gnutls_certificate_allocate_credentials(&config[idx].ssl_cred);
			int ret = gnutls_priority_init(&config[idx].priority_cache,
				common_config.ssl_cipher_priority, 0);
			if (ret) {
				fprintf(stderr, "invalid priority string: %s\n\n", common_config.ssl_cipher_priority);
				return EXIT_FAILURE;
			}
		}
	}
#endif // WITH_SSL

	// Block signals for all threads
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGPIPE);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGHUP);
	if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
		nxweb_log_error(NULL, "can't set pthread_sigmask");
		exit(EXIT_FAILURE);
	}

	if (i_opt_ip4_addr) {
		const char *prt;
		int port;
		struct sockaddr_in *addr;
		struct addrinfo ainf;

		ainf.ai_family = AF_INET;
		ainf.ai_flags = 0;
		ainf.ai_protocol = 0;
		ainf.ai_canonname = NULL;
		ainf.ai_socktype = SOCK_STREAM;
		ainf.ai_addrlen = sizeof(struct sockaddr_in);
		ainf.ai_next = NULL;
		ainf.ai_addr = NULL;

		for (idx = 0; idx < common_config.tot_domains_number; ++idx) {
			prt = strchr(config[idx].uri_host, ':');
			if (prt)
				port = atoi(++prt);
			else
				port = config[idx].secure ? 443:80;
			addr = malloc(sizeof(struct sockaddr_in));
			if (!addr)
				nxweb_die("can't allocate memory");
			addr->sin_family = AF_INET;
			addr->sin_port = htons(port);

			if (!inet_pton(AF_INET, i_opt_ip4_addr, &addr->sin_addr))
				nxweb_die("wrong ip address");

			config[idx].saddr = malloc(sizeof(struct addrinfo));
			if (!config[idx].saddr)
				nxweb_die("can't allocate memory");
			memcpy(config[idx].saddr, &ainf, sizeof(struct addrinfo));
			config[idx].saddr->ai_addr = (struct sockaddr*)addr;
		}
	}
	else {
		for (idx = 0; idx < common_config.tot_domains_number; ++idx)
			if (resolve_host(&config[idx].saddr, config[idx].uri_host, idx)) {
				nxweb_log_error(NULL, "can't resolve host %s", config[idx].uri_host);
				exit(EXIT_FAILURE);
			}
	}

	for (idx = 0; idx < common_config.tot_domains_number; ++idx) {
		switch (method) {
		case GET:
			make_GET_request(&config[idx]);
			break;
		case POST:
			make_POST_request(&config[idx], payload);
			break;
		case PUT:
			make_PUT_request(&config[idx], payload);
			break;
		case DELETE:
			make_DELETE_request(&config[idx]);
			break;
		}

	}

	thread_config** threads = calloc(common_config.num_threads, sizeof(thread_config*));

	if (!threads)
		nxweb_die("can't allocate thread pool");

	struct inter_res_data idata;
	if (inter_res) {
		if (inter_res < 1 || inter_res > 100000) {
			printf("Intermediate results timer is set to 5 seconds.\n");
			time_limit = 5;
		}
		on_inter_timer_expired.sa_flags = SA_SIGINFO;
		on_inter_timer_expired.sa_sigaction = inter_timer_expired_handler;
		if (sigaction(SIGRTMIN, &on_inter_timer_expired, NULL) == -1)
			nxweb_die("can't set handler for intermediate results timer");

		idata.last_ts = ev_time();
		idata.tdata = threads;

		sev.sigev_notify = SIGEV_SIGNAL;
		sev.sigev_signo = SIGRTMIN;
		sev.sigev_value.sival_ptr = &idata;
		if (timer_create(CLOCK_REALTIME, &sev, &inter_timer) == -1)
			nxweb_die("can't create intermediate results timer");

		struct itimerspec its;
		its.it_value.tv_sec = its.it_interval.tv_sec = inter_res;
		its.it_value.tv_nsec = its.it_interval.tv_nsec = 0;
		if (timer_settime(inter_timer, 0, &its, NULL) == -1)
			nxweb_die("can't start intermediate results timer");
	}

	ev_tstamp start_ts = ev_time();
	int j;
	int conns_allocated = 0;
	thread_config* tdata;
	int cur_domain_idx = 0;
	for (i = 0; i < common_config.num_threads; i++) {
		threads[i] = tdata = memalign(MEM_GUARD, sizeof(thread_config) + MEM_GUARD);
		if (!tdata)
			nxweb_die("can't allocate thread data");
		memset(tdata, 0, sizeof(thread_config));
		tdata->id = i + 1;
		tdata->start_time = start_ts;
		tdata->num_conn = (common_config.num_connections - conns_allocated) / (common_config.num_threads - i);
		conns_allocated += tdata->num_conn;
		tdata->conns = memalign(MEM_GUARD, tdata->num_conn * sizeof(connection) + MEM_GUARD);
		if (!tdata->conns)
			nxweb_die("can't allocate thread connection pool");
		memset(tdata->conns, 0, tdata->num_conn*sizeof(connection));
		tdata->loop = ev_loop_new(0);
		cd_list_init(&tdata->throttled_conns);

		connection* conn;
		for (j = 0; j < tdata->num_conn; j++) {
			conn = &tdata->conns[j];
			conn->tdata = tdata;
			conn->loop = tdata->loop;
			conn->idx = cur_domain_idx % common_config.tot_domains_number;
			conn->socket_id = cur_domain_idx;
			conn->secure = config[conn->idx].secure;
			ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
			ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
			open_socket(conn);
			cur_domain_idx++;
		}

		pthread_create(&tdata->tid, 0, thread_main, tdata);
	}

	// Unblock signals for the main thread;
	// other threads have inherited sigmask we set earlier
	sigdelset(&set, SIGPIPE); // except SIGPIPE
	if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) {
		nxweb_log_error(NULL, "can't unset pthread_sigmask");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < common_config.num_threads; i++)
		pthread_join(threads[i]->tid, 0);
	if (inter_res)
		timer_delete(inter_timer);

	ev_tstamp end_ts = ev_time();
	double duration =  end_ts - start_ts;

#ifdef WITH_SSL
	if (common_config.secure && !common_config.quiet) {
		for (i = 0; i < common_config.num_threads; i++) {
			tdata = threads[i];
			if (tdata->ssl_identified) {
				printf("\nSSL INFO: %s\n", gnutls_cipher_suite_get_name(tdata->ssl_kx,
					tdata->ssl_cipher, tdata->ssl_mac));
				printf ("- Protocol: %s\n", gnutls_protocol_get_name(tdata->ssl_protocol));
				printf ("- Key Exchange: %s\n", gnutls_kx_get_name(tdata->ssl_kx));
# ifdef GNUTLS3
				if (tdata->ssl_ecdh)
					printf ("- Ephemeral ECDH using curve %s\n", gnutls_ecc_curve_get_name(tdata->ssl_ecc_curve));
# endif
				if (tdata->ssl_dhe)
					printf ("- Ephemeral DH using prime of %d bits\n", tdata->ssl_dh_prime_bits);

				printf ("- Cipher: %s\n", gnutls_cipher_get_name(tdata->ssl_cipher));
				printf ("- MAC: %s\n", gnutls_mac_get_name(tdata->ssl_mac));
				printf ("- Compression: %s\n", gnutls_compression_get_name(tdata->ssl_compression));
				printf ("- Certificate Type: %s\n", gnutls_certificate_type_get_name(tdata->ssl_cert_type));

				if (tdata->ssl_cert) {
					gnutls_datum_t cinfo;
					if (!gnutls_x509_crt_print(tdata->ssl_cert, GNUTLS_CRT_PRINT_ONELINE, &cinfo)) {
						printf ("- Certificate Info: %s\n", cinfo.data);
						gnutls_free(cinfo.data);
					}
				}
				break;
			}
		}
	}
#endif // WITH_SSL

	struct req_stats total;

	calc_total(threads, &total);
	print_stats(threads, &total, duration);

	long long loops = atomic_get(&total.num_success) + atomic_get(&total.num_fail);
	long long failed = atomic_get(&total.num_fail);

	if (!common_config.include_non2xx)
		failed += atomic_get(&total.num_success) - atomic_get(&total.num_2xx);

	double prc = get_summary_percentile(common_config.percentile) * 1000;	// from sec to msec

	/* print results in perf-atomic format */
	printf("loops: %lld; failed: %lld; time: %.1f; percentile(%d%%): %.1f ms; rate: { %.1f } req/sec;\n",
		loops, failed, duration, common_config.percentile, prc, duration ? (loops - failed) / duration : 0.0);

	for (idx = 0; idx < common_config.tot_domains_number; ++idx)
		freeaddrinfo(config[idx].saddr);

	for (i = 0; i < common_config.num_threads; i++) {
		tdata = threads[i];
		if (tdata->conns->cookies)
			free(tdata->conns->cookies);
		free(tdata->conns);
#ifdef WITH_SSL
		if (tdata->ssl_cert)
			gnutls_x509_crt_deinit(tdata->ssl_cert);
#endif // WITH_SSL
		free(tdata);
	}
	free(threads);

#ifdef WITH_SSL
	if (common_config.secure) {
		for (idx = 0; idx < common_config.tot_domains_number; ++idx) {
			gnutls_certificate_free_credentials(config[idx].ssl_cred);
			gnutls_priority_deinit(config[idx].priority_cache);
		}
		gnutls_global_deinit();
	}
#endif // WITH_SSL

	return EXIT_SUCCESS;
}
