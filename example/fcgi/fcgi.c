#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <fcntl.h>

#include <fcgiapp.h>

#include "dnet/packet.h"
#include "dnet/interface.h"

#include "hash.h"
#include "common.h"

#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

#define DNET_FCGI_ID_PATTERN		"id="
#define DNET_FCGI_ID_DELIMITER		"&"
#define DNET_FCGI_LOG			"/tmp/dnet_fcgi.log"
#define DNET_FCGI_LOCAL_ADDR		"0.0.0.0:1025:2"
#define DNET_FCGI_SUCCESS_STATUS_PATTERN	"Status: 301"
#define DNET_FCGI_ROOT_PATTERN		""
#define DNET_FCGI_MAX_REQUEST_SIZE	(100*1024*1024)
#define DNET_FCGI_COOKIE_HEADER		"HTTP_COOKIE"
#define DNET_FCGI_SIGN_HASH		"md5"
#define DNET_FCGI_RANDOM_FILE		"/dev/urandom"
#define DNET_FCGI_COOKIE_DELIMITER	"obscure_cookie="
#define DNET_FCGI_COOKIE_ENDING		";"
#define DNET_FCGI_TOKEN_STRING		" "

static FILE *dnet_fcgi_log;
static pthread_cond_t dnet_fcgi_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t dnet_fcgi_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static int dnet_fcgi_request_completed, dnet_fcgi_request_init_value = 11223344;
static char *dnet_fcgi_status_pattern, *dnet_fcgi_root_pattern;
static unsigned long dnet_fcgi_max_request_size;
static int dnet_fcgi_base_port;
static unsigned char dnet_fcgi_id[DNET_ID_SIZE];

static char *dnet_fcgi_direct_download;
static int dnet_fcgi_direct_patterns_num;
static char **dnet_fcgi_direct_patterns;

/*
 * This is actually not a good idea, but it will work, since
 * no available crypto engines support that large digests.
 */
static char dnet_fcgi_sign_data[256];
static char dnet_fcgi_sign_tmp[4096];

/*
 * Freaking secure long lived key...
 */
static char *dnet_fcgi_sign_key;
static struct dnet_crypto_engine dnet_fcgi_sign_hash;

static char *dnet_fcgi_cookie_header, *dnet_fcgi_cookie_delimiter, *dnet_fcgi_cookie_ending;
static char *dnet_fcgi_cookie_addon;
static long dnet_fcgi_expiration_interval;
static int dnet_urandom_fd;

static int dnet_fcgi_dns_lookup;

static char *dnet_fcgi_unlink_pattern;

static FCGX_Request dnet_fcgi_request;

#define LISTENSOCK_FILENO	0
#define LISTENSOCK_FLAGS	0

static int dnet_fcgi_fill_config(struct dnet_config *cfg)
{
	char *p;
	int err;
	char addr[128];

	memset(cfg, 0, sizeof(struct dnet_config));

	cfg->sock_type = SOCK_STREAM;
	cfg->proto = IPPROTO_TCP;
	cfg->wait_timeout = 60;
	cfg->log_mask = DNET_LOG_ERROR | DNET_LOG_INFO;
	cfg->io_thread_num = 2;
	cfg->max_pending = 256;
	cfg->log = dnet_common_log;
	cfg->log_private = dnet_fcgi_log;
	cfg->log_mask = DNET_LOG_ERROR | DNET_LOG_INFO;

	p = getenv("DNET_FCGI_NODE_ID");
	if (p) {
		err = dnet_parse_numeric_id(p, cfg->id);
		if (err)
			return err;
	}

	p = getenv("DNET_FCGI_NODE_LOG_MASK");
	if (p)
		cfg->log_mask = strtoul(p, NULL, 0);

	p = getenv("DNET_FCGI_NODE_WAIT_TIMEOUT");
	if (p)
		cfg->wait_timeout = strtoul(p, NULL, 0);

	p = getenv("DNET_FCGI_NODE_LOCAL_ADDR");
	if (!p)
		p = DNET_FCGI_LOCAL_ADDR;

	snprintf(addr, sizeof(addr), "%s", p);

	err = dnet_parse_addr(addr, cfg);
	if (err)
		return err;

	return 0;
}

