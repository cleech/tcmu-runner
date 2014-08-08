/*
 * Copyright 2014, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
*/

#define _GNU_SOURCE
#define _BITS_UIO_H
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <dlfcn.h>
#include <scsi/scsi.h>
#include <pthread.h>
#include <signal.h>

#include <libnl3/netlink/genl/genl.h>
#include <libnl3/netlink/genl/mngt.h>
#include <libnl3/netlink/genl/ctrl.h>
#include "../kernel/drivers/target/target_core_user.h"
#include "darray.h"
#include "tcmu-runner.h"

#define ARRAY_SIZE(X) (sizeof(X) / sizeof((X)[0]))

#define HANDLER_PATH "."

darray(struct tcmu_device) devices = darray_new();

darray(struct tcmu_handler) handlers = darray_new();

struct tcmu_thread {
	pthread_t thread_id;
	char dev_name[16]; /* e.g. "uio14" */
};

darray(struct tcmu_thread) threads = darray_new();

static struct nla_policy tcmu_attr_policy[TCMU_ATTR_MAX+1] = {
	[TCMU_ATTR_DEVICE]	= { .type = NLA_STRING },
	[TCMU_ATTR_MINOR]	= { .type = NLA_U32 },
};

int add_device(char *dev_name, char *cfgstring);
void remove_device(char *dev_name, char *cfgstring);

static int handle_netlink(struct nl_cache_ops *unused, struct genl_cmd *cmd,
			  struct genl_info *info, void *arg)
{
	char buf[32];

	if (!info->attrs[TCMU_ATTR_MINOR] || !info->attrs[TCMU_ATTR_DEVICE]) {
		printf("TCMU_ATTR_MINOR or TCMU_ATTR_DEVICE not set, doing nothing\n");
		return 0;
	}

	snprintf(buf, sizeof(buf), "uio%d", nla_get_u32(info->attrs[TCMU_ATTR_MINOR]));

	switch (cmd->c_id) {
	case TCMU_CMD_ADDED_DEVICE:
		add_device(buf, nla_get_string(info->attrs[TCMU_ATTR_DEVICE]));
		break;
	case TCMU_CMD_REMOVED_DEVICE:
		remove_device(buf, nla_get_string(info->attrs[TCMU_ATTR_DEVICE]));
		break;
	default:
		printf("Unknown notification %d\n", cmd->c_id);
	}

	return 0;
}

static struct genl_cmd tcmu_cmds[] = {
	{
		.c_id		= TCMU_CMD_ADDED_DEVICE,
		.c_name		= "ADDED DEVICE",
		.c_msg_parser	= handle_netlink,
		.c_maxattr	= TCMU_ATTR_MAX,
		.c_attr_policy	= tcmu_attr_policy,
	},
	{
		.c_id		= TCMU_CMD_REMOVED_DEVICE,
		.c_name		= "REMOVED DEVICE",
		.c_msg_parser	= handle_netlink,
		.c_maxattr	= TCMU_ATTR_MAX,
		.c_attr_policy	= tcmu_attr_policy,
	},
};

static struct genl_ops tcmu_ops = {
	.o_name		= "TCM-USER",
	.o_cmds		= tcmu_cmds,
	.o_ncmds	= ARRAY_SIZE(tcmu_cmds),
};

struct nl_sock *setup_netlink(void)
{
	struct nl_sock *sock;
	int ret;

	sock = nl_socket_alloc();
	if (!sock) {
		printf("couldn't alloc socket\n");
		exit(1);
	}

	nl_socket_disable_seq_check(sock);

	nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, genl_handle_msg, NULL);

	ret = genl_connect(sock);
	if (ret < 0) {
		printf("couldn't connect\n");
		exit(1);
	}

	ret = genl_register_family(&tcmu_ops);
	if (ret < 0) {
		printf("couldn't register family\n");
		exit(1);
	}

	ret = genl_ops_resolve(sock, &tcmu_ops);
	if (ret < 0) {
		printf("couldn't resolve ops, is target_core_user.ko loaded?\n");
		exit(1);
	}

	ret = genl_ctrl_resolve_grp(sock, "TCM-USER", "config");

	printf("multicast id %d\n", ret);

	ret = nl_socket_add_membership(sock, ret);
	if (ret < 0) {
		printf("couldn't add membership\n");
		exit(1);
	}

	return sock;
}

