/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2017 pooler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <json-c/json.h>
#include <curl/curl.h>
#include <assert.h>
#include "compat.h"
#include "miner.h"
#include "../conf.h"

#define LP_SCANTIME		60

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void)
{
	struct sched_param param;
	param.sched_priority = 0;

#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpuset_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif
		
enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
};

enum algos {
	ALGO_SCRYPT,		/* scrypt(1024,1,1) */
	ALGO_SHA256D,		/* SHA-256d */
};

static const char *algo_names[] = {
	[ALGO_SCRYPT]		= "scrypt",
	[ALGO_SHA256D]		= "sha256d",
};

bool opt_debug = false;
bool opt_protocol = false;
static bool opt_benchmark = false;
bool opt_redirect = true;
bool want_longpoll = true;
bool have_longpoll = false;
bool have_gbt = true;
bool allow_getwork = true;
bool want_stratum = true;
bool have_stratum = true;
bool use_syslog = false;
static bool opt_background = false;
static bool opt_quiet = true;
static int opt_retries = -1;
static int opt_fail_pause = 30;
int opt_timeout = 0;
static int opt_scantime = 5;
static enum algos opt_algo = ALGO_SHA256D;
static int opt_scrypt_n = 1024;
static int opt_n_threads;
static int num_processors;
static char *rpc_url;
static char *rpc_userpass;
static char *rpc_user, *rpc_pass;
static int pk_script_size;
static unsigned char pk_script[25];
static char	coinbase_address[40];
static char coinbase_sig[101] = "";
char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
struct work_restart *work_restart = NULL;
static struct stratum_ctx stratum;

#if	0
pthread_mutex_t applog_lock;
#endif
static pthread_mutex_t stats_lock;

static unsigned long accepted_count = 0L;
static unsigned long rejected_count = 0L;
static double *thr_hashrates;

struct work {
	uint32_t data[32];
	uint32_t target[8];

	int height;
	char *txs;
	char *workid;

	char *job_id;
	size_t xnonce2_len;
	unsigned char *xnonce2;
};

static struct work g_work;
static time_t g_work_time;
static pthread_mutex_t g_work_lock;
static bool submit_old = false;
static char *lp_id;

static inline void work_free(struct work *w)
{
	free(w->txs);
	free(w->workid);
	free(w->job_id);
	free(w->xnonce2);
}

static inline void work_copy(struct work *dest, const struct work *src)
{
	memcpy(dest, src, sizeof(struct work));
	if (src->txs)
		dest->txs = strdup(src->txs);
	if (src->workid)
		dest->workid = strdup(src->workid);
	if (src->job_id)
		dest->job_id = strdup(src->job_id);
	if (src->xnonce2) {
		dest->xnonce2 = malloc(src->xnonce2_len);
		memcpy(dest->xnonce2, src->xnonce2, src->xnonce2_len);
	}
}

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp = NULL;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}

static bool work_decode(const json_t *val, struct work *work)
{
	int i;

	if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
		applog(LOG_ERR, "JSON invalid data");
		goto err_out;
	}
	if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto err_out;
	}

	for (i = 0; i < ARRAY_SIZE(work->data); i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[i] = le32dec(work->target + i);

	return true;

err_out:
	return false;
}

