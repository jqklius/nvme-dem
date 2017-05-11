/*
 * NVMe over Fabrics Distributed Endpoint Manager (NVMe-oF DEM).
 * Copyright (c) 2017 Intel Corporation., Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>

#include "json.h"
#include "tags.h"
#include "common.h"

int refresh_ctrl(char *alias)
{
	struct controller	*ctrl;

	list_for_each_entry(ctrl, ctrl_list, node)
		if (!strcmp(ctrl->alias, alias)) {
			fetch_log_pages(ctrl);
			return 0;
		}

	return -EINVAL;
}

static void check_host(struct subsystem *subsys, json_t *nqn, json_t *array,
		       const char *host_nqn)
{
	json_t			*obj;
	json_t			*access;
	json_t			*iter;
	struct host		*host;
	int			 i, n;

	n = json_array_size(array);
	for (i = 0; i < n; i++) {
		iter = json_array_get(array, i);
		obj = json_object_get(iter, TAG_NQN);
		if (obj && json_equal(nqn, obj)) {
			access = json_object_get(iter, TAG_ACCESS);
			if (access && json_integer_value(access)) {
				host = malloc(sizeof(*host));
				if (!host)
					return;

				memset(host, 0, sizeof(*host));
				host->subsystem = subsys;
				strcpy(host->nqn, host_nqn);
				host->access = json_integer_value(access);
				list_add(&host->node, &subsys->host_list);
			}
		}
	}
}

static void check_hosts(struct subsystem *subsys, json_t *array, json_t *nqn)
{
	json_t			*grp;
	json_t			*iter;
	json_t			*host;
	int			 i, n;

	n = json_array_size(array);
	for (i = 0; i < n; i++) {
		iter = json_array_get(array, i);
		host = json_object_get(iter, TAG_NQN);
		if (unlikely(!host))
			continue;
		grp = json_object_get(iter, TAG_ACL);
		if (grp)
			check_host(subsys, nqn, grp, json_string_value(host));
	}
}

static void check_subsystems(struct controller *ctrl, struct json_context *ctx,
			     json_t *array)
{
	json_t			*obj;
	json_t			*iter;
	json_t			*nqn;
	struct subsystem	*subsys;
	int			 i, n;

	n = json_array_size(array);
	for (i = 0; i < n; i++) {
		iter = json_array_get(array, i);
		nqn = json_object_get(iter, TAG_NQN);
		if (!nqn)
			continue;

		subsys = malloc(sizeof(*subsys));
		if (!subsys)
			return;

		memset(subsys, 0, sizeof(*subsys));
		subsys->ctrl = ctrl;
		strcpy(subsys->nqn, json_string_value(nqn));

		INIT_LIST_HEAD(&subsys->host_list);
		list_add(&subsys->node, &ctrl->subsys_list);

		obj = json_object_get(iter, TAG_ALLOW_ALL);
		if (obj && json_is_integer(obj))
			subsys->access = json_integer_value(obj);

		if (ctx->hosts)
			check_hosts(subsys, ctx->hosts, nqn);
	}
}

static int get_transport_info(json_t *ctrl, char *type, char *fam,
			      char *address, char *port, int *_addr)
{
	json_t			*obj;
	char			*str;
	int			 addr[ADDR_LEN];
	int			 ret;

	obj = json_object_get(ctrl, TAG_TYPE);
	if (!obj)
		goto out;
	str = (char *) json_string_value(obj);
	strncpy(type, str, CONFIG_TYPE_SIZE);

	obj = json_object_get(ctrl, TAG_FAMILY);
	if (!obj)
		goto out;
	str = (char *) json_string_value(obj);
	strncpy(fam, str, CONFIG_FAMILY_SIZE);

	obj = json_object_get(ctrl, TAG_ADDRESS);
	if (!obj)
		goto out;
	str = (char *) json_string_value(obj);

	if (strcmp(fam, "ipv4") == 0)
		ret = ipv4_to_addr(str, addr);
	else if (strcmp(fam, "ipv6") == 0)
		ret = ipv6_to_addr(str, addr);
	else if (strcmp(fam, "fc") == 0)
		ret = fc_to_addr(str, addr);
	else
		goto out;

	if (ret < 0)
		goto out;

	memcpy(_addr, addr, sizeof(addr[0]) * ADDR_LEN);
	strncpy(address, str, CONFIG_ADDRESS_SIZE);

	obj = json_object_get(ctrl, TAG_PORT);
	if (obj)
		sprintf(port, "%*lld", CONFIG_PORT_SIZE,
			json_integer_value(obj));
	else
		sprintf(port, "%d", NVME_RDMA_IP_PORT);

	return 1;
out:
	return 0;
}

static int add_to_ctrl_list(struct json_context *ctx, json_t *grp,
			    json_t *parent)
{
	struct controller	*ctrl;
	json_t			*subgroup;
	json_t			*obj;
	int			 addr[ADDR_LEN];
	char			 address[CONFIG_ADDRESS_SIZE + 1];
	char			 port[CONFIG_PORT_SIZE + 1];
	char			 fam[CONFIG_FAMILY_SIZE + 1];
	char			 type[CONFIG_TYPE_SIZE + 1];
	int			 refresh = 0;

	if (!get_transport_info(grp, type, fam, address, port, addr))
		goto err1;

	ctrl = malloc(sizeof(*ctrl));
	if (!ctrl)
		goto err1;

	memset(ctrl, 0, sizeof(*ctrl));

	INIT_LIST_HEAD(&ctrl->subsys_list);

	list_add(&ctrl->node, ctrl_list);

	obj = json_object_get(parent, TAG_REFRESH);
	if (obj)
		refresh = json_integer_value(obj);

	obj = json_object_get(parent, TAG_ALIAS);
	if (!obj)
		goto err2;

	strncpy(ctrl->alias, (char *) json_string_value(obj),
		MAX_ALIAS_SIZE);

	strncpy(ctrl->trtype, type, CONFIG_TYPE_SIZE);
	strncpy(ctrl->addrfam, fam, CONFIG_FAMILY_SIZE);
	strncpy(ctrl->address, address, CONFIG_ADDRESS_SIZE);
	strncpy(ctrl->port, port, CONFIG_PORT_SIZE);

	memcpy(ctrl->addr, addr, ADDR_LEN);

	ctrl->port_num = atoi(port);
	ctrl->refresh = refresh;

	subgroup = json_object_get(parent, TAG_SUBSYSTEMS);
	if (subgroup)
		check_subsystems(ctrl, ctx, subgroup);

	return 1;
err2:
	free(ctrl);
err1:
	return 0;
}

int build_ctrl_list(void *context)
{
	struct json_context	*ctx = context;
	json_t			*array = ctx->ctrls;
	json_t			*subgroup;
	json_t			*iter;
	int			 i, n;

	if (!array)
		return -1;

	n = json_array_size(array);
	for (i = 0; i < n; i++) {
		iter = json_array_get(array, i);
		subgroup = json_object_get(iter, TAG_TRANSPORT);
		if (subgroup)
			add_to_ctrl_list(ctx, subgroup, iter);
	}

	return 0;
}

static int count_dem_config_files(void)
{
	struct dirent		*entry;
	DIR			*dir;
	int			 filecount = 0;

	dir = opendir(PATH_NVMF_DEM_DISC);
	if (dir != NULL) {
		while ((entry = readdir(dir))) {
			if (!strcmp(entry->d_name, "."))
				continue;
			if (!strcmp(entry->d_name, ".."))
				continue;
			filecount++;
		}
	} else {
		print_err("%s does not exist", PATH_NVMF_DEM_DISC);
		filecount = -ENOENT;
	}

	closedir(dir);

	return filecount;
}

static void read_dem_config(FILE *fid, struct interface *iface)
{
	int			 ret;
	char			 tag[LARGEST_TAG + 1];
	char			 val[LARGEST_VAL + 1];

	ret = parse_line(fid, tag, LARGEST_TAG, val, LARGEST_VAL);
	if (ret)
		return;

	if (strcasecmp(tag, TAG_TYPE) == 0)
		strncpy(iface->trtype, val, CONFIG_TYPE_SIZE);
	else if (strcasecmp(tag, TAG_FAMILY) == 0)
		strncpy(iface->addrfam, val, CONFIG_FAMILY_SIZE);
	else if (strcasecmp(tag, TAG_ADDRESS) == 0)
		strncpy(iface->address, val, CONFIG_ADDRESS_SIZE);
	else if (strcasecmp(tag, TAG_PORT) == 0)
		strncpy(iface->pseudo_target_port, val, CONFIG_PORT_SIZE);
}

static void translate_addr_to_array(struct interface *iface)
{
	int			 mask_bits;
	char			*p;

	if (strcmp(iface->addrfam, "ipv4") == 0)
		mask_bits = ipv4_to_addr(iface->address, iface->addr);
	else if (strcmp(iface->addrfam, "ipv6") == 0)
		mask_bits = ipv6_to_addr(iface->address, iface->addr);
	else if (strcmp(iface->addrfam, "fc") == 0)
		mask_bits = fc_to_addr(iface->address, iface->addr);
	else {
		print_err("unsupported or unspecified address family");
		return;
	}

	if (mask_bits) {
		p = strchr(iface->address, '/');
		p[0] = 0;
	}

	if (!strlen(iface->pseudo_target_port))
		sprintf(iface->pseudo_target_port, "%d", NVME_RDMA_IP_PORT);
}

static int read_dem_config_files(struct interface *iface)
{
	struct dirent		*entry;
	DIR			*dir;
	FILE			*fid;
	char			 config_file[FILENAME_MAX + 1];
	int			 count = 0;
	int			 ret;

	dir = opendir(PATH_NVMF_DEM_DISC);
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, "."))
			continue;

		if (!strcmp(entry->d_name, ".."))
			continue;

		snprintf(config_file, FILENAME_MAX, "%s%s",
			 PATH_NVMF_DEM_DISC, entry->d_name);

		fid = fopen(config_file, "r");
		if (fid != NULL) {
			while (!feof(fid))
				read_dem_config(fid, &iface[count]);

			fclose(fid);

			if ((!strcmp(iface[count].trtype, "")) ||
			    (!strcmp(iface[count].addrfam, "")) ||
			    (!strcmp(iface[count].address, "")))
				print_err("%s: %s.",
					"bad config file. Ignoring interface",
					config_file);
			else {
				translate_addr_to_array(&iface[count]);
				count++;
			}
		} else {
			print_err("Failed to open config file %s", config_file);
			ret = -ENOENT;
			goto out;
		}
	}

	if (count == 0) {
		print_err("No viable interfaces. Exiting");
		ret = -ENODATA;
	}

	ret = 0;
out:
	closedir(dir);
	return ret;
}

void cleanup_controllers(void)
{
	struct controller	*ctrl;
	struct controller	*next_ctrl;
	struct subsystem	*subsys;
	struct subsystem	*next_subsys;
	struct host		*host;
	struct host		*next_host;

	list_for_each_entry_safe(ctrl, next_ctrl, ctrl_list, node) {
		list_for_each_entry_safe(subsys, next_subsys,
					 &ctrl->subsys_list, node) {
			list_for_each_entry_safe(host, next_host,
						 &subsys->host_list, node)
				free(host);

			free(subsys);
		}

		list_del(&ctrl->node);
		free(ctrl);
	}
}

int init_interfaces(void)
{
	struct interface	*table;
	int			 count;
	int			 ret;

	/* Could avoid this if we move to a list */
	count = count_dem_config_files();
	if (count < 0)
		return count;

	table = calloc(count, sizeof(struct interface));
	if (!table)
		return -ENOMEM;

	memset(table, 0, count * sizeof(struct interface));

	ret = read_dem_config_files(table);
	if (ret) {
		free(table);
		return -1;
	}

	interfaces = table;

	return count;
}