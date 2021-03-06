/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elliptics.h"
#include "elliptics/interface.h"

ssize_t dnet_db_read_raw(struct eblob_backend *b, struct dnet_raw_id *id, void **datap)
{
	struct eblob_key key;
	void *data;
	uint64_t offset, size;
	int fd, err;

	memcpy(key.id, id->id, DNET_ID_SIZE);

	err = eblob_read(b, &key, &fd, &offset, &size, EBLOB_TYPE_META);
	if (err) {
		goto err_out_exit;
	}

	data = malloc(size);
	if (!data) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	err = pread(fd, data, size, offset);
	if (err != (int)size) {
		err = -errno;
		goto err_out_free;
	}

	*datap = data;

	return size;

err_out_free:
	free(data);
err_out_exit:
	return err;
}

int dnet_db_write_raw(struct eblob_backend *b, struct dnet_raw_id *id, void *data, unsigned int size)
{
	struct eblob_write_control wc;
	struct eblob_key key;
	int err;

	memset(&wc, 0, sizeof(struct eblob_write_control));
	memcpy(key.id, id->id, DNET_ID_SIZE);
	err = eblob_write(b, &key, data, 0, size, BLOB_DISK_CTL_NOCSUM, EBLOB_TYPE_META);
	if (err)
		goto err_out_exit;

	wc.offset = 0;
	wc.size = size;
	wc.flags = BLOB_DISK_CTL_NOCSUM;
	wc.type = EBLOB_TYPE_META;
	err = eblob_write_commit(b, &key, NULL, 0, &wc);

err_out_exit:
	return err;
}

static int dnet_db_remove_direct(struct eblob_backend *b, struct dnet_raw_id *id)
{
	struct eblob_key key;

	memcpy(key.id, id->id, EBLOB_ID_SIZE);
	return eblob_remove(b, &key, EBLOB_TYPE_META);
}

int dnet_db_remove_raw(struct eblob_backend *b, struct dnet_raw_id *id, int real_del)
{
	if (real_del) {
		dnet_db_remove_direct(b, id);
		return 1;
	}

	return dnet_update_ts_metadata(b, id, DNET_IO_FLAGS_REMOVED, 0);
}

int dnet_update_ts_metadata(struct eblob_backend *b, struct dnet_raw_id *id, uint64_t flags_set, uint64_t flags_clear)
{
	int err = 0;
	struct dnet_meta_container mc;
	struct dnet_meta *m;

	memset(&mc, 0, sizeof(struct dnet_meta_container));

	err = dnet_db_read_raw(b, id, &mc.data);
	if (err < 0) {
		m = malloc(sizeof(struct dnet_meta) + sizeof(struct dnet_meta_update));
		if (!m) {
			err = -ENOMEM;
			goto err_out_exit;
		}
		dnet_create_meta_update(m, NULL, flags_set, flags_clear);

		mc.data = m;
		mc.size = sizeof(struct dnet_meta_update) + sizeof(struct dnet_meta);
	} else {
		err = dnet_update_ts_metadata_raw(&mc, flags_set, flags_clear);
		if (err) {
			/* broken metadata, rewrite it */
			if (err != -ENOENT) {
				free(mc.data);

				mc.data = NULL;
				mc.size = 0;
			}

			mc.data = realloc(mc.data, mc.size + sizeof(struct dnet_meta) + sizeof(struct dnet_meta_update));
			if (!mc.data) {
				err = -ENOMEM;
				goto err_out_exit;
			}

			m = mc.data + mc.size;
			mc.size += sizeof(struct dnet_meta) + sizeof(struct dnet_meta_update);

			dnet_create_meta_update(m, NULL, flags_set, flags_clear);
		}
	}

	err = dnet_db_write_raw(b, id, mc.data, mc.size);
	if (err) {
		goto err_out_free;
	}

err_out_free:
	free(mc.data);
err_out_exit:
       return err;
}