int is_handler(const struct dirent *dirent)
{
	if (strncmp(dirent->d_name, "handler_", 8))
		return 0;

	return 1;
}

int open_handlers(void)
{
	struct dirent **dirent_list;
	int num_handlers;
	int num_good = 0;
	int i;

	num_handlers = scandir(HANDLER_PATH, &dirent_list, is_handler, alphasort);

	if (num_handlers == -1)
		return -1;

	for (i = 0; i < num_handlers; i++) {
		char *path;
		void *handle;
		struct tcmu_handler *tcmu_handler;
		int ret;

		ret = asprintf(&path, "%s/%s", HANDLER_PATH, dirent_list[i]->d_name);
		free(dirent_list[i]);
		if (ret == -1) {
			printf("ENOMEM\n");
			continue;
		}

		handle = dlopen(path, RTLD_NOW|RTLD_LOCAL);
		if (!handle) {
			printf("Could not open handler at %s\n", path);
			free(path);
			continue;
		}

		tcmu_handler = dlsym(handle, "handler_struct");
		if (!tcmu_handler) {
			printf("dlsym failure on %s\n", path);
			free(path);
			continue;
		}

		darray_append(handlers, *tcmu_handler);

		free(path);

		num_good++;
	}

	free(dirent_list);

	return num_good;
}

int is_uio(const struct dirent *dirent)
{
	int fd;
	char tmp_path[64];
	char buf[256];
	ssize_t ret;

	if (strncmp(dirent->d_name, "uio", 3))
		return 0;

	snprintf(tmp_path, sizeof(tmp_path), "/sys/class/uio/%s/name", dirent->d_name);

	fd = open(tmp_path, O_RDONLY);
	if (fd == -1) {
		printf("could not open %s!\n", tmp_path);
		return 0;
	}

	ret = read(fd, buf, sizeof(buf));
	if (ret <= 0 || ret >= sizeof(buf)) {
		printf("read of %s had issues\n", tmp_path);
		return 0;
	}

	/* we only want uio devices whose name is a format we expect */
	snprintf(tmp_path, sizeof(tmp_path), "tcm-user+%s/", "srv");
	if (strncmp(buf, "tcm-user+", 9))
		return 0;

	return 1;
}

struct tcmu_handler *find_handler(char *cfgstring)
{
	struct tcmu_handler *handler;
	char *subtype;

	subtype = strtok(cfgstring, "/");
	if (!subtype)
		subtype = cfgstring;

	darray_foreach(handler, handlers) {
		if (!strcmp(subtype, handler->subtype))
		    return handler;
	}

	return NULL;
}

int block_size = 4096;

int handle_one_command(struct tcmu_device *dev,
		       struct tcmu_mailbox *mb,
		       struct tcmu_cmd_entry *ent)
{
	uint8_t *cdb;
	int i;

	cdb = (void *)mb + ent->req.cdb_off;

	/* Convert iovec addrs in-place to not be offsets */
	for (i = 0; i < ent->req.iov_cnt; i++)
		ent->req.iov[i].iov_base = (void *) mb + (size_t)ent->req.iov[i].iov_base;

	return dev->handler->cmd_submit(dev, cdb, ent->req.iov);
}

void poke_kernel(int fd)
{
	uint32_t buf = 0xabcdef12;

	printf("poke kernel\n");
	write(fd, &buf, 4);
}