static int dnet_fcgi_add_remote_addr(struct dnet_node *n, struct dnet_config *main_cfg)
{
	char *a;
	char *addr, *p;
	int added = 0, err;
	struct dnet_config cfg;

	addr = getenv("DNET_FCGI_REMOTE_ADDR");
	if (!addr) {
		fprintf(dnet_fcgi_log, "No remote address specified, aborting.\n");
		err = -ENOENT;
		goto err_out_exit;
	}

	a = strdup(addr);
	if (!a) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	addr = a;

	while (addr) {
		p = strchr(addr, ' ');
		if (p)
			*p++ = '\0';

		memcpy(&cfg, main_cfg, sizeof(struct dnet_config));

		err = dnet_parse_addr(addr, &cfg);
		if (err) {
			fprintf(dnet_fcgi_log, "Failed to parse addr '%s': %d.\n", addr, err);
			goto next;
		}

		err = dnet_add_state(n, &cfg);
		if (err) {
			fprintf(dnet_fcgi_log, "Failed to add addr '%s': %d.\n", addr, err);
			goto next;
		}

		added++;

		if (!p)
			break;

next:
		addr = p;

		while (addr && *addr && isspace(*addr))
			addr++;
	}

	free(a);

	if (!added) {
		err = -ENOENT;
		fprintf(dnet_fcgi_log, "No remote addresses added, aborting.\n");
		goto err_out_exit;
	}

	return 0;

err_out_exit:
	return err;
}

static int dnet_fcgi_add_transform(struct dnet_node *n)
{
	char *h, *hash, *p;
	int added = 0, err;
	struct dnet_crypto_engine *e;

	hash = getenv("DNET_FCGI_HASH");
	if (!hash) {
		fprintf(dnet_fcgi_log, "No hashes specified, aborting.\n");
		err = -ENODEV;
		goto err_out_exit;
	}

	h = strdup(hash);
	if (!h) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	hash = h;

	while (hash) {
		p = strchr(hash, ' ');
		if (p)
			*p++ = '\0';

		e = malloc(sizeof(struct dnet_crypto_engine));
		if (!e) {
			err = -ENOMEM;
			goto err_out_exit;
		}

		memset(e, 0, sizeof(struct dnet_crypto_engine));

		err = dnet_crypto_engine_init(e, hash);
		if (err) {
			fprintf(dnet_fcgi_log, "Failed to initialize hash '%s': %d.\n",
					hash, err);
			goto err_out_exit;
		}

		err = dnet_add_transform(n, e, e->name,	e->init, e->update, e->final);
		if (err) {
			fprintf(dnet_fcgi_log, "Failed to add hash '%s': %d.\n", hash, err);
			goto err_out_exit;
		}

		fprintf(dnet_fcgi_log, "Added hash '%s'.\n", hash);
		added++;

		if (!p)
			break;

		hash = p;

		while (hash && *hash && isspace(*hash))
			hash++;
	}
	free(h);

	if (!added) {
		err = -ENOENT;
		fprintf(dnet_fcgi_log, "No remote hashes added, aborting.\n");
		goto err_out_exit;
	}

	return 0;

err_out_exit:
	return err;
}

#define dnet_fcgi_wait(condition)						\
({										\
	pthread_mutex_lock(&dnet_fcgi_wait_lock);				\
	while (!(condition)) 							\
		pthread_cond_wait(&dnet_fcgi_cond, &dnet_fcgi_wait_lock);	\
	pthread_mutex_unlock(&dnet_fcgi_wait_lock);				\
})

static void dnet_fcgi_wakeup(int err)
{
	pthread_mutex_lock(&dnet_fcgi_wait_lock);
	dnet_fcgi_request_completed = err;
	pthread_cond_broadcast(&dnet_fcgi_cond);
	pthread_mutex_unlock(&dnet_fcgi_wait_lock);
}

static void dnet_fcgi_data_to_hex(char *dst, unsigned int dlen, unsigned char *src, unsigned int slen)
{
	unsigned int i;

	if (slen > dlen/2 - 1)
		slen = dlen/2 - 1;

	for (i=0; i<slen; ++i)
		sprintf(&dst[2*i], "%02x", src[i]);
}