int dnet_process_meta(struct dnet_net_state *st, struct dnet_cmd *cmd, struct dnet_io_attr *io)
{
	struct dnet_node *n = st->n;
	struct dnet_raw_id id;
	void *data;
	int err;

	if (cmd->cmd == DNET_CMD_READ || cmd->cmd == DNET_CMD_WRITE) {
		if (cmd->size < sizeof(struct dnet_io_attr)) {
			dnet_log(n, DNET_LOG_ERROR,
				"%s: wrong read attribute, size does not match "
					"IO attribute size: size: %llu, must be: %zu.\n",
					dnet_dump_id(&cmd->id), (unsigned long long)cmd->size,
					sizeof(struct dnet_io_attr));
			err = -EINVAL;
			goto err_out_exit;
		}

		memcpy(id.id, io->id, DNET_ID_SIZE);
	}

	switch (cmd->cmd) {
	case DNET_CMD_READ:
		err = n->cb->meta_read(n->cb->command_private, &id, &data);
		if (err > 0) {
			io->size = err;
			err = dnet_send_read_data(st, cmd, io, data, -1, io->offset, 0);
			free(data);
		}
		break;
	case DNET_CMD_WRITE:
		if (n->flags & DNET_CFG_NO_META) {
			err = 0;
			break;
		}

		data = io + 1;

		err = n->cb->meta_write(n->cb->command_private, &id, data, io->size);
		break;
	case DNET_CMD_DEL:
		memcpy(id.id, cmd->id.id, DNET_ID_SIZE);
		n->cb->meta_remove(n->cb->command_private, &id, !!(cmd->flags & DNET_ATTR_DELETE_HISTORY));
		err = n->cb->command_handler(st, n->cb->command_private, cmd, io);
		break;
	default:
		err = -EINVAL;
		break;
	}

err_out_exit:
	return err;
}

struct dnet_db_list_control {
	struct dnet_node		*n;
	struct dnet_net_state		*st;
	struct dnet_cmd			*cmd;
	struct dnet_check_request	*req;
	struct dnet_check_params	params;

	atomic_t			completed;
	atomic_t			errors;
	atomic_t			total;
};

static long long dnet_meta_get_ts(struct dnet_node *n, struct dnet_meta_container *mc)
{
	struct dnet_meta *m;
	struct dnet_meta_check_status *c;

	m = dnet_meta_search(n, mc, DNET_META_CHECK_STATUS);
	if (!m)
		return -ENOENT;

	c = (struct dnet_meta_check_status *)m->data;
	dnet_convert_meta_check_status(c);

	return (long long)c->tm.tsec;
}

static int dnet_db_send_check_reply(struct dnet_db_list_control *ctl)
{
	struct dnet_check_reply reply;

	memset(&reply, 0, sizeof(reply));

	reply.total = atomic_read(&ctl->total);
	reply.errors = atomic_read(&ctl->errors);
	reply.completed = atomic_read(&ctl->completed);

	dnet_convert_check_reply(&reply);
	return dnet_send_reply(ctl->st, ctl->cmd, &reply, sizeof(reply), 1);
}

struct dnet_check_temp_db * dnet_check_temp_db_alloc(struct dnet_node *n, char *path)
{
	static char temp_meta_path[310];
	struct eblob_config ecfg;
	struct dnet_check_temp_db *db;

	db = (struct dnet_check_temp_db *)malloc(sizeof(struct dnet_check_temp_db));
	if (!db) {
		dnet_log(n, DNET_LOG_ERROR, "Failed to allocate memory for temp meta eblob config\n");
		return NULL;
	}

	snprintf(temp_meta_path, sizeof(temp_meta_path), "%s/tmp_meta", path);

	memset(&ecfg, 0, sizeof(struct eblob_config));

	ecfg.file = temp_meta_path;

	db->log.log = n->log->log;
	db->log.log_private = n->log->log_private;
	db->log.log_level = EBLOB_LOG_NOTICE;
	ecfg.log = &db->log;

	db->b = eblob_init(&ecfg);
	if (!db->b) {
		dnet_log(n, DNET_LOG_ERROR, "Failed to initialize temp meta eblob\n");
		goto err_out_free;
	}

	atomic_init(&db->refcnt, 1);

	return db;

err_out_free:
	free(db);
	return NULL;
}