int handle_device_events(struct tcmu_device *dev)
{
	struct tcmu_cmd_entry *ent;
	struct tcmu_mailbox *mb = dev->map;
	int did_some_work = 0;

	ent = (void *) mb + mb->cmdr_off + mb->cmd_tail;

	printf("ent addr1 %p mb %p cmd_tail %u cmd_head %u\n", ent, mb, mb->cmd_tail, mb->cmd_head);

	while (ent != (void *)mb + mb->cmdr_off + mb->cmd_head) {

		if (tcmu_hdr_get_op(&ent->hdr) == TCMU_OP_CMD) {
			printf("handling a command entry, len %d\n", tcmu_hdr_get_len(&ent->hdr));
			if (handle_one_command(dev, mb, ent)) {
				ent->rsp.scsi_status = NO_SENSE;
			}
			else {
				/* Tell the kernel we didn't handle it */
				char *buf = ent->rsp.sense_buffer;

				ent->rsp.scsi_status = CHECK_CONDITION;

				buf[0] = 0x70;	/* fixed, current */
				buf[2] = 0x5;	/* illegal request */
				buf[7] = 0xa;
				buf[12] = 0x20;	/* ASC: invalid command operation code */
				buf[13] = 0x0;	/* ASCQ: (none) */
			}

		}
		else {
			printf("handling a pad entry, len %d\n", tcmu_hdr_get_len(&ent->hdr));
		}

		mb->cmd_tail = (mb->cmd_tail + tcmu_hdr_get_len(&ent->hdr)) % mb->cmdr_size;
		ent = (void *) mb + mb->cmdr_off + mb->cmd_tail;
		printf("ent addr2 %p\n", ent);
		did_some_work = 1;
	}

	if (did_some_work)
		poke_kernel(dev->fd);

	return 0;
}

void thread_cleanup(void *arg)
{
	struct tcmu_device *dev = arg;

	printf("in thread cleanup\n");

	dev->handler->close(dev);
	munmap(dev->map, dev->map_len);
	close(dev->fd);
	free(dev);
}

void *thread_start(void *arg)
{
	struct tcmu_device *dev = arg;

	printf("in thread for dev %s\n", dev->name);

	pthread_cleanup_push(thread_cleanup, dev);

	while (1) {
		char buf[4];
		int ret = read(dev->fd, buf, 4);

		if (ret != 4) {
			printf("read didn't get 4! thread terminating\n");
			break;
		}

		handle_device_events(dev);
	}

	printf("thread terminating, should never happen\n");

	pthread_cleanup_pop(1);

	return NULL;
}

int add_device(char *dev_name, char *cfgstring)
{
	struct tcmu_device *dev;
	struct tcmu_thread thread;
	char str_buf[64];
	int fd;
	int ret;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		printf("calloc failed in add_device\n");
		return -1;
	}

	snprintf(dev->name, sizeof(dev->name), "%s", dev_name);
	snprintf(thread.dev_name, sizeof(thread.dev_name), "%s", dev_name);
	snprintf(dev->cfgstring, sizeof(dev->cfgstring), "%s", cfgstring+9);
	snprintf(str_buf, sizeof(str_buf), "/dev/%s", dev_name);
	printf("dev %s\n", str_buf);

	dev->fd = open(str_buf, O_RDWR);
	if (dev->fd == -1) {
		printf("could not open %s\n", dev->name);
		goto err_free;
	}

	snprintf(str_buf, sizeof(str_buf), "/sys/class/uio/%s/maps/map0/size", dev->name);
	fd = open(str_buf, O_RDONLY);
	if (fd == -1) {
		printf("could not open %s\n", dev->name);
		goto err_fd_close;
	}

	ret = read(fd, str_buf, sizeof(str_buf));
	close(fd);
	if (ret <= 0) {
		printf("could not read size of map0\n");
		goto err_fd_close;
	}

	dev->map_len = strtoull(str_buf, NULL, 0);
	if (dev->map_len == ULLONG_MAX) {
		printf("could not get map length\n");
		goto err_fd_close;
	}

	dev->map = mmap(NULL, dev->map_len, PROT_READ|PROT_WRITE, MAP_SHARED, dev->fd, 0);
	if (dev->map == MAP_FAILED) {
		printf("could not mmap: %m\n");
		goto err_fd_close;
	}

	dev->handler = find_handler(dev->cfgstring);
	if (!dev->handler) {
		printf("could not find handler for %s\n", dev->name);
		goto err_munmap;
	}

	ret = dev->handler->open(dev);
	if (ret < 0) {
		printf("handler open failed for %s\n", dev->name);
		goto err_munmap;
	}

	/* dev will be freed by the new thread */
	ret = pthread_create(&thread.thread_id, NULL, thread_start, dev);
	if (ret) {
		printf("Could not start thread\n");
		goto err_handler_close;
	}

	darray_append(threads, thread);

	return 0;