static int dnet_fcgi_generate_sign(long timestamp)
{
	char *cookie = FCGX_GetParam(dnet_fcgi_cookie_header, dnet_fcgi_request.envp);
	struct dnet_crypto_engine *e = &dnet_fcgi_sign_hash;
	int err, len;
	char cookie_res[128];
	unsigned int rsize = sizeof(dnet_fcgi_sign_data);

	if (cookie) {
		char *val, *end;

		val = strstr(cookie, dnet_fcgi_cookie_delimiter);
		cookie = NULL;

		if (val) {
			end = strstr(val, dnet_fcgi_cookie_ending);

			len = end - val;
			if (len > (int)sizeof(cookie_res) - 1)
				len = sizeof(cookie_res) - 1;

			snprintf(cookie_res, len, "%s", val);
			cookie = cookie_res;
		}
	}

	if (!cookie) {
		unsigned long long tmp;
		char *addr = FCGX_GetParam("REMOTE_ADDR", dnet_fcgi_request.envp);

		err = read(dnet_urandom_fd, &tmp, sizeof(tmp));
		if (err < 0) {
			err = -errno;
			fprintf(dnet_fcgi_log, "%s: failed to read random data: %s [%d].\n",
					addr, strerror(errno), errno);
			goto err_out_exit;
		}

		cookie = dnet_fcgi_sign_tmp;
		len = snprintf(dnet_fcgi_sign_tmp, sizeof(dnet_fcgi_sign_tmp), "%s-%lx-%llx", addr, timestamp, tmp);

		e->init(e, NULL);
		e->update(e, dnet_fcgi_sign_tmp, len, dnet_fcgi_sign_data, &rsize, 0);
		e->final(e, dnet_fcgi_sign_data, dnet_fcgi_sign_data, &rsize, 0);

		dnet_fcgi_data_to_hex(cookie_res, sizeof(cookie_res), (unsigned char *)dnet_fcgi_sign_data, rsize);

		FCGX_FPrintF(dnet_fcgi_request.out, "Set-Cookie: %s%s", dnet_fcgi_cookie_delimiter, cookie_res);
		if (dnet_fcgi_expiration_interval) {
			char str[128];
			struct tm tm;
			time_t t = timestamp + dnet_fcgi_expiration_interval;

			localtime_r(&t, &tm);
			strftime(str, sizeof(str), "%a, %d-%b-%Y %T %Z", &tm);
			FCGX_FPrintF(dnet_fcgi_request.out, "%s expires=%s%s", dnet_fcgi_cookie_ending, str, dnet_fcgi_cookie_addon);
		}
		FCGX_FPrintF(dnet_fcgi_request.out, "\r\n");
	}

	err = 0;
	len = snprintf(dnet_fcgi_sign_tmp, sizeof(dnet_fcgi_sign_tmp), "%s%lx%s", dnet_fcgi_sign_key, timestamp, cookie_res);

	rsize = sizeof(dnet_fcgi_sign_data);
	e->init(e, NULL);
	e->update(e, dnet_fcgi_sign_tmp, len, dnet_fcgi_sign_data, &rsize, 0);
	e->final(e, dnet_fcgi_sign_data, dnet_fcgi_sign_data, &rsize, 0);
	
	dnet_fcgi_data_to_hex(dnet_fcgi_sign_tmp, sizeof(dnet_fcgi_sign_tmp), (unsigned char *)dnet_fcgi_sign_data, rsize);

err_out_exit:
	return err;
}

static int dnet_fcgi_lookup_complete(struct dnet_net_state *st, struct dnet_cmd *cmd,
		struct dnet_attr *attr, void *priv)
{
	int err = 0;
	struct dnet_addr_attr *a;

	if (!cmd || !st) {
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!(cmd->flags & DNET_FLAGS_MORE)) {
		err = dnet_lookup_complete(st, cmd, attr, priv);
		if (err && err != -EEXIST)
			goto err_out_exit;

		a = (struct dnet_addr_attr *)(attr + 1);
#if 0
		fprintf(dnet_fcgi_log, "%s: addr: %s, is object presented there: %d.\n",
				dnet_dump_id(cmd->id),
				dnet_server_convert_dnet_addr(&a->addr),
				attr->flags);
#endif
		err = -EAGAIN;
		if (attr->flags) {
			char addr[256];
			char id[DNET_ID_SIZE*2+1];
			int port = dnet_server_convert_port((struct sockaddr *)a->addr.addr, a->addr.addr_len);
			long timestamp = time(NULL);

			snprintf(id, sizeof(id), "%s", dnet_dump_id_len(dnet_fcgi_id, DNET_ID_SIZE));

			if (dnet_fcgi_dns_lookup) {
				err = getnameinfo((struct sockaddr *)a->addr.addr, a->addr.addr_len,
						addr, sizeof(addr), NULL, 0, 0);
				if (err)
					snprintf(addr, sizeof(addr), "%s", dnet_state_dump_addr_only(&a->addr));
			} else {
				snprintf(addr, sizeof(addr), "%s", dnet_state_dump_addr_only(&a->addr));
			}
#if 0
			fprintf(dnet_fcgi_log, "%s -> http://%s%s/%d/%02x/%s\n",
					dnet_fcgi_status_pattern,
					addr,
					dnet_fcgi_root_pattern, port - dnet_fcgi_base_port,
					dnet_fcgi_id[0], id);
#endif
			FCGX_FPrintF(dnet_fcgi_request.out, "%s\r\n", dnet_fcgi_status_pattern);
			FCGX_FPrintF(dnet_fcgi_request.out, "Location: http://%s%s/%d/%02x/%s\r\n",
					addr,
					dnet_fcgi_root_pattern,
					port - dnet_fcgi_base_port,
					dnet_fcgi_id[0], id);

			if (dnet_fcgi_sign_key) {
				err = dnet_fcgi_generate_sign(timestamp);
				if (err)
					goto err_out_exit;
			}

			FCGX_FPrintF(dnet_fcgi_request.out, "Content-type: application/xml\r\n");
			FCGX_FPrintF(dnet_fcgi_request.out, "\r\n\r\n");

			FCGX_FPrintF(dnet_fcgi_request.out, "<?xml version=\"1.0\" encoding=\"utf-8\"?><download-info><host>%s</host><path>%s/%d/%02x/%s</path><ts>%lx</ts>",
					addr,
					dnet_fcgi_root_pattern, port - dnet_fcgi_base_port, dnet_fcgi_id[0], id,
					timestamp);
			if (dnet_fcgi_sign_key)
				FCGX_FPrintF(dnet_fcgi_request.out, "<s>%s</s>", dnet_fcgi_sign_tmp);
			FCGX_FPrintF(dnet_fcgi_request.out, "</download-info>\r\n");

			err = 0;
		}

		dnet_fcgi_wakeup(err);
	}