int dnet_db_iterate(struct eblob_backend *b, struct dnet_iterate_ctl *dctl)
{
	struct eblob_iterate_control ctl;

	memset(&ctl, 0, sizeof(ctl));

	ctl.flags = dctl->flags | EBLOB_ITERATE_FLAGS_ALL;
	ctl.priv = dctl->callback_private;
	ctl.iterator_cb = dctl->iterate_cb;
	ctl.start_type = ctl.max_type = EBLOB_TYPE_META;
	ctl.blob_start = dctl->blob_start;
	ctl.blob_num = dctl->blob_num;

	return eblob_iterate(b, &ctl);
}

static int dnet_db_list_iter_init(struct eblob_iterate_control *iter_ctl, void **thread_priv)
{
	struct dnet_db_list_control *ctl = iter_ctl->priv;
	struct dnet_node *n = ctl->n;
	struct dnet_bulk_array *bulk_array = NULL;
	struct dnet_net_state *st;
	struct dnet_group *g;
	int only_merge = !!(ctl->req->flags & DNET_CHECK_MERGE);
	int bulk_array_tmp_num;
	int err = 0;

	dnet_log(n, DNET_LOG_DEBUG, "BULK: only_merge=%d\n", only_merge);
	if (!only_merge) {
		bulk_array = malloc(sizeof(struct dnet_bulk_array));
		if (!bulk_array) {
			err = -ENOMEM;
			goto err_out_exit;
		}
		atomic_init(&bulk_array->refcnt, 0);

		bulk_array_tmp_num = DNET_BULK_STATES_ALLOC_STEP;
		bulk_array->num = 0;
		bulk_array->states = NULL;
		dnet_log(n, DNET_LOG_DEBUG, "BULK: allocating space for arrays, num=%d\n", bulk_array_tmp_num);

		bulk_array->states = (struct dnet_bulk_state *)malloc(sizeof(struct dnet_bulk_state) * bulk_array_tmp_num);
		if (!bulk_array->states) {
			err = -ENOMEM;
			dnet_log(n, DNET_LOG_ERROR, "BULK: Failed to allocate buffer for bulk states array.\n");
			goto err_out_exit;
		}

		pthread_mutex_lock(&n->state_lock);
		list_for_each_entry(g, &n->group_list, group_entry) {
			if (g->group_id == n->st->idc->group->group_id)
				continue;

			list_for_each_entry(st, &g->state_list, state_entry) {
				if (st == n->st)
					continue;

				if (bulk_array->num == bulk_array_tmp_num) {
					dnet_log(n, DNET_LOG_DEBUG, "BULK: reallocating space for arrays, num=%d\n", bulk_array_tmp_num);
					bulk_array_tmp_num += DNET_BULK_STATES_ALLOC_STEP;
					bulk_array->states = realloc(bulk_array->states, sizeof(struct dnet_bulk_state) * bulk_array_tmp_num);
					if (!bulk_array->states) {
						err = -ENOMEM;
						dnet_log(n, DNET_LOG_ERROR, "BULK: Failed to reallocate buffer for bulk states array.\n");
						goto err_out_exit;
					}
				}

				memcpy(&bulk_array->states[bulk_array->num].addr, &st->addr, sizeof(struct dnet_addr));
				pthread_mutex_init(&bulk_array->states[bulk_array->num].state_lock, NULL);
				bulk_array->states[bulk_array->num].num = 0;
				bulk_array->states[bulk_array->num].ids = NULL;

				bulk_array->states[bulk_array->num].ids = malloc(sizeof(struct dnet_bulk_id) * DNET_BULK_IDS_SIZE);
				if (!bulk_array->states[bulk_array->num].ids) {
					err = -ENOMEM;
					dnet_log(n, DNET_LOG_ERROR, "BULK: Failed to reallocate buffer for bulk states array.\n");
					pthread_mutex_unlock(&n->state_lock);
					goto err_out_exit;
				}

				dnet_log(n, DNET_LOG_DEBUG, "BULK: added state %s (%s)\n",
						dnet_dump_id_str(st->idc->ids[0].raw.id),
						dnet_server_convert_dnet_addr(&st->addr));
				bulk_array->num++;
			}
		}
		pthread_mutex_unlock(&n->state_lock);

		qsort(bulk_array->states, bulk_array->num, sizeof(struct dnet_bulk_state), dnet_compare_bulk_state);
	}

	*thread_priv = bulk_array;
	return 0;

err_out_exit:
	return err;
}

