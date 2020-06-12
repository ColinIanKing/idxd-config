// SPDX-License-Identifier: LGPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <builtin.h>
#include <accfg/libaccel_config.h>
#include <ccan/array_size/array_size.h>
#include <util/parse-options.h>
#include <util/strbuf.h>
#include <util/util.h>
#include <util/main.h>

const char accfg_usage_string[] = "accel-config [--version] [--help] COMMAND [ARGS]";
const char accfg_more_info_string[] =
    "See 'accel-config help COMMAND' for more information on a specific command.\n"
    " accel-config --list-cmds to see all available commands";

static int cmd_version(int argc, const char **argv, void *ctx)
{
	printf("%s\n", VERSION);
	return 0;
}

static int cmd_help(int argc, const char **argv, void *ctx)
{
	const char *const builtin_help_subcommands[] = {
		"enable-workqueue", "disable-workqueue",
		"enable-group", "disable-group", NULL
	};
	struct option builtin_help_options[] = {
		OPT_END(),
	};
	const char *builtin_help_usage[] = {
		"accel-config help [command]",
		NULL
	};

	argc = parse_options_subcommand(argc, argv, builtin_help_options,
					builtin_help_subcommands,
					builtin_help_usage, 0);

	if (!argv[0]) {
		printf("\n usage: %s\n\n", accfg_usage_string);
		printf("\n %s\n\n", accfg_more_info_string);
		return 0;
	}

	return help_show_man_page(argv[0], "accfg", "ACCFG_MAN_VIEWER");
}

static struct cmd_struct commands[] = {
	{"version", cmd_version},
	{"list", cmd_list},
	{"load-config", cmd_config},
	{"save-config",  cmd_save},
	{"help", cmd_help},
	{"disable-device", cmd_disable_device},
	{"enable-device", cmd_enable_device},
	{"disable-wq", cmd_disable_wq},
	{"enable-wq", cmd_enable_wq},
	{"config-device", cmd_config_device},
	{"config-group", cmd_config_group},
	{"config-wq", cmd_config_wq},
	{"config-engine", cmd_config_engine},
	{"create-mdev", cmd_create_mdev},
	{"remove-mdev", cmd_remove_mdev},
#ifdef ENABLE_TEST
	{"test", cmd_test},
#endif
};

int main(int argc, const char **argv)
{
	struct accfg_ctx *ctx;
	int rc;

	/* Look for flags.. */
	argv++;
	argc--;
	main_handle_options(&argv, &argc, accfg_usage_string, commands,
			    ARRAY_SIZE(commands));

	if (argc > 0) {
		if (!prefixcmp(argv[0], "--"))
			argv[0] += 2;
	} else {
		/* The user didn't specify a command; give them help */
		printf("\n usage: %s\n\n", accfg_usage_string);
		printf("\n %s\n\n", accfg_more_info_string);
		return 0;
	}

	rc = accfg_new(&ctx);
	if (rc)
		return 0;
	main_handle_internal_command(argc, argv, ctx, commands,
				     ARRAY_SIZE(commands));
	accfg_unref(ctx);
	fprintf(stderr, "Unknown command: '%s'\n", argv[0]);
	return -EINVAL;
}