	if (cmd->status || !cmd->size) {
		err = cmd->status;
		goto err_out_exit;
	}

	return err;

err_out_exit:
	if (!cmd || !(cmd->flags & DNET_FLAGS_MORE))
		dnet_fcgi_wakeup(err);
	return err;
}

static int dnet_fcgi_unlink_complete(struct dnet_net_state *st __unused,
		struct dnet_cmd *cmd, struct dnet_attr *a __unused,
		void *priv __unused)
{
	if (!cmd || !(cmd->flags & DNET_FLAGS_MORE))
		dnet_fcgi_wakeup(dnet_fcgi_request_completed + 1);
	return 0;
}

static int dnet_fcgi_unlink(struct dnet_node *n, char *obj, int len)
{
	unsigned char addr[DNET_ID_SIZE];
	int err, error = -ENOENT;
	int pos = 0, num = 0;
	struct dnet_trans_control ctl;

	fprintf(dnet_fcgi_log, "Unlinking object '%s'.\n", obj);

	memset(&ctl, 0, sizeof(struct dnet_trans_control));

	ctl.complete = dnet_fcgi_unlink_complete;
	ctl.cmd = DNET_CMD_DEL;
	ctl.cflags = DNET_FLAGS_NEED_ACK;
	ctl.aflags = DNET_ATTR_DIRECT_TRANSACTION;

	dnet_fcgi_request_completed = 0;
	while (1) {
		unsigned int rsize = DNET_ID_SIZE;

		err = dnet_transform(n, obj, len, dnet_fcgi_id, addr, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}

		memcpy(ctl.id, dnet_fcgi_id, DNET_ID_SIZE);

		err = dnet_trans_alloc_send(n, &ctl);
		num++;
		if (err)
			error = err;
		else
			error = 0;
	}

	dnet_fcgi_wait(dnet_fcgi_request_completed == num);
	return error;
}

static int dnet_fcgi_process_io(struct dnet_node *n, char *obj, int len, struct dnet_io_control *ctl)
{
	unsigned char addr[DNET_ID_SIZE];
	int err, error = -ENOENT;
	int pos = 0;

	while (1) {
		unsigned int rsize = DNET_ID_SIZE;

		err = dnet_transform(n, obj, len, dnet_fcgi_id, addr, &rsize, &pos);
		if (err) {
			if (err > 0)
				break;
			continue;
		}

		dnet_fcgi_request_completed = dnet_fcgi_request_init_value;

		if (ctl) {
			memcpy(ctl->io.id, dnet_fcgi_id, DNET_ID_SIZE);
			memcpy(ctl->io.origin, dnet_fcgi_id, DNET_ID_SIZE);
			memcpy(ctl->addr, dnet_fcgi_id, DNET_ID_SIZE);

			err = dnet_read_object(n, ctl);
		} else {
			err = dnet_lookup_object(n, dnet_fcgi_id, 1,
					dnet_fcgi_lookup_complete, NULL);
		}

		if (err) {
			error = err;
			continue;
		}


		dnet_fcgi_wait(dnet_fcgi_request_completed != dnet_fcgi_request_init_value);

		if (dnet_fcgi_request_completed < 0) {
			error = dnet_fcgi_request_completed;
			continue;
		}

		error = 0;
		break;
	}

	return error;
}