static int dnet_db_list_iter_free(struct eblob_iterate_control *iter_ctl, void **thread_priv)
{
	struct dnet_db_list_control *ctl = iter_ctl->priv;
	struct dnet_node *n = ctl->n;
	struct dnet_bulk_array *bulk_array = *thread_priv;
	int err;
	int i;

	if (bulk_array) {
		while(atomic_read(&bulk_array->refcnt) > 0)
			sleep(1);

		for (i = 0; i < bulk_array->num; ++i) {
			dnet_log(n, DNET_LOG_DEBUG, "CHECK: free: processing state %d %s: %d ids in this state\n",
					i, dnet_server_convert_dnet_addr(&bulk_array->states[i].addr), bulk_array->states[i].num);

			if (bulk_array->states[i].num > 0) {
				err = dnet_request_bulk_check(n, &bulk_array->states[i], &ctl->params);
				if (err) {
					dnet_log(n, DNET_LOG_ERROR, "CHECK: dnet_request_bulk_check failed, state %s, err %d\n",
							dnet_server_convert_dnet_addr(&bulk_array->states[i].addr), err);
				}
			}
			free(bulk_array->states[i].ids);
		}

		free(bulk_array->states);
		free(bulk_array);
	}

	*thread_priv = NULL;

	return 0;
}