static bool gbt_work_decode(const json_t *val, struct work *work)
{
	int i, n;
	uint32_t version, curtime, bits;
	uint32_t prevhash[8];
	uint32_t target[8];
	int cbtx_size;
	unsigned char *cbtx = NULL;
	int tx_count, tx_size;
	unsigned char txc_vi[9];
	unsigned char (*merkle_tree)[32] = NULL;
	bool coinbase_append = false;
	bool submit_coinbase = false;
	bool segwit = false;
	json_t *tmp, *txa;
	bool rc = false;

	tmp = json_object_get(val, "rules");
	if (tmp && json_is_array(tmp)) {
		n = json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "segwit") || !strcmp(s, "!segwit"))
				segwit = true;
		}
	}

	tmp = json_object_get(val, "mutable");
	if (tmp && json_is_array(tmp)) {
		n = json_array_size(tmp);
		for (i = 0; i < n; i++) {
			const char *s = json_string_value(json_array_get(tmp, i));
			if (!s)
				continue;
			if (!strcmp(s, "coinbase/append"))
				coinbase_append = true;
			else if (!strcmp(s, "submit/coinbase"))
				submit_coinbase = true;
		}
	}

	tmp = json_object_get(val, "height");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid height");
		goto out;
	}
	work->height = json_integer_value(tmp);

	tmp = json_object_get(val, "version");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid version");
		goto out;
	}
	version = json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "previousblockhash", prevhash, sizeof(prevhash)))) {
		applog(LOG_ERR, "JSON invalid previousblockhash");
		goto out;
	}

	tmp = json_object_get(val, "curtime");
	if (!tmp || !json_is_integer(tmp)) {
		applog(LOG_ERR, "JSON invalid curtime");
		goto out;
	}
	curtime = json_integer_value(tmp);

	if (unlikely(!jobj_binary(val, "bits", &bits, sizeof(bits)))) {
		applog(LOG_ERR, "JSON invalid bits");
		goto out;
	}

	/* find count and size of transactions */
	txa = json_object_get(val, "transactions");
	if (!txa || !json_is_array(txa)) {
		applog(LOG_ERR, "JSON invalid transactions");
		goto out;
	}
	tx_count = json_array_size(txa);
	tx_size = 0;
	for (i = 0; i < tx_count; i++) {
		const json_t *tx = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tx, "data"));
		if (!tx_hex) {
			applog(LOG_ERR, "JSON invalid transactions");
			goto out;
		}
		tx_size += strlen(tx_hex) / 2;
	}

	/* build coinbase transaction */
	tmp = json_object_get(val, "coinbasetxn");
	if (tmp) {
		const char *cbtx_hex = json_string_value(json_object_get(tmp, "data"));
		cbtx_size = cbtx_hex ? strlen(cbtx_hex) / 2 : 0;
		cbtx = malloc(cbtx_size + 100);
		if (cbtx_size < 60 || !hex2bin(cbtx, cbtx_hex, cbtx_size)) {
			applog(LOG_ERR, "JSON invalid coinbasetxn");
			goto out;
		}
	} else {
		int64_t cbvalue;
		if (!pk_script_size) {
			if (allow_getwork) {
				applog(LOG_INFO, "No payout address provided, switching to getwork");
				have_gbt = false;
			} else
				applog(LOG_ERR, "No payout address provided");
			goto out;
		}
		tmp = json_object_get(val, "coinbasevalue");
		if (!tmp || !json_is_integer(tmp)) {
			applog(LOG_ERR, "JSON invalid coinbasevalue");
			goto out;
		}
		cbvalue = json_integer_value(tmp) ;
		cbtx = malloc(256);
		if (!cbtx)
			exit(-1);
		memset(cbtx, 0, 256);
		le32enc((uint32_t *)cbtx, 1); /* version */
		cbtx[4] = 1; /* in-counter */
		memset(cbtx+5, 0x00, 32); /* prev txout hash */
		le32enc((uint32_t *)(cbtx+37), 0xffffffff); /* prev txout index */
		cbtx_size = 43;
		/* BIP 34: height in coinbase */
		for (n = work->height; n; n >>= 8) {
			cbtx[cbtx_size++] = n & 0xff;
			if (n < 0x100 && n >= 0x80)
				cbtx[cbtx_size++] = 0;
		}
		cbtx[42] = cbtx_size - 43;
		cbtx[41] = cbtx_size - 42; /* scriptsig length */
		le32enc((uint32_t *)(cbtx+cbtx_size), 0xffffffff); /* sequence */
		cbtx_size += 4;
		cbtx[cbtx_size++] = segwit ? 2 : 1; /* out-counter */
		le32enc((uint32_t *)(cbtx+cbtx_size), (uint32_t)cbvalue); /* value */
		le32enc((uint32_t *)(cbtx+cbtx_size+4), cbvalue >> 32);
		cbtx_size += 8;
		cbtx[cbtx_size++] = pk_script_size; /* txout-script length */
		memcpy(cbtx+cbtx_size, pk_script, pk_script_size);
		cbtx_size += pk_script_size;
		if (segwit) {
			unsigned char (*wtree)[32] = calloc(tx_count + 2, 32);
			memset(cbtx+cbtx_size, 0, 8); /* value */
			cbtx_size += 8;
			cbtx[cbtx_size++] = 38; /* txout-script length */
			cbtx[cbtx_size++] = 0x6a; /* txout-script */
			cbtx[cbtx_size++] = 0x24;
			cbtx[cbtx_size++] = 0xaa;
			cbtx[cbtx_size++] = 0x21;
			cbtx[cbtx_size++] = 0xa9;
			cbtx[cbtx_size++] = 0xed;
			for (i = 0; i < tx_count; i++) {
				const json_t *tx = json_array_get(txa, i);
				const json_t *hash = json_object_get(tx, "hash");
				if (!hash || !hex2bin(wtree[1+i], json_string_value(hash), 32)) {
					applog(LOG_ERR, "JSON invalid transaction hash");
					free(wtree);
					goto out;
				}
				memrev(wtree[1+i], 32);
			}
			n = tx_count + 1;
			while (n > 1) {
				if (n % 2)
					memcpy(wtree[n], wtree[n-1], 32);
				n = (n + 1) / 2;
				for (i = 0; i < n; i++)
					sha256d(wtree[i], wtree[2*i], 64);
			}
			memset(wtree[1], 0, 32);  /* witness reserved value = 0 */
			sha256d(cbtx+cbtx_size, wtree[0], 64);
			cbtx_size += 32;
			free(wtree);
		}
		le32enc((uint32_t *)(cbtx+cbtx_size), 0); /* lock time */
		cbtx_size += 4;
		coinbase_append = true;
	}
	if (coinbase_append) {
		unsigned char xsig[100] = {0};
		int xsig_len = 0;
		if (*coinbase_sig) {
			n = strlen(coinbase_sig);
			if (cbtx[41] + xsig_len + n <= 100) {
				memcpy(xsig+xsig_len, coinbase_sig, n);
				xsig_len += n;
			} else {
				applog(LOG_WARNING, "Signature does not fit in coinbase, skipping");
			}
		}
		tmp = json_object_get(val, "coinbaseaux");
		if (tmp && json_is_object(tmp)) {
			struct json_object_iterator itrBegin = json_object_iter_begin(tmp);
 			struct json_object_iterator itrEnd = json_object_iter_end(tmp);
			while (!json_object_iter_equal(&itrBegin, &itrEnd)) {
				unsigned char buf[100] = {0};
				const char *s = json_string_value(json_object_iter_peek_value(&itrBegin));
				n = s ? strlen(s) / 2 : 0;
				if (!s || n > 100 || !hex2bin(buf, s, n)) {
					applog(LOG_ERR, "JSON invalid coinbaseaux");
					break;
				}
				if (cbtx[41] + xsig_len + n <= 100) {
					memcpy(xsig+xsig_len, buf, n);
					xsig_len += n;
				}
				json_object_iter_next(&itrBegin);
			}
		}

		if (xsig_len) {
			unsigned char *ssig_end = cbtx + 42 + cbtx[41];
			int push_len = cbtx[41] + xsig_len < 76 ? 1 :
			               cbtx[41] + 2 + xsig_len > 100 ? 0 : 2;
			n = xsig_len + push_len;
			memmove(ssig_end + n, ssig_end, cbtx_size - 42 - cbtx[41]);
			cbtx[41] += n;
			if (push_len == 2)
				*(ssig_end++) = 0x4c; /* OP_PUSHDATA1 */
			if (push_len)
				*(ssig_end++) = xsig_len;
			memcpy(ssig_end, xsig, xsig_len);
			cbtx_size += n;
		}
	}

	n = varint_encode(txc_vi, 1 + tx_count);
	work->txs = malloc(2 * (n + cbtx_size + tx_size) + 1);
	bin2hex(work->txs, txc_vi, n);
	bin2hex(work->txs + 2*n, cbtx, cbtx_size);

	/* generate merkle root */
	merkle_tree = malloc(32 * ((1 + tx_count + 1) & ~1));
	sha256d(merkle_tree[0], cbtx, cbtx_size);
	for (i = 0; i < tx_count; i++) {
		tmp = json_array_get(txa, i);
		const char *tx_hex = json_string_value(json_object_get(tmp, "data"));
		const int tx_size = tx_hex ? strlen(tx_hex) / 2 : 0;
		if (segwit) {
			const char *txid = json_string_value(json_object_get(tmp, "txid"));
			if (!txid || !hex2bin(merkle_tree[1 + i], txid, 32)) {
				applog(LOG_ERR, "JSON invalid transaction txid");
				goto out;
			}
			memrev(merkle_tree[1 + i], 32);
		} else {
			unsigned char *tx = malloc(tx_size);
			if (!tx_hex || !hex2bin(tx, tx_hex, tx_size)) {
				applog(LOG_ERR, "JSON invalid transactions");
				free(tx);
				goto out;
			}
			sha256d(merkle_tree[1 + i], tx, tx_size);
			free(tx);
		}
		if (!submit_coinbase)
			strcat(work->txs, tx_hex);
	}
	n = 1 + tx_count;
	while (n > 1) {
		if (n % 2) {
			memcpy(merkle_tree[n], merkle_tree[n-1], 32);
			++n;
		}
		n /= 2;
		for (i = 0; i < n; i++)
			sha256d(merkle_tree[i], merkle_tree[2*i], 64);
	}

	/* assemble block header */
	work->data[0] = swab32(version);
	for (i = 0; i < 8; i++)
		work->data[8 - i] = le32dec(prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_tree[0] + i);
	work->data[17] = swab32(curtime);
	work->data[18] = le32dec(&bits);
	memset(work->data + 19, 0x00, 52);
	work->data[20] = 0x80000000;
	work->data[31] = 0x00000280;

	if (unlikely(!jobj_binary(val, "target", target, sizeof(target)))) {
		applog(LOG_ERR, "JSON invalid target");
		goto out;
	}
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[7 - i] = be32dec(target + i);

	tmp = json_object_get(val, "workid");
	if (tmp) {
		if (!json_is_string(tmp)) {
			applog(LOG_ERR, "JSON invalid workid");
			goto out;
		}
		work->workid = strdup(json_string_value(tmp));
	}

	/* Long polling */
	tmp = json_object_get(val, "longpollid");
	if (want_longpoll && json_is_string(tmp)) {
		free(lp_id);
		lp_id = strdup(json_string_value(tmp));
		if (!have_longpoll) {
			char *lp_uri;
			tmp = json_object_get(val, "longpolluri");
			lp_uri = strdup(json_is_string(tmp) ? json_string_value(tmp) : rpc_url);
			have_longpoll = true;
			tq_push(thr_info[longpoll_thr_id].q, lp_uri);
		}
	}

	rc = true;