static int dnet_fcgi_upload_complete(struct dnet_net_state *st, struct dnet_cmd *cmd,
		struct dnet_attr *attr __unused, void *priv __unused)
{
	int err = 0;

	if (!cmd || !st) {
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!(cmd->flags & DNET_FLAGS_MORE)) {
		dnet_fcgi_wakeup(dnet_fcgi_request_completed + 1);
		fprintf(dnet_fcgi_log, "%s: upload completed: %d.\n",
				dnet_dump_id(cmd->id), dnet_fcgi_request_completed);
	}

	if (cmd->status) {
		err = cmd->status;
		goto err_out_exit;
	}


err_out_exit:
	return err;
}

static int dnet_fcgi_upload(struct dnet_node *n, char *addr, char *obj, unsigned int len, void *data, uint64_t size)
{
	struct dnet_io_control ctl;
	int trans_num = 0;
	int err;

	memset(&ctl, 0, sizeof(struct dnet_io_control));

	ctl.data = data;
	ctl.fd = -1;

	ctl.complete = dnet_fcgi_upload_complete;
	ctl.priv = NULL;

	ctl.cflags = DNET_FLAGS_NEED_ACK;
	ctl.cmd = DNET_CMD_WRITE;
	ctl.aflags = DNET_ATTR_DIRECT_TRANSACTION | DNET_ATTR_NO_TRANSACTION_SPLIT;

	ctl.io.flags = DNET_IO_FLAGS_NO_HISTORY_UPDATE;
	ctl.io.size = size;
	ctl.io.offset = 0;

	dnet_fcgi_request_completed = 0;
	err = dnet_write_object(n, &ctl, obj, len, NULL, 1, &trans_num);
	if (err < 0) {
		fprintf(dnet_fcgi_log, "%s: failed to upload '%s' object: %d.\n", addr, obj, err);
		goto err_out_exit;
	}

	err = 0;

	fprintf(dnet_fcgi_log, "%s: waiting for upload completion: %d/%d.\n", addr, dnet_fcgi_request_completed, trans_num);
	dnet_fcgi_wait(dnet_fcgi_request_completed == trans_num);

err_out_exit:
	return err;
}

static int dnet_fcgi_handle_post(struct dnet_node *n, char *addr, char *id, int length)
{
	void *data;
	unsigned long data_size, size;
	char *p;
	long err;

	p = FCGX_GetParam("CONTENT_LENGTH", dnet_fcgi_request.envp);
	if (!p) {
		fprintf(dnet_fcgi_log, "%s: no content length.\n", addr);
		goto err_out_exit;
	}

	data_size = strtoul(p, NULL, 0);
	if (data_size > dnet_fcgi_max_request_size || !data_size) {
		fprintf(dnet_fcgi_log, "%s: invalid content length: %lu.\n", addr, data_size);
		goto err_out_exit;
	}

	data = malloc(data_size);
	if (!data) {
		fprintf(dnet_fcgi_log, "%s: failed to allocate %lu bytes.\n", addr, data_size);
		goto err_out_exit;
	}

	size = data_size;
	p = data;

	while (size) {
		err = FCGX_GetStr(p, size, dnet_fcgi_request.in);
		if (err < 0 && errno != EAGAIN) {
			fprintf(dnet_fcgi_log, "%s: failed to read %lu bytes, total of %lu: %s [%d].\n",
					addr, size, data_size, strerror(errno), errno);
			goto err_out_free;
		}

		if (err == 0) {
			fprintf(dnet_fcgi_log, "%s: short read, %lu/%lu, aborting.\n",
					addr, size, data_size);
			goto err_out_free;
		}

		p += err;
		size -= err;
	}

	err = dnet_fcgi_upload(n, addr, id, length, data, data_size);
	if (err)
		goto err_out_free;

	free(data);

	return 0;

err_out_free:
	free(data);
err_out_exit:
	return -EINVAL;
}

static void dnet_fcgi_destroy_sign_hash(void)
{
	if (!dnet_fcgi_sign_key)
		return;

	close(dnet_urandom_fd);
	dnet_crypto_engine_exit(&dnet_fcgi_sign_hash);
}

