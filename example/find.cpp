/*
 * 2011+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
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

#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sstream>
#include <stdexcept>

#include "elliptics/interface.h"
#include "elliptics/packet.h"
#include "elliptics/cppdef.h"

#include "common.h"

using namespace ioremap::elliptics;

class finder : public session {
	public:
		finder(node &n) : session(n) {}
		virtual ~finder() {}

		void add_remote(char *addr);

		void parse_lookup(const command_result &ret);
		void parse_meta(const command_result &ret);
};

void finder::add_remote(char *addr)
{
	int remote_port, remote_family;
	int err;

	err = dnet_parse_addr(addr, &remote_port, &remote_family);
	if (err < 0) {
		std::ostringstream str;
		str << "Failed to parse addr: " << addr;
		throw std::runtime_error(str.str());
	}

    get_node().add_remote(addr, remote_port, remote_family);
}

void finder::parse_lookup(const command_result &ret)
{
	for (size_t i = 0; i < ret.size(); ++i) {
		const callback_result_entry &data = ret[i];
		struct dnet_cmd *cmd = data.command();

		if (data.size()) {
			struct dnet_file_info *info = NULL;
			char addr_str[128] = "no-address";

			if (data.size() >= sizeof(struct dnet_addr)) {
				struct dnet_addr *addr = data.data<struct dnet_addr>();

				if (cmd->size > sizeof(struct dnet_addr) + sizeof(struct dnet_file_info)) {
					info = (struct dnet_file_info *)(addr + 1);
					dnet_convert_file_info(info);
				}

				dnet_convert_addr(addr);
				dnet_server_convert_dnet_addr_raw(addr, addr_str, sizeof(addr_str));
			}

			std::string route_addr = "failed to get route table";

			try {
				route_addr = lookup_address(cmd->id, cmd->id.group_id);
			} catch (const std::exception &e) {
			}

			if (!info)
				dnet_log_raw(get_node().get_native(), DNET_LOG_DATA, "%s: FIND object: %s: should live at: %s\n",
					dnet_dump_id(&cmd->id), addr_str, route_addr.c_str());
			else
				dnet_log_raw(get_node().get_native(), DNET_LOG_DATA, "%s: FIND-OK object: %s: should live at: %s, "
						"offset: %llu, size: %llu, mode: %llo, path: %s\n",
					dnet_dump_id(&cmd->id), addr_str, route_addr.c_str(),
					(unsigned long long)info->offset, (unsigned long long)info->size,
					(unsigned long long)info->mode, (char *)(info + 1));
		} else {
			if (cmd->status != 0)
				dnet_log_raw(get_node().get_native(), DNET_LOG_DATA, "%s: FIND object: status: %d\n",
						dnet_dump_id(&cmd->id), cmd->status);
		}
	}
}

void finder::parse_meta(const command_result &ret)
{
	for (size_t i = 0; i < ret.size(); ++i) {
		const callback_result_entry &data = ret[i];
		struct dnet_cmd *cmd = data.command();
		char addr_str[128];

		if (data.size() > sizeof(struct dnet_io_attr)) {
			struct dnet_io_attr *io = data.data<struct dnet_io_attr>();

			dnet_convert_io_attr(io);

			dnet_log_raw(get_node().get_native(), DNET_LOG_DATA, "%s: FIND-OK meta: %s: cmd: %s, io size: %llu\n",
					dnet_dump_id(&cmd->id), addr_str, dnet_cmd_string(cmd->cmd),
					(unsigned long long)io->size);

			struct dnet_meta_container mc;
			memset(&mc, 0, sizeof(mc));
			mc.data = io + 1;
			mc.size = io->size;

			memcpy(&mc.id, &cmd->id, sizeof(struct dnet_id));
			dnet_meta_print(get_native(), &mc);
		} else {
			if (cmd->status != 0)
				dnet_log_raw(get_node().get_native(), DNET_LOG_DATA, "%s: FIND meta: %s: status: %d\n",
						dnet_dump_id(&cmd->id), addr_str, cmd->status);
		}
	}
}

static __attribute__ ((noreturn)) void efinder_usage(const char *p)
{
	fprintf(stderr, "Usage: %s <options>\n"
			"  -r addr:port:family            - remote node to connect\n"
			"  -l log                         - log file\n"
			"  -m level                       - log level\n"
			"  -I id                          - object ID\n"
			"  -h                             - this help\n"
			, p);
	exit(-1);
}

int main(int argc, char *argv[])
{
	int ch, err;
	const char *logfile = "/dev/stderr";
	int log_level = DNET_LOG_ERROR;
	char *remote = NULL;
	struct dnet_id raw;
	memset(&raw, 0, sizeof(struct dnet_id));

	while ((ch = getopt(argc, argv, "r:l:m:I:h")) != -1) {
		switch (ch) {
			case 'r':
				remote = optarg;
				break;
			case 'l':
				logfile = optarg;
				break;
			case 'm':
				log_level = strtoul(optarg, NULL, 0);
				break;
			case 'I':
				err = dnet_parse_numeric_id(optarg, raw.id);
				if (err < 0)
					return err;
				break;
			case 'h':
			default:
				efinder_usage(argv[0]);

		}
	}

	if (!remote) {
		fprintf(stderr, "You must specify remote addr and object ID\n");
		efinder_usage(argv[0]);
	}

	try {
		file_logger log(logfile, log_level);
		node n(log);
		finder find(n);

		find.add_remote(remote);

		{
			transport_control ctl(raw, DNET_CMD_LOOKUP,
				DNET_FLAGS_DIRECT | DNET_FLAGS_NEED_ACK | DNET_ATTR_META_TIMES);

			command_result results = find.request_cmd(ctl);
			find.parse_lookup(results);
		}



		{
			transport_control ctl(raw, DNET_CMD_READ,
				DNET_FLAGS_DIRECT | DNET_FLAGS_NEED_ACK);

			struct dnet_io_attr io;
			memset(&io, 0, sizeof(io));
			io.flags = DNET_IO_FLAGS_META;
			memcpy(io.id, raw.id, DNET_ID_SIZE);
			memcpy(io.parent, raw.id, DNET_ID_SIZE);

			ctl.set_data(&io, sizeof(io));

			command_result results = find.request_cmd(ctl);
			find.parse_meta(results);
		}
	} catch (const std::exception &e) {
		std::cerr << e.what() << std::endl;
	}

	return 0;
}