out:
	if (cbtx) free(cbtx);
	if (merkle_tree) free(merkle_tree);
	return rc;
}

static void share_result(int result, const char *reason)
{
	char s[345];
	double hashrate;
	int i;

	hashrate = 0.;
	pthread_mutex_lock(&stats_lock);
	for (i = 0; i < opt_n_threads; i++)
		hashrate += thr_hashrates[i];
	result ? accepted_count++ : rejected_count++;
	pthread_mutex_unlock(&stats_lock);
	
	sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
	applog(LOG_INFO, "accepted: %lu/%lu (%.2f%%), %s khash/s %s",
		   accepted_count,
		   accepted_count + rejected_count,
		   100. * accepted_count / (accepted_count + rejected_count),
		   s,
		   result ? "(yay!!!)" : "(booooo)");

	if (opt_debug && reason)
		applog(LOG_DEBUG, "DEBUG: reject reason: %s", reason);
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	json_t *val, *res, *reason;
	char data_str[2 * sizeof(work->data) + 1] = {0};
	char s[345] = {0};
	int i;
	bool rc = false;

	/* pass if the previous hash is not the current previous hash */
	if (!submit_old && memcmp(work->data + 1, g_work.data + 1, 32)) {
		if (opt_debug)
			applog(LOG_DEBUG, "DEBUG: stale work detected, discarding");
		return true;
	}

	if (have_stratum) {
		uint32_t ntime, nonce;
		char ntimestr[9], noncestr[9], *xnonce2str, *req;

		le32enc(&ntime, work->data[17]);
		le32enc(&nonce, work->data[19]);
		bin2hex(ntimestr, (const unsigned char *)(&ntime), 4);
		bin2hex(noncestr, (const unsigned char *)(&nonce), 4);
		xnonce2str = abin2hex(work->xnonce2, work->xnonce2_len);
		req = malloc(256 + strlen(rpc_user) + strlen(work->job_id) + 2 * work->xnonce2_len);
		sprintf(req,
			"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
			rpc_user, work->job_id, xnonce2str, ntimestr, noncestr);
		free(xnonce2str);
		
		rc = stratum_send_line(&stratum, req);
		free(req);
		if (unlikely(!rc)) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			goto out;
		}
	} else if (work->txs) {
		char *req;

		for (i = 0; i < ARRAY_SIZE(work->data); i++)
			be32enc(work->data + i, work->data[i]);
		bin2hex(data_str, (unsigned char *)work->data, 80);
		if (work->workid) {
			char *params;
			val = json_object_new_object();
			json_object_set_new(val, "workid", json_string(work->workid));
			params = json_dumps(val);
			req = malloc(128 + 2*80 + strlen(work->txs) + strlen(params));
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s\", %s], \"id\":1}\r\n",
				data_str, work->txs, params);
			json_decref(val);
		} else {
			req = malloc(128 + 2*80 + strlen(work->txs));
			sprintf(req,
				"{\"method\": \"submitblock\", \"params\": [\"%s%s\"], \"id\":1}\r\n",
				data_str, work->txs);
		}
		val = json_rpc_call(curl, rpc_url, rpc_userpass, req, NULL, 0);
		free(req);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			goto out;
		}

		json_decref(val);
	} else {
		/* build hex string */
		for (i = 0; i < ARRAY_SIZE(work->data); i++)
			le32enc(work->data + i, work->data[i]);
		bin2hex(data_str, (unsigned char *)work->data, sizeof(work->data));

		/* build JSON-RPC request */
		sprintf(s,
			"{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
			data_str);

		/* issue JSON-RPC request */
		val = json_rpc_call(curl, rpc_url, rpc_userpass, s, NULL, 0);
		if (unlikely(!val)) {
			applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			goto out;
		}

		res = json_object_get(val, "result");
		reason = json_object_get(val, "reject-reason");
		share_result(json_is_true(res), reason ? json_string_value(reason) : NULL);

		json_decref(val);
	}

	rc = true;

out:
	return rc;
}

static const char *getwork_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

#define GBT_CAPABILITIES "[\"coinbasetxn\", \"coinbasevalue\", \"longpoll\", \"workid\"]"
#define GBT_RULES "[\"segwit\"]"

static const char *gbt_req =
	"{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": "
	GBT_CAPABILITIES ", \"rules\": " GBT_RULES ", \"coinbase-addr\": \"%s\"}], \"id\":0}\r\n";
static const char *gbt_lp_req =
	"{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": "
	GBT_CAPABILITIES ", \"rules\": " GBT_RULES ", \"longpollid\": \"%s\", \"coinbase-addr\": \"%s\"}], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	int err;
	bool rc;
	struct timeval tv_start, tv_end, diff;
	char *req = NULL;
start:
	gettimeofday(&tv_start, NULL);
	if (have_gbt) {
		int len = strlen(gbt_req) + 40; 
		assert(pk_script_size > 0);
		req = malloc(len);
		assert(req);
		memset(req, 0, len);
		snprintf(req, len, gbt_req, coinbase_address);
	}
	val = json_rpc_call(curl, rpc_url, rpc_userpass,
			    have_gbt ? req : getwork_req,
			    &err, have_gbt ? JSON_RPC_QUIET_404 : 0);
	
	if (have_gbt) free(req);

	gettimeofday(&tv_end, NULL);

	if (have_stratum) {
		if (val)
			json_decref(val);
		return true;
	}

	if (!have_gbt && !allow_getwork) {
		applog(LOG_ERR, "No usable protocol");
		if (val)
			json_decref(val);
		return false;
	}

	if (have_gbt && allow_getwork && !val && err == CURLE_OK) {
		applog(LOG_INFO, "getblocktemplate failed, falling back to getwork");
		have_gbt = false;
		goto start;
	}

	if (!val)
		return false;

	if (have_gbt) {
		rc = gbt_work_decode(json_object_get(val, "result"), work);
		if (!have_gbt) {
			json_decref(val);
			goto start;
		}
	} else
		rc = work_decode(json_object_get(val, "result"), work);

	if (opt_debug && rc) {
		timeval_subtract(&diff, &tv_end, &tv_start);
		applog(LOG_DEBUG, "DEBUG: got new work in %d ms",
		       diff.tv_sec * 1000 + diff.tv_usec / 1000);
	}

	json_decref(val);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		work_free(wc->u.work);
		free(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = calloc(1, sizeof(*ret_work));
	if (!ret_work)
		return false;

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(curl, ret_work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			free(ret_work);
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "json_rpc_call failed, retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	/* send work to requesting thread */
	if (!tq_push(wc->thr->q, ret_work))
		free(ret_work);

	return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(curl, wc->u.work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "...terminating workio thread");
			return false;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "...retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	return true;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok) {
		struct workio_cmd *wc;

		/* wait for workio_cmd sent to us, on our queue */
		wc = tq_pop(mythr->q, NULL);
		if (!wc) {
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_GET_WORK:
			ok = workio_get_work(wc, curl);
			break;
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc, curl);
			break;

		default:		/* should never happen */
			ok = false;
			break;
		}

		workio_cmd_free(wc);
	}

	tq_freeze(mythr->q);
	curl_easy_cleanup(curl);

	return NULL;
}

static bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	if (opt_benchmark) {
		memset(work->data, 0x55, 76);
		work->data[17] = swab32(time(NULL));
		memset(work->data + 19, 0x00, 52);
		work->data[20] = 0x80000000;
		work->data[31] = 0x00000280;
		memset(work->target, 0x00, sizeof(work->target));
		return true;
	}

	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
		return false;
	}

	/* wait for response, a unit of work */
	work_heap = tq_pop(thr->q, NULL);
	if (!work_heap)
		return false;

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	free(work_heap);

	return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;
	
	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = malloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	work_copy(wc->u.work, work_in);

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	unsigned char merkle_root[64] = {0};
	int i;

	pthread_mutex_lock(&sctx->work_lock);

	free(work->job_id);
	work->job_id = strdup(sctx->job.job_id);
	work->xnonce2_len = sctx->xnonce2_size;
	work->xnonce2 = realloc(work->xnonce2, sctx->xnonce2_size);
	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

	/* Generate merkle root */
	sha256d(merkle_root, sctx->job.coinbase, sctx->job.coinbase_size);
	for (i = 0; i < sctx->job.merkle_count; i++) {
		memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
		sha256d(merkle_root, merkle_root, 64);
	}
	
	/* Increment extranonce2 */
	for (i = 0; i < sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

	/* Assemble block header */
	memset(work->data, 0, 128);
	work->data[0] = le32dec(sctx->job.version);
	for (i = 0; i < 8; i++)
		work->data[1 + i] = le32dec((uint32_t *)sctx->job.prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
	work->data[17] = le32dec(sctx->job.ntime);
	work->data[18] = le32dec(sctx->job.nbits);
	work->data[20] = 0x80000000;
	work->data[31] = 0x00000280;

	pthread_mutex_unlock(&sctx->work_lock);

	if (opt_debug) {
		char *xnonce2str = abin2hex(work->xnonce2, work->xnonce2_len);
		applog(LOG_DEBUG, "DEBUG: job_id='%s' extranonce2=%s ntime=%08x",
		       work->job_id, xnonce2str, swab32(work->data[17]));
		free(xnonce2str);
	}

	if (opt_algo == ALGO_SCRYPT)
		diff_to_target(work->target, sctx->job.diff / 65536.0);
	else
		diff_to_target(work->target, sctx->job.diff);
}

static void *miner_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	int thr_id = mythr->id;
	struct work work = {{0}};
	uint32_t max_nonce;
	uint32_t end_nonce = 0xffffffffU / opt_n_threads * (thr_id + 1) - 0x20;
	unsigned char *scratchbuf = NULL;
	char s[16] = {0};
	int i;

	/* Cpu affinity only makes sense if the number of threads is a multiple
	 * of the number of CPUs */
	if (num_processors > 1 && opt_n_threads % num_processors == 0) {
		if (!opt_quiet)
			applog(LOG_INFO, "Binding thread %d to cpu %d",
			       thr_id, thr_id % num_processors);
		affine_to_cpu(thr_id, thr_id % num_processors);
	}
	
	if (opt_algo == ALGO_SCRYPT) {
		scratchbuf = scrypt_buffer_alloc(opt_scrypt_n);
		if (!scratchbuf) {
			applog(LOG_ERR, "scrypt buffer allocation failed");
			exit(1);
		}
	}

	while (1) {
		unsigned long hashes_done;
		struct timeval tv_start, tv_end, diff;
		int64_t max64;
		int rc;

		if (have_stratum) {
			while (time(NULL) >= g_work_time + 120)
				sleep(1);
			pthread_mutex_lock(&g_work_lock);
			if (work.data[19] >= end_nonce && !memcmp(work.data, g_work.data, 76))
				stratum_gen_work(&stratum, &g_work);
		} else {
			int min_scantime = have_longpoll ? LP_SCANTIME : opt_scantime;
			/* obtain new work from internal workio thread */
			pthread_mutex_lock(&g_work_lock);
			if (!have_stratum &&
			    (time(NULL) - g_work_time >= min_scantime ||
			     work.data[19] >= end_nonce)) {
				work_free(&g_work);
				if (unlikely(!get_work(mythr, &g_work))) {
					applog(LOG_ERR, "work retrieval failed, exiting "
						"mining thread %d", mythr->id);
					pthread_mutex_unlock(&g_work_lock);
					goto out;
				}
				g_work_time = have_stratum ? 0 : time(NULL);
			}
			if (have_stratum) {
				pthread_mutex_unlock(&g_work_lock);
				continue;
			}
		}
		if (memcmp(work.data, g_work.data, 76)) {
			work_free(&work);
			work_copy(&work, &g_work);
			work.data[19] = 0xffffffffU / opt_n_threads * thr_id;
		} else
			work.data[19]++;
		pthread_mutex_unlock(&g_work_lock);
		work_restart[thr_id].restart = 0;
		
		/* adjust max_nonce to meet target scan time */
		if (have_stratum)
			max64 = LP_SCANTIME;
		else
			max64 = g_work_time + (have_longpoll ? LP_SCANTIME : opt_scantime)
			      - time(NULL);
		max64 *= thr_hashrates[thr_id];
		if (max64 <= 0) {
			switch (opt_algo) {
			case ALGO_SCRYPT:
				max64 = opt_scrypt_n < 16 ? 0x3ffff : 0x3fffff / opt_scrypt_n;
				break;
			case ALGO_SHA256D:
				max64 = 0x1fffff;
				break;
			}
		}
		if (work.data[19] + max64 > end_nonce)
			max_nonce = end_nonce;
		else
			max_nonce = work.data[19] + max64;
		
		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {
		case ALGO_SCRYPT:
			rc = scanhash_scrypt(thr_id, work.data, scratchbuf, work.target,
			                     max_nonce, &hashes_done, opt_scrypt_n);
			break;

		case ALGO_SHA256D:
			rc = scanhash_sha256d(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		default:
			/* should never happen */
			goto out;
		}

		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);
		timeval_subtract(&diff, &tv_end, &tv_start);
		if (diff.tv_usec || diff.tv_sec) {
			pthread_mutex_lock(&stats_lock);
			thr_hashrates[thr_id] =
				hashes_done / (diff.tv_sec + 1e-6 * diff.tv_usec);
			pthread_mutex_unlock(&stats_lock);
		}
		if (!opt_quiet) {
			memset(s, 0, 16);
			snprintf(s, 16, thr_hashrates[thr_id] >= 1e6 ? "%.0f" : "%.2f",
				1e-3 * thr_hashrates[thr_id]);
			debug(LOG_INFO, "thread %d: %lu hashes, %s khash/s",
				thr_id, hashes_done, s);
		}
		if (opt_benchmark && thr_id == opt_n_threads - 1) {
			double hashrate = 0.;
			for (i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
				hashrate += thr_hashrates[i];
			if (i == opt_n_threads) {
				memset(s, 0, 16);
				snprintf(s,16, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
				applog(LOG_INFO, "Total: %s khash/s", s);
			}
		}
		
		/* if nonce found, submit work */
		if (rc && !opt_benchmark && !submit_work(mythr, &work))
			break;
	}

out:
	tq_freeze(mythr->q);

	return NULL;
}

static void restart_threads(void)
{
	int i;

	for (i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	CURL *curl = NULL;
	char *copy_start, *hdr_path = NULL, *lp_url = NULL;
	bool need_slash = false;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		goto out;
	}

start:
	hdr_path = tq_pop(mythr->q, NULL);
	if (!hdr_path)
		goto out;

	/* full URL */
	if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	
	/* absolute path, on current server */
	else {
		copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (rpc_url[strlen(rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = malloc(strlen(rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

		sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
	}

	applog(LOG_INFO, "Long-polling activated for %s", lp_url);

	while (1) {
		json_t *val, *res, *soval;
		char *req = NULL;
		int err;

		if (have_gbt) {
			int len = strlen(gbt_lp_req) + strlen(lp_id) + 40;
			assert(pk_script_size > 0);
			req = malloc(len);
			assert(req);
			memset(req, 0, len);
			snprintf(req, len, gbt_lp_req, lp_id, coinbase_address);
		}
		val = json_rpc_call(curl, lp_url, rpc_userpass,
				    req ? req : getwork_req, &err,
				    JSON_RPC_LONGPOLL);
		if(req) free(req);
		if (have_stratum) {
			if (val)
				json_decref(val);
			goto out;
		}
		if (likely(val)) {
			bool rc;
			applog(LOG_INFO, "LONGPOLL pushed new work");
			res = json_object_get(val, "result");
			soval = json_object_get(res, "submitold");
			submit_old = soval ? json_is_true(soval) : false;
			pthread_mutex_lock(&g_work_lock);
			work_free(&g_work);
			if (have_gbt)
				rc = gbt_work_decode(res, &g_work);
			else
				rc = work_decode(res, &g_work);
			if (rc) {
				time(&g_work_time);
				restart_threads();
			}
			pthread_mutex_unlock(&g_work_lock);
			json_decref(val);
		} else {
			pthread_mutex_lock(&g_work_lock);
			g_work_time -= LP_SCANTIME;
			pthread_mutex_unlock(&g_work_lock);
			if (err == CURLE_OPERATION_TIMEDOUT) {
				restart_threads();
			} else {
				have_longpoll = false;
				restart_threads();
				free(hdr_path);
				free(lp_url);
				lp_url = NULL;
				sleep(opt_fail_pause);
				goto start;
			}
		}
	}

out:
	free(hdr_path);
	free(lp_url);
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;
}

static bool stratum_handle_response(char *buf)
{
	json_t *val, *err_val, *res_val, *id_val;
	bool ret = false;

	val = json_tokener_parse(buf);
	if (is_error(val)) {
		applog(LOG_INFO, "JSON decode failed");
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) || !res_val)
		goto out;

	share_result(json_is_true(res_val),
		err_val ? json_string_value(json_array_get(err_val, 1)) : NULL);

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}

static void *stratum_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	char *s;

	stratum.url = tq_pop(mythr->q, NULL);
	if (!stratum.url)
		goto out;
	applog(LOG_INFO, "Starting Stratum on %s", stratum.url);

	while (1) {
		int failures = 0;

		while (!stratum.curl) {
			pthread_mutex_lock(&g_work_lock);
			g_work_time = 0;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();

			if (!stratum_connect(&stratum, stratum.url) ||
			    !stratum_subscribe(&stratum) ||
			    !stratum_authorize(&stratum, rpc_user, rpc_pass)) {
				stratum_disconnect(&stratum);
				if (opt_retries >= 0 && ++failures > opt_retries) {
					applog(LOG_ERR, "...terminating workio thread");
					tq_push(thr_info[work_thr_id].q, NULL);
					goto out;
				}
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
		}

		if (stratum.job.job_id &&
		    (!g_work_time || strcmp(stratum.job.job_id, g_work.job_id))) {
			pthread_mutex_lock(&g_work_lock);
			stratum_gen_work(&stratum, &g_work);
			time(&g_work_time);
			pthread_mutex_unlock(&g_work_lock);
			if (stratum.job.clean) {
				applog(LOG_INFO, "Stratum requested work restart");
				restart_threads();
			}
		}
		
		if (!stratum_socket_full(&stratum, 120)) {
			applog(LOG_ERR, "Stratum connection timed out");
			s = NULL;
		} else
			s = stratum_recv_line(&stratum);
		if (!s) {
			stratum_disconnect(&stratum);
			applog(LOG_ERR, "Stratum connection interrupted");
			continue;
		}
		if (!stratum_handle_method(&stratum, s))
			stratum_handle_response(s);
		free(s);
	}

out:
	return NULL;
}

static void strhide(char *s)
{
	if (*s) *s++ = 'x';
	while (*s) *s++ = '\0';
}


static void show_usage_and_exit(int nexit)
{
	exit(nexit);
}

static void parse_arg(int key, char *arg, char *pname)
{
	char *p;
	int v, i;

	switch(key) {
	case 'a':
		for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
			v = strlen(algo_names[i]);
			if (!strncmp(arg, algo_names[i], v)) {
				if (arg[v] == '\0') {
					opt_algo = i;
					break;
				}
				if (arg[v] == ':' && i == ALGO_SCRYPT) {
					char *ep;
					v = strtol(arg+v+1, &ep, 10);
					if (*ep || v & (v-1) || v < 2)
						continue;
					opt_algo = i;
					opt_scrypt_n = v;
					break;
				}
			}
		}
		if (i == ARRAY_SIZE(algo_names)) {
			fprintf(stderr, "%s: unknown algorithm -- '%s'\n",
				pname, arg);
			show_usage_and_exit(1);
		}
		break;
	case 'B':
		opt_background = true;
		break;
	case 'c': {
		break;
	}
	case 'q':
		opt_quiet = true;
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		strhide(arg);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_scantime = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999)	/* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_n_threads = v;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		break;
	case 'o': {			/* --url */
		char *ap, *hp;
		ap = strstr(arg, "://");
		ap = ap ? ap + 3 : arg;
		hp = strrchr(arg, '@');
		if (hp) {
			*hp = '\0';
			p = strchr(ap, ':');
			if (p) {
				free(rpc_userpass);
				rpc_userpass = strdup(ap);
				free(rpc_user);
				rpc_user = calloc(p - ap + 1, 1);
				strncpy(rpc_user, ap, p - ap);
				free(rpc_pass);
				rpc_pass = strdup(++p);
				if (*p) *p++ = 'x';
				v = strlen(hp + 1) + 1;
				memmove(p + 1, hp + 1, v);
				memset(p + v, 0, hp - p);
				hp = p;
			} else {
				free(rpc_user);
				rpc_user = strdup(ap);
			}
			*hp++ = '@';
		} else
			hp = ap;
		if (ap != arg) {
			if (strncasecmp(arg, "http://", 7) &&
			    strncasecmp(arg, "https://", 8) &&
			    strncasecmp(arg, "stratum+tcp://", 14) &&
			    strncasecmp(arg, "stratum+tcps://", 15)) {
				fprintf(stderr, "%s: unknown protocol -- '%s'\n",
					pname, arg);
				show_usage_and_exit(1);
			}
			free(rpc_url);
			rpc_url = strdup(arg);
			strcpy(rpc_url + (ap - arg), hp);
		} else {
			if (*hp == '\0' || *hp == '/') {
				fprintf(stderr, "%s: invalid URL -- '%s'\n",
					pname, arg);
				show_usage_and_exit(1);
			}
			free(rpc_url);
			rpc_url = malloc(strlen(hp) + 8);
			sprintf(rpc_url, "http://%s", hp);
		}
		have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		break;
	}
	case 'O':			/* --userpass */
		p = strchr(arg, ':');
		if (!p) {
			fprintf(stderr, "%s: invalid username:password pair -- '%s'\n",
				pname, arg);
			show_usage_and_exit(1);
		}
		free(rpc_userpass);
		rpc_userpass = strdup(arg);
		free(rpc_user);
		rpc_user = calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		free(rpc_pass);
		rpc_pass = strdup(++p);
		strhide(p);
		break;
	case 'x':			/* --proxy */
		if (!strncasecmp(arg, "socks4://", 9))
			opt_proxy_type = CURLPROXY_SOCKS4;
		else if (!strncasecmp(arg, "socks5://", 9))
			opt_proxy_type = CURLPROXY_SOCKS5;
#if LIBCURL_VERSION_NUM >= 0x071200
		else if (!strncasecmp(arg, "socks4a://", 10))
			opt_proxy_type = CURLPROXY_SOCKS4A;
		else if (!strncasecmp(arg, "socks5h://", 10))
			opt_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
#endif
		else
			opt_proxy_type = CURLPROXY_HTTP;
		free(opt_proxy);
		opt_proxy = strdup(arg);
		break;
	case 1001:
		free(opt_cert);
		opt_cert = strdup(arg);
		break;
	case 1005:
		opt_benchmark = true;
		want_longpoll = false;
		want_stratum = false;
		have_stratum = false;
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1007:
		want_stratum = false;
		break;
	case 1009:
		opt_redirect = false;
		break;
	case 1010:
		allow_getwork = false;
		break;
	case 1011:
		have_gbt = false;
		break;
	case 1013:			/* --coinbase-addr */
		pk_script_size = address_to_script(pk_script, sizeof(pk_script), arg);
		if (!pk_script_size) {
			fprintf(stderr, "%s: invalid address -- '%s'\n",
				pname, arg);
			show_usage_and_exit(1);
		}
		snprintf(coinbase_address, 40, "%s", arg);
		break;
	case 1015:			/* --coinbase-sig */
		if (strlen(arg) + 1 > sizeof(coinbase_sig)) {
			fprintf(stderr, "%s: coinbase signature too long\n", pname);
			show_usage_and_exit(1);
		}
		strcpy(coinbase_sig, arg);
		break;
	case 'S':
		use_syslog = true;
		break;
	default:
		show_usage_and_exit(1);
	}
}

static int init_miner_config(s_config *config)
{
	if (!config->pool_server || !config->pool_server->pool_server || 
		!config->pool_server->coinbase_address || config->pool_server->port <= 0)
		return 0;

	rpc_user = malloc(strlen(config->pool_server->coinbase_address)+strlen(config->gw_id)+2);
	if (!rpc_user)
		return 0;
	memset(rpc_user, 0, strlen(config->pool_server->coinbase_address)+strlen(config->gw_id)+2);
	sprintf(rpc_user, "%s.%s", config->pool_server->coinbase_address, config->gw_id);
	rpc_pass = strdup(config->gw_id);


	if (!rpc_userpass) {
		rpc_userpass = malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if (!rpc_userpass)
			goto err;
		memset(rpc_userpass, 0, strlen(rpc_user) + strlen(rpc_pass) + 2);
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}
	
	if (!rpc_url) {
		rpc_url = malloc(strlen(config->pool_server->pool_server)+30);
		memset(rpc_url, 0, strlen(config->pool_server->pool_server)+30);
		sprintf(rpc_url, "stratum+tcp://%s:%d", 
			config->pool_server->pool_server, config->pool_server->port);	
	}

	pk_script_size = address_to_script(pk_script, sizeof(pk_script), config->pool_server->coinbase_address);
    if (!pk_script_size) {
		goto err;	
    }
    snprintf(coinbase_address, 40, "%s", config->pool_server->coinbase_address);	

	debug(LOG_DEBUG, "rpc_url  [%s] rpc_userpass [%s] coinbase_address [%s]",
				rpc_url, rpc_userpass, coinbase_address);	
	return 1;
err:
	if (rpc_url) free(rpc_url);
	if (rpc_userpass) free(rpc_userpass);
	if (rpc_user)	free(rpc_user);
	if (rpc_pass)	free(rpc_pass);
	return 0;
}

int miner_start(void *arg)
{
	struct thr_info *thr;
	long flags;
	int i;
	s_config *config = arg;
	
	if (!init_miner_config(config))
		return 1;
#if	0	
	pthread_mutex_init(&applog_lock, NULL);
#endif	
	pthread_mutex_init(&stats_lock, NULL);
	pthread_mutex_init(&g_work_lock, NULL);
	pthread_mutex_init(&stratum.sock_lock, NULL);
	pthread_mutex_init(&stratum.work_lock, NULL);

	flags = opt_benchmark || (strncasecmp(rpc_url, "https://", 8) &&
	                          strncasecmp(rpc_url, "stratum+tcps://", 15))
	      ? (CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL)
	      : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		applog(LOG_ERR, "CURL initialization failed");
		return 1;
	}


#if defined(WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	num_processors = sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_CONF)
	num_processors = sysconf(_SC_NPROCESSORS_CONF);
#elif defined(CTL_HW) && defined(HW_NCPU)
	int req[] = { CTL_HW, HW_NCPU };
	size_t len = sizeof(num_processors);
	sysctl(req, 2, &num_processors, &len, NULL, 0);
#else
	num_processors = 1;
#endif
	if (num_processors < 1)
		num_processors = 1;
	opt_n_threads = 1;


	work_restart = calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return 1;

	thr_info = calloc(opt_n_threads + 3, sizeof(*thr));
	if (!thr_info)
		return 1;
	
	thr_hashrates = (double *) calloc(opt_n_threads, sizeof(double));
	if (!thr_hashrates)
		return 1;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return 1;

	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return 1;
	}
	pthread_detach(thr->pth);

	if (want_longpoll && !have_stratum) {
		/* init longpoll thread info */
		longpoll_thr_id = opt_n_threads + 1;
		thr = &thr_info[longpoll_thr_id];
		thr->id = longpoll_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start longpoll thread */
		if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
			applog(LOG_ERR, "longpoll thread create failed");
			return 1;
		}
		pthread_detach(thr->pth);
	}
	
	if (want_stratum) {
		/* init stratum thread info */
		stratum_thr_id = opt_n_threads + 2;
		thr = &thr_info[stratum_thr_id];
		thr->id = stratum_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
			applog(LOG_ERR, "stratum thread create failed");
			return 1;
		}
		pthread_detach(thr->pth);

		if (have_stratum)
			tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", i);
			return 1;
		}
		pthread_detach(thr->pth);
	}

	applog(LOG_INFO, "%d miner threads started, "
		"using '%s' algorithm.",
		opt_n_threads,
		algo_names[opt_algo]);

	return 0;
}