static int dnet_fcgi_setup_sign_hash(void)
{
	char *p;
	int err;

	dnet_fcgi_sign_key = getenv("DNET_FCGI_SIGN_KEY");
	if (!dnet_fcgi_sign_key) {
		err = 0;
		fprintf(dnet_fcgi_log, "No sign key, system will not authentificate users.\n");
		goto err_out_exit;
	}

	p = getenv("DNET_FCGI_SIGN_HASH");
	if (!p)
		p = DNET_FCGI_SIGN_HASH;

	err = dnet_crypto_engine_init(&dnet_fcgi_sign_hash, p);
	if (err) {
		fprintf(dnet_fcgi_log, "Failed to initialize hash '%s': %d.\n", p, err);
		goto err_out_exit;
	}

	p = getenv("DNET_FCGI_RANDOM_FILE");
	if (!p)
		p = DNET_FCGI_RANDOM_FILE;
	err = open(p, O_RDONLY);
	if (err < 0) {
		err = -errno;
		fprintf(dnet_fcgi_log, "Failed to open (read-only) random file '%s': %s [%d].\n",
				p, strerror(errno), errno);
		goto err_out_destroy;
	}
	dnet_urandom_fd  = err;

	dnet_fcgi_cookie_header = getenv("DNET_FCGI_COOKIE_HEADER");
	if (!dnet_fcgi_cookie_header)
		dnet_fcgi_cookie_header = DNET_FCGI_COOKIE_HEADER;

	dnet_fcgi_cookie_addon = getenv("DNET_FCGI_COOKIE_ADDON");
	if (!dnet_fcgi_cookie_addon)
		dnet_fcgi_cookie_addon = "";

	dnet_fcgi_cookie_delimiter = getenv("DNET_FCGI_COOKIE_DELIMITER");
	if (!dnet_fcgi_cookie_delimiter)
		dnet_fcgi_cookie_delimiter = DNET_FCGI_COOKIE_DELIMITER;

	dnet_fcgi_cookie_ending = getenv("DNET_FCGI_COOKIE_ENDING");
	if (!dnet_fcgi_cookie_ending)
		dnet_fcgi_cookie_ending = DNET_FCGI_COOKIE_ENDING;

	p = getenv("DNET_FCGI_COOKIE_EXPIRATION_INTERVAL");
	if (p)
		dnet_fcgi_expiration_interval = atoi(p);

	return 0;

err_out_destroy:
	dnet_crypto_engine_exit(&dnet_fcgi_sign_hash);
err_out_exit:
	dnet_fcgi_sign_key = NULL;
	return err;
}

static int dnet_fcgi_read_complete(struct dnet_net_state *st, struct dnet_cmd *cmd,
		struct dnet_attr *a, void *priv __unused)
{
	int err;
	struct dnet_io_attr *io;
	unsigned long long size;
	void *data;

	if (!cmd || !st) {
		err = -EINVAL;
		goto err_out_exit;
	}

	if (cmd->status || !cmd->size) {
		err = cmd->status;
		goto err_out_exit;
	}

	if (cmd->size <= sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr)) {
		fprintf(dnet_fcgi_log, "%s: read completion error: wrong size: cmd_size: %llu, must be more than %zu.\n",
				dnet_dump_id(cmd->id), (unsigned long long)cmd->size,
				sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!a) {
		fprintf(dnet_fcgi_log, "%s: no attributes but command size is not null.\n", dnet_dump_id(cmd->id));
		err = -EINVAL;
		goto err_out_exit;
	}

	io = (struct dnet_io_attr *)(a + 1);
	data = io + 1;

	dnet_convert_io_attr(io);

	FCGX_FPrintF(dnet_fcgi_request.out, "Content-type: octet/stream\r\n\r\n");

	size = io->size;
	while (size) {
		err = FCGX_PutStr(data, size, dnet_fcgi_request.out);
		if (err < 0 && errno != EAGAIN) {
			err = -errno;
			fprintf(dnet_fcgi_log, "%s: failed to write %llu bytes, "
					"total of %llu: %s [%d].\n",
					dnet_dump_id(dnet_fcgi_id), size, (unsigned long long)io->size,
					strerror(errno), errno);
			goto err_out_exit;
		}

		data += err;
		size -= err;
	}

	err = 0;

err_out_exit:
	if (!cmd || !(cmd->flags & DNET_FLAGS_MORE))
		dnet_fcgi_wakeup(err);
	return err;
}

static int dnet_fcgi_handle_get(struct dnet_node *n, char *query, char *addr, char *id, int length)
{
	int err;
	char *p;
	struct dnet_io_control ctl, *c = NULL;

	if (dnet_fcgi_unlink_pattern) {
		p = strstr(query, dnet_fcgi_unlink_pattern);
		if (p) {
			return dnet_fcgi_unlink(n, id, length);
		}
	}

	if (dnet_fcgi_direct_download) {
		int i;

		p = strstr(query, dnet_fcgi_direct_download);
		if (!p)
			goto lookup;

		for (i=0; i<dnet_fcgi_direct_patterns_num; ++i) {
			p = strstr(query, dnet_fcgi_direct_patterns[i]);
			if (p)
				break;
		}

		if (i != dnet_fcgi_direct_patterns_num) {
			memset(&ctl, 0, sizeof(struct dnet_io_control));

			ctl.fd = -1;
			ctl.complete = dnet_fcgi_read_complete;
			ctl.cmd = DNET_CMD_READ;
			ctl.cflags = DNET_FLAGS_NEED_ACK;

			c = &ctl;
		}
	}

lookup:
	err = dnet_fcgi_process_io(n, id, length, c);
	if (err) {
		fprintf(dnet_fcgi_log, "%s: Failed to lookup object '%s': %d.\n", addr, id, err);
		goto err_out_exit;
	}

	return 0;

err_out_exit:
	return err;
}