static int dnet_db_list_iter(struct eblob_disk_control *dc, struct eblob_ram_control *rc,
				void *data, void *p, void *thread_priv)
{
	struct dnet_db_list_control *ctl = p;
	struct dnet_node *n = ctl->n;
	struct dnet_meta_container mc;
	struct dnet_net_state *tmp;
	struct dnet_bulk_array *bulk_array;
	long long check_ts, check_edge_ts = ctl->req->timestamp, update_ts;
	char check_time[64], check_edge_time[64], update_start[64], update_stop[64], update_time[64];
	struct tm tm;
	int will_check, should_be_merged;
	int send_check_reply = 1;
	int err = 0;

	bulk_array = thread_priv;
	if (!bulk_array && !(ctl->req->flags & DNET_CHECK_MERGE)) {
		dnet_log(n, DNET_LOG_ERROR, "CHECK: bulk_array is not initialized and check type is not MERGE_ONLY\n");
		return -ENOMEM;
	}

	mc.data = data;
	mc.size = rc->size;

	if (check_edge_ts) {
		localtime_r((time_t *)&check_edge_ts, &tm);
		strftime(check_edge_time, sizeof(check_edge_time), "%F %R:%S %Z", &tm);
	} else {
		snprintf(check_edge_time, sizeof(check_edge_time), "no-check-edge");
	}

	if (ctl->req->updatestamp_start) {
		localtime_r((time_t *)&ctl->req->updatestamp_start, &tm);
		strftime(update_start, sizeof(update_start), "%F %R:%S %Z", &tm);
	} else {
		snprintf(update_start, sizeof(update_start), "all");
	}
	if (!ctl->req->updatestamp_stop)
		ctl->req->updatestamp_stop = time(NULL);
	localtime_r((time_t *)&ctl->req->updatestamp_stop, &tm);
	strftime(update_stop, sizeof(update_stop), "%F %R:%S %Z", &tm);


	dnet_setup_id(&mc.id, n->id.group_id, dc->key.id);

	/*
	* Use group ID field to specify whether we should check number of copies
	* or merge transaction with other history log in the storage
	*
	* tmp == NULL means this key belongs to given node and we should check
	* number of its copies in the storage. If state is not NULL then given
	* key must be moved to another machine and potentially merged with data
	* present there
	*/
	tmp = dnet_state_get_first(n, &mc.id);
	should_be_merged = (tmp != NULL);
	dnet_state_put(tmp);

	/*
	* If timestamp is specified check should be performed only to files
	* that was not checked since that timestamp
	*/
	check_ts = dnet_meta_get_ts(n, &mc);
	will_check = !(check_edge_ts && (check_ts > check_edge_ts));

	/*
	 * If start/stop update stamp is specified check should be performed only to files
	 * that were created in that interval (inclusive)
	 */
	update_ts = 0;
	if (will_check) {
		struct dnet_meta_update mu;

		/* only try to check creation/update timestamp if it is really present in database */
		if (dnet_get_meta_update(n, &mc, &mu)) {
			update_ts = mu.tm.tsec;

			will_check = 0;
			if ((mu.tm.tsec >= ctl->req->updatestamp_start) && (mu.tm.tsec <= ctl->req->updatestamp_stop))
				will_check = 1;
		}
	}

	if (will_check && !should_be_merged && (ctl->req->flags & DNET_CHECK_MERGE)) {
		will_check = 0;
	}

	if (n->log->log_level > DNET_LOG_NOTICE) {
		localtime_r((time_t *)&check_ts, &tm);
		strftime(check_time, sizeof(check_time), "%F %R:%S %Z", &tm);

		localtime_r((time_t *)&update_ts, &tm);
		strftime(update_time, sizeof(update_time), "%F %R:%S %Z", &tm);

		dnet_log_raw(n, DNET_LOG_NOTICE, "CHECK: start key: %s, "
				"last check: %lld [%s], "
				"last check before: %lld [%s], "
				"created/updated: %lld [%s], "
				"updated between: %lld [%s] - %lld [%s], "
				"will check: %d, should_be_merged: %d, dry: %d, flags: %x, size: %u.\n",
				dnet_dump_id(&mc.id),
				check_ts, check_time,
				check_edge_ts, check_edge_time,
				update_ts, update_time,
				(unsigned long long)ctl->req->updatestamp_start, update_start,
				(unsigned long long)ctl->req->updatestamp_stop, update_stop,
				will_check, should_be_merged,
				!!(ctl->req->flags & DNET_CHECK_DRY_RUN), ctl->req->flags, mc.size);
	}

	if (will_check) {
		err = 0;
		if (!(ctl->req->flags & DNET_CHECK_DRY_RUN)) {
			err = dnet_check(n, &mc, bulk_array, should_be_merged, &ctl->params);

			dnet_log_raw(n, DNET_LOG_NOTICE, "CHECK: complete key: %s, merge: %d, err: %d\n",
					dnet_dump_id(&mc.id), should_be_merged, err);
		}

		if (!err) {
			atomic_inc(&ctl->completed);
		} else {
			atomic_inc(&ctl->errors);
		}
	}

	if ((atomic_inc(&ctl->total) % 30000) == 0) {
		if (send_check_reply) {
			if (dnet_db_send_check_reply(ctl))
				send_check_reply = 0;
		}

		dnet_log(n, DNET_LOG_INFO, "CHECK: total: %d, completed: %d, errors: %d\n",
				atomic_read(&ctl->total), atomic_read(&ctl->completed), atomic_read(&ctl->errors));
	}


	/*
	 * We do not return check error, since it wil be propagated to all other iterating threads
	 */

	return 0;
}

int dnet_db_list(struct dnet_net_state *st, struct dnet_cmd *cmd)
{
	struct dnet_node *n = st->n;
	struct dnet_db_list_control ctl;
	struct dnet_check_request *r, req;
	char ctl_time[64];
	struct tm tm;
	int err = 0;

	if (n->check_in_progress)
		return -EINPROGRESS;

	if (cmd->size < sizeof(struct dnet_check_request)) {
		dnet_log(n, DNET_LOG_ERROR, "%s: CHECK: invalid check request size %llu, must be %zu\n",
		dnet_dump_id(&cmd->id), (unsigned long long)cmd->size, sizeof(struct dnet_check_request));
		return -EINVAL;
	}

	r = (struct dnet_check_request *)(cmd + 1);
	dnet_convert_check_request(r);

	n->check_in_progress = 1;

	if (!r->thread_num)
		r->thread_num = 50;

	memcpy(&req, r, sizeof(req));

	memset(&ctl, 0, sizeof(struct dnet_db_list_control));

	atomic_init(&ctl.completed, 0);
	atomic_init(&ctl.errors, 0);
	atomic_init(&ctl.total, 0);

	ctl.n = n;
	ctl.st = st;
	ctl.cmd = cmd;
	ctl.req = &req;
	ctl.params.db = dnet_check_temp_db_alloc(n, n->temp_meta_env);
	if (!ctl.params.db) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	if (req.timestamp) {
		localtime_r((time_t *)&req.timestamp, &tm);
		strftime(ctl_time, sizeof(ctl_time), "%F %R:%S %Z", &tm);
	} else {
		snprintf(ctl_time, sizeof(ctl_time), "all records");
	}

	if (req.obj_num > 0)
		req.thread_num = 1;

	dnet_log(n, DNET_LOG_INFO, "CHECK: Started %u checking threads, recovering %llu transactions, "
			"which started before %s: merge: %d, full: %d, dry: %d.\n",
			req.thread_num, (unsigned long long)req.obj_num, ctl_time,
			!!(req.flags & DNET_CHECK_MERGE), !!(req.flags & DNET_CHECK_FULL),
			!!(req.flags & DNET_CHECK_DRY_RUN));

	if (req.group_num) {
		int *groups;
		char str[req.group_num*36+1], *ptr;
		int rest;
		uint32_t i;

		groups = (int *)((char *)(r) + sizeof(struct dnet_check_request) + r->obj_num * sizeof(struct dnet_id));

		ptr = str;
		rest = sizeof(ptr);
		for (i = 0; i < req.group_num; ++i) {
			err = snprintf(ptr, rest, "%d:", groups[i]);
			if (err > rest)
				break;

			rest -= err;
			ptr += err;
		}

		*(--ptr) = '\0';
			
		dnet_log(n, DNET_LOG_INFO, "CHECK: groups will be overrided with: %s\n", str);

		ctl.params.group_num = req.group_num;
		ctl.params.groups = groups;
	}

	dnet_ioprio_set(dnet_get_id(), n->bg_ionice_class, n->bg_ionice_prio);

	if (req.obj_num > 0) {
		struct dnet_id *ids = (struct dnet_id *)(r + 1);
		struct eblob_iterate_control iter_ctl;
		struct eblob_disk_control dc;
		struct eblob_ram_control rc;
		struct dnet_raw_id id;
		void *data;
		void *priv = NULL;
		int err;
		uint32_t i;

		memset(&dc, 0, sizeof(struct eblob_disk_control));
		memset(&rc, 0, sizeof(struct eblob_ram_control));

		iter_ctl.thread_num = 1;
		iter_ctl.priv = &ctl;
		dnet_db_list_iter_init(&iter_ctl, &priv);

		for (i = 0; i < req.obj_num; ++i) {
			memcpy(&id.id, &ids[i].id, DNET_ID_SIZE);
			err = n->cb->meta_read(n->cb->command_private, &id, &data);
			if (err > 0) {
				rc.size = err;
				memcpy(&dc.key.id, &ids[i].id, DNET_ID_SIZE);
				err = dnet_db_list_iter(&dc, &rc, data, &ctl, priv);
			}
		}
		dnet_db_list_iter_free(&iter_ctl, &priv);

	} else {
		struct dnet_iterate_ctl dctl;

		memset(&dctl, 0, sizeof(struct dnet_iterate_ctl));

		dctl.iterate_private = n->cb->command_private;
		dctl.flags = 0;
		dctl.blob_start = req.blob_start;
		dctl.blob_num = req.blob_num;
		dctl.callback_private = &ctl;

		dctl.iterate_cb.iterator = dnet_db_list_iter;
		dctl.iterate_cb.iterator_init = dnet_db_list_iter_init;
		dctl.iterate_cb.iterator_free = dnet_db_list_iter_free;
		dctl.iterate_cb.thread_num = req.thread_num;

		err = n->cb->meta_iterate(&dctl);
	}

	if(r->flags & DNET_CHECK_MERGE) {
		dnet_counter_set(n, DNET_CNTR_NODE_LAST_MERGE, 0, atomic_read(&ctl.completed));
		dnet_counter_set(n, DNET_CNTR_NODE_LAST_MERGE, 1, atomic_read(&ctl.errors));
	}

	dnet_db_send_check_reply(&ctl);

	dnet_check_temp_db_put(ctl.params.db);

err_out_exit:
	n->check_in_progress = 0;
	return err;
}