err_handler_close:
	dev->handler->close(dev);
err_munmap:
	munmap(dev->map, dev->map_len);
err_fd_close:
	close(dev->fd);
err_free:
	free(dev);

	return -1;
}

void cancel_thread(pthread_t thread)
{
	void *join_retval;
	int ret;

	ret = pthread_cancel(thread);
	if (ret) {
		printf("pthread_cancel failed with value %d\n", ret);
		return;
	}

	ret = pthread_join(thread, &join_retval);
	if (ret) {
		printf("pthread_join failed with value %d\n", ret);
		return;
	}

	if (join_retval != PTHREAD_CANCELED)
		printf("unexpected join retval: %p\n", join_retval);
}

void remove_device(char *dev_name, char *cfgstring)
{
	struct tcmu_thread *thread;
	int i = 0;
	bool found = false;

	darray_foreach(thread, threads) {
		if (strncmp(thread->dev_name, dev_name, strnlen(thread->dev_name, sizeof(thread->dev_name))))
			i++;
		else {
			found = true;
			break;
		}
	}

	if (!found) {
		printf("could not remove device %s: not found\n", dev_name);
		return;
	}

	cancel_thread(thread->thread_id);

	darray_remove(threads, i);
}

int open_devices(void)
{
	struct dirent **dirent_list;
	int num_devs;
	int num_good_devs = 0;
	int i;

	num_devs = scandir("/dev", &dirent_list, is_uio, alphasort);

	if (num_devs == -1)
		return -1;

	for (i = 0; i < num_devs; i++) {
		char tmp_path[64];
		char buf[256];
		int fd;
		int ret;

		snprintf(tmp_path, sizeof(tmp_path), "/sys/class/uio/%s/name",
			 dirent_list[i]->d_name);

		fd = open(tmp_path, O_RDONLY);
		if (fd == -1) {
			printf("could not open %s!\n", tmp_path);
			continue;
		}

		ret = read(fd, buf, sizeof(buf));
		close(fd);
		if (ret <= 0 || ret >= sizeof(buf)) {
			printf("read of %s had issues\n", tmp_path);
			continue;
		}

		ret = add_device(dirent_list[i]->d_name, buf);
		if (ret < 0)
			continue;

		num_good_devs++;
	}

	for (i = 0; i < num_devs; i++)
		free(dirent_list[i]);
	free(dirent_list);

	return num_good_devs;
}

void sighandler(int signal)
{
	struct tcmu_thread *thread;

	printf("signal %d received!\n", signal);

	darray_foreach(thread, threads) {
		cancel_thread(thread->thread_id);
	}

	exit(1);
}

struct sigaction tcmu_sigaction = {
	.sa_handler = sighandler,
};

int main()
{
	struct nl_sock *nl_sock;
	int ret;

	nl_sock = setup_netlink();
	if (!nl_sock) {
		printf("couldn't setup netlink\n");
		exit(1);
	}

	ret = open_handlers();
	printf("%d handlers found\n", ret);
	if (ret < 0) {
		printf("couldn't open handlers\n");
		exit(1);
	}
	else if (!ret) {
		printf("No handlers, how's this gonna work???\n");
	}

	ret = open_devices();
	printf("%d devices found\n", ret);
	if (ret < 0) {
		printf("couldn't open devices\n");
		exit(1);
	}

	ret = sigaction(SIGINT, &tcmu_sigaction, NULL);
	if (ret) {
		printf("couldn't set sigaction\n");
		exit(1);
	}

	while (1) {
		ret = nl_recvmsgs_default(nl_sock);
		if (ret < 0) {
			printf("nl_recvmsgs_default poll returned %d", ret);
			exit(1);
		}
	}

	return 0;
}