int main()
{
	char *p, *addr, *reason, *method, *query;
	char *id_pattern, *id_delimiter, *direct_patterns = NULL;
	int length, id_pattern_length, err, post_allowed;
	char *id, *end;
	struct dnet_config cfg;
	struct dnet_node *n;
	char tmp[128];

	dnet_fcgi_status_pattern = getenv("DNET_FCGI_SUCCESS_STATUS_PATTERN");
	if (!dnet_fcgi_status_pattern)
		dnet_fcgi_status_pattern = DNET_FCGI_SUCCESS_STATUS_PATTERN;

	dnet_fcgi_root_pattern = getenv("DNET_FCGI_ROOT_PATTERN");
	if (!dnet_fcgi_root_pattern)
		dnet_fcgi_root_pattern = DNET_FCGI_ROOT_PATTERN;

	p = getenv("DNET_FCGI_MAX_REQUEST_SIZE");
	if (p)
		dnet_fcgi_max_request_size = strtoul(p, NULL, 0);

	if (!dnet_fcgi_max_request_size)
		dnet_fcgi_max_request_size = DNET_FCGI_MAX_REQUEST_SIZE;

	p = getenv("DNET_FCGI_LOG");
	if (!p)
		p = DNET_FCGI_LOG;

	snprintf(tmp, sizeof(tmp), "%s.%d", p, getpid());
	p = tmp;

	dnet_fcgi_log = fopen(p, "a");
	if (!dnet_fcgi_log) {
		err = -errno;
		fprintf(stderr, "Failed to open '%s' log file.\n", p);
		goto err_out_exit;
	}

	p = getenv("DNET_FCGI_BASE_PORT");
	if (!p) {
		err = -ENOENT;
		fprintf(dnet_fcgi_log, "No DNET_FCGI_BASE_PORT provided, I will not be able to determine proper directory to fetch objects.\n");
		goto err_out_close;
	}
	dnet_fcgi_base_port = atoi(p);

	dnet_fcgi_unlink_pattern = getenv("DNET_FCGI_UNLINK_PATTERN_URI");

	dnet_fcgi_direct_download = getenv("DNET_FCGI_DIRECT_PATTERN_URI");
	if (dnet_fcgi_direct_download) {
		p = getenv("DNET_FCGI_DIRECT_PATTERNS");
		if (!p) {
			dnet_fcgi_direct_download = NULL;
		} else {
			char *tmp = strdup(p);
			char *saveptr, *token;

			if (!tmp) {
				err = -ENOMEM;
				goto err_out_close;
			}

			direct_patterns = tmp;

			while (1) {
				token = strtok_r(tmp, DNET_FCGI_TOKEN_STRING, &saveptr);
				if (!token)
					break;

				dnet_fcgi_direct_patterns_num++;
				dnet_fcgi_direct_patterns = realloc(dnet_fcgi_direct_patterns,
						dnet_fcgi_direct_patterns_num * sizeof(char *));
				if (!dnet_fcgi_direct_patterns) {
					err = -ENOMEM;
					goto err_out_free_direct_patterns;
				}

				fprintf(dnet_fcgi_log, "Added '%s' direct download pattern.\n", token);

				dnet_fcgi_direct_patterns[dnet_fcgi_direct_patterns_num - 1] = token;
				tmp = NULL;
			}
		}
	}


	err = dnet_fcgi_fill_config(&cfg);
	if (err) {
		fprintf(dnet_fcgi_log, "Failed to parse config.\n");
		goto err_out_free_direct_patterns;
	}

	err = dnet_fcgi_setup_sign_hash();
	if (err)
		goto err_out_close;

	n = dnet_node_create(&cfg);
	if (!n)
		goto err_out_sign_destroy;

	err = dnet_fcgi_add_remote_addr(n, &cfg);
	if (err)
		goto err_out_free;

	err = dnet_fcgi_add_transform(n);
	if (err)
		goto err_out_free;

	p = getenv("DNET_FCGI_DNS_LOOKUP");
	if (p)
		dnet_fcgi_dns_lookup = atoi(p);

	post_allowed = 0;
	p = getenv("DNET_FCGI_POST_ALLOWED");
	if (p)
		post_allowed = atoi(p);

	fprintf(dnet_fcgi_log, "Started on %s, POST is %s.\n", getenv("SERVER_ADDR"), (post_allowed) ? "allowed" : "not allowed");
	fflush(dnet_fcgi_log);

	id_pattern = getenv("DNET_FCGI_ID_PATTERN");
	id_delimiter = getenv("DNET_FCGI_ID_DELIMITER");

	if (!id_pattern)
		id_pattern = DNET_FCGI_ID_PATTERN;
	if (!id_delimiter)
		id_delimiter = DNET_FCGI_ID_DELIMITER;

	id_pattern_length = strlen(id_pattern);

	err = FCGX_Init();
	if (err) {
		fprintf(dnet_fcgi_log, "FCGX initaliation failed: %d.\n", err);
		goto err_out_free;
	}

	err = FCGX_InitRequest(&dnet_fcgi_request, LISTENSOCK_FILENO, LISTENSOCK_FLAGS);
	if (err) {
		fprintf(dnet_fcgi_log, "FCGX request initaliation failed: %d.\n", err);
		goto err_out_fcgi_exit;
	}

	while (1) {
		err = FCGX_Accept_r(&dnet_fcgi_request);
		if (err || !dnet_fcgi_request.in || !dnet_fcgi_request.out || !dnet_fcgi_request.err || !dnet_fcgi_request.envp) {
			fprintf(dnet_fcgi_log, "Failed to accept client: no IO streams: in: %p, out: %p, err: %p, env: %p, err: %d.\n",
					dnet_fcgi_request.in, dnet_fcgi_request.out, dnet_fcgi_request.err, dnet_fcgi_request.envp, err);
			continue;
		}

		addr = FCGX_GetParam("REMOTE_ADDR", dnet_fcgi_request.envp);
		if (!addr)
			continue;

		method = FCGX_GetParam("REQUEST_METHOD", dnet_fcgi_request.envp);

		err = -EINVAL;
		query = p = FCGX_GetParam("QUERY_STRING", dnet_fcgi_request.envp);
		if (!p) {
			reason = "no query string";
			goto err_continue;
		}

		id = strstr(p, id_pattern);
		if (!id) {
			reason = "malformed request, no id part";
			goto err_continue;
		}

		id += id_pattern_length;
		if (!*id) {
			reason = "malformed request, no id part";
			goto err_continue;
		}

		end = strstr(id, id_delimiter);
		if (!end)
			end = p + strlen(p);

		length = end - id;

		if (!strncmp(method, "POST", 4)) {
			if (!post_allowed) {
				err = -EACCES;
				fprintf(dnet_fcgi_log, "%s: POST is not allowed for object '%s'.\n", addr, id);
				reason = "POST is not allowed";
				goto err_continue;
			}

			err = dnet_fcgi_handle_post(n, addr, id, length);
			if (err) {
				fprintf(dnet_fcgi_log, "%s: Failed to handle POST for object '%s': %d.\n", addr, id, err);
				reason = "failed to handle POST";
				goto err_continue;
			}
		} else {
			err = dnet_fcgi_handle_get(n, query, addr, id, length);
			if (err) {
				fprintf(dnet_fcgi_log, "%s: Failed to handle GET for object '%s': %d.\n", addr, id, err);
				reason = "failed to handle GET";
				goto err_continue;
			}
		}

cont:
		FCGX_Finish_r(&dnet_fcgi_request);
		continue;

err_continue:
		FCGX_FPrintF(dnet_fcgi_request.out, "Status: 400\r\n\r\n: %s: %d\r\n", reason, err);
		fprintf(dnet_fcgi_log, "%s: bad request: %s: %s [%d]\n", addr, reason, strerror(-err), err);
		fflush(dnet_fcgi_log);
		goto cont;
	}

	dnet_node_destroy(n);
	dnet_fcgi_destroy_sign_hash();

	free(direct_patterns);
	free(dnet_fcgi_direct_patterns);

	fflush(dnet_fcgi_log);
	fclose(dnet_fcgi_log);

	return 0;

err_out_fcgi_exit:
	FCGX_ShutdownPending();
err_out_free:
	dnet_node_destroy(n);
err_out_sign_destroy:
	dnet_fcgi_destroy_sign_hash();
err_out_free_direct_patterns:
	free(direct_patterns);
	free(dnet_fcgi_direct_patterns);
err_out_close:
	fflush(dnet_fcgi_log);
	fclose(dnet_fcgi_log);
err_out_exit:
	return err;
}