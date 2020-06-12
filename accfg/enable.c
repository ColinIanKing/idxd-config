/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <json-c/json.h>
#include <libgen.h>
#include <dirent.h>
#include <linux/limits.h>
#include <util/json.h>
#include <util/filter.h>
#include <util/util.h>
#include <util/parse-options.h>
#include <util/strbuf.h>
#include <accfg/libaccel_config.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <accfg.h>
#include "private.h"

enum dev_action {
	DEV_ACTION_ENABLE = 0,
	DEV_ACTION_DISABLE,
};

enum wq_action {
	WQ_ACTION_ENABLE = 0,
	WQ_ACTION_DISABLE,
};

static struct {
	bool verbose;
	bool force;
} param;

static const struct option device_options[] = {
	OPT_BOOLEAN('v', "verbose", &param.verbose, "turn on debug"),
	OPT_END(),
};

static const struct option device_disable_options[] = {
	OPT_BOOLEAN('v', "verbose", &param.verbose, "turn on debug"),
	OPT_BOOLEAN('f', "force", &param.force, "force action"),
	OPT_END(),
};

static int action_disable_device(struct accfg_device *device)
{
	if (!accfg_device_is_active(device)) {
		fprintf(stderr, "%s is in disabled state already, skipping...\n",
			accfg_device_get_devname(device));
		return -EBUSY;
	}

	return accfg_device_disable(device, param.force);
}

static int action_enable_device(struct accfg_device *device)
{
	if (accfg_device_is_active(device)) {
		fprintf(stderr, "%s is in enabled state already, skipping...\n",
			accfg_device_get_devname(device));
		return -EBUSY;
	}

	return accfg_device_enable(device);
}

static int dev_action_switch(struct accfg_device *device,
			     enum dev_action action)
{
	switch (action) {
	case DEV_ACTION_ENABLE:
		return action_enable_device(device);
	case DEV_ACTION_DISABLE:
		return action_disable_device(device);
	default:
		return -EINVAL;
	}
}

static int device_action(int argc, const char **argv, const char *usage,
			 const struct option *options, enum dev_action action,
			 struct accfg_ctx *ctx)
{
	const char *const u[] = {
		usage,
		NULL
	};
	int i, rc = 0, success = 0, fail = 0, fail_reason = 0;
	const char *all = "all";
	enum accfg_device_state state;

	argc = parse_options(argc, argv, options, u, 0);

	if (argc == 0) {
		argc = 1;
		argv = &all;
		usage_with_options(u, options);
	} else {
		for (i = 0; i < argc; i++) {
			if (strcmp(argv[i], "all") == 0) {
				argv[0] = "all";
				argc = 1;
				break;
			}
		}
	}

	for (i = 0; i < argc; i++) {
		int found = 0;
		struct accfg_device *device;

		if (!accfg_device_type_validate(argv[i]))
			return -EINVAL;

		accfg_device_foreach(ctx, device) {
			if (!util_device_filter(device, argv[i]))
				continue;
			found++;

			rc = dev_action_switch(device, action);
			if (rc == 0) {
				/*
				 * Double check if the state of the device
				 * matches with the enable/disable
				 */
				state = accfg_device_get_state(device);
				if (((state == 1) && (action == DEV_ACTION_ENABLE))
				    ||((state == 0) && (action == DEV_ACTION_DISABLE)))
					success++;
				else
					fail++;
			} else if (!fail) {
				fail_reason = rc;
				fprintf(stderr, "failed in %s\n", argv[i]);
			}
		}

		if (!found && param.verbose)
			fprintf(stderr, "no device matches with the name: %s\n",
					argv[i]);
	}

	if (success) {
		if (action == DEV_ACTION_ENABLE)
			fprintf(stderr, "successfully enabled %d device%s\n",
					success, success > 1 ? "s" : "");
		else
			fprintf(stderr, "successfully disabled %d device%s\n",
					success, success > 1 ? "s" : "");
		return success;
	}

	if (fail) {
		if (action == DEV_ACTION_ENABLE)
			fprintf(stderr, "failed to enable %d device%s\n", fail,
				fail > 1 ? "s" : "");
		else
			fprintf(stderr, "failed to disable %d device%s\n", fail,
				fail > 1 ? "s" : "");

		return fail;
	}

	if (fail_reason) {
		fprintf(stderr, "failed due to reason %d\n", fail_reason);
		return fail_reason;
	}

	return -ENXIO;
}

static int action_disable_wq(struct accfg_wq *wq, const char *wq_name)
{
	enum accfg_wq_state wq_state = accfg_wq_get_state(wq);

	if (wq_state == ACCFG_WQ_DISABLED) {
		fprintf(stderr,
			"%s is in disabled mode already, skipping...\n",
			wq_name);
		return -ENXIO;
	} else if (wq_state == ACCFG_WQ_QUIESCING) {
		fprintf(stderr,
			"%s is in quiescing mode, skipping...\n",
			wq_name);
		return -EBUSY;
	}
	return accfg_wq_disable(wq, param.force);
}

static int action_enable_wq(struct accfg_wq *wq, const char *wq_name)
{
	enum accfg_wq_state wq_state = accfg_wq_get_state(wq);

	if (wq_state == ACCFG_WQ_ENABLED) {
		fprintf(stderr,
			"%s is in enabled mode already, skipping...\n",
			wq_name);
		return -ENXIO;
	} else if (wq_state == ACCFG_WQ_QUIESCING) {
		fprintf(stderr,
			"%s is in quiescing mode, skipping...\n",
			wq_name);
		return -EBUSY;
	}
	return accfg_wq_enable(wq);
}

static int wq_action_switch(struct accfg_wq *wq, enum wq_action action,
				const char *wq_name)
{
	switch (action) {
	case WQ_ACTION_ENABLE:
		return action_enable_wq(wq, wq_name);
	case WQ_ACTION_DISABLE:
		return action_disable_wq(wq, wq_name);
	default:
		return -EINVAL;
	}
}

static int wq_action(int argc, const char **argv, const char *usage,
			const struct option *options, enum wq_action action,
			struct accfg_ctx *ctx)
{
	const char *const u[] = {
		usage,
		NULL
	};
	unsigned long dev_id, wq_id;
	int i, rc = 0, success = 0, fail = 0, fail_reason = 0;
	const char *all = "all";
	enum accfg_wq_state state;

	argc = parse_options(argc, argv, options, u, 0);

	if (argc == 0) {
		argc = 1;
		argv = &all;
		usage_with_options(u, options);
	} else {
		for (i = 0; i < argc; i++) {
			if (strcmp(argv[i], "all") == 0) {
				argv[0] = "all";
				argc = 1;
				break;
			}
		}
	}

	for (i = 0; i < argc; i++) {
		struct accfg_device *device;
		struct accfg_wq *wq;
		char dev_name[MAX_DEV_LEN], wq_name[MAX_DEV_LEN];
		int found = 0;

		if (sscanf(argv[i], "%[^/]/wq%lu.%lu", dev_name, &dev_id, &wq_id) != 3) {
			fprintf(stderr, "'%s' is not a valid wq name\n",
				argv[i]);
			return -EINVAL;
		}

		if (!accfg_device_type_validate(dev_name))
			return -EINVAL;

		rc = sprintf(wq_name, "wq%lu.%lu", dev_id, wq_id);
		if (rc < 0)
			return errno;

		accfg_device_foreach(ctx, device) {
			if (!util_device_filter(device, dev_name))
				continue;
			accfg_wq_foreach(device, wq) {
				if (!util_wq_filter(wq, wq_name))
					continue;
				found++;
				rc = wq_action_switch(wq, action, wq_name);
				if (rc == 0) {
					/*
					 * Double check if the state of the
					 * wq matches with the enable/disable
					 */
					state = accfg_wq_get_state(wq);
					if (((state == 1) && (action == WQ_ACTION_ENABLE))
					    ||((state == 0) && (action == WQ_ACTION_DISABLE)))
						success++;
					else
						fail++;
				} else if (!fail) {
					fail_reason = rc;
					fprintf(stderr, "failed in %s\n",
						wq_name);
				}
			}
		}

		if (!found && param.verbose)
			fprintf(stderr, "no wq matches id: %s\n", wq_name);
	}

	if (success) {
		if (action == WQ_ACTION_ENABLE)
			fprintf(stderr, "successfully enabled %d wq%s\n",
					success,
			success > 1 ? "s" : "");
		else
			fprintf(stderr, "successfully disabled %d wq%s\n",
					success,
			success > 1 ? "s" : "");
		return success;
	}

	if (fail) {
		if (action == WQ_ACTION_ENABLE)
			fprintf(stderr, "failed to enable %d wq%s\n",
			fail, fail > 1 ? "s" : "");
		else
			fprintf(stderr, "failed to disable %d wq%s\n",
			fail, fail > 1 ? "s" : "");
		return fail;
	}

	if (fail_reason) {
		fprintf(stderr, "failed due to reason %d\n",
			fail_reason);
		return fail_reason;
	}
	return -ENXIO;
}

int cmd_disable_device(int argc, const char **argv, void *ctx)
{
	char *usage =
	    "accel-config disable-device <accel_basename0> [<accel_basename1>..<accel_basenameN>] [<options>]";
	int count = device_action(argc, argv, usage, device_disable_options,
				  DEV_ACTION_DISABLE, ctx);
	return count >= 0 ? 0 : EXIT_FAILURE;
}

int cmd_enable_device(int argc, const char **argv, void *ctx)
{
	char *usage =
	    "accel-config enable-device <accel_basename0> [<accel_basename1>..<accel_basenameN>] [<options>]";
	int count = device_action(argc, argv, usage, device_options,
				  DEV_ACTION_ENABLE, ctx);
	return count >= 0 ? 0 : EXIT_FAILURE;
}

int cmd_disable_wq(int argc, const char **argv, void *ctx)
{
	char *usage =
	    "accel-config disable-wq <accel_basenameX>/<wqX.0> [<wqX.1>..<wqX.N>] [<options>] X is the device number where wq belongs to";
	int count = wq_action(argc, argv, usage, device_disable_options,
			      WQ_ACTION_DISABLE, ctx);
	return count >= 0 ? 0 : EXIT_FAILURE;
}

int cmd_enable_wq(int argc, const char **argv, void *ctx)
{
	char *usage =
	    "accel-config enable-wq <accel_basenameX>/<wqX.0> [<wqX.1>..<wqX.N>] [<options>] X is the device number where wq belongs to";
	int count = wq_action(argc, argv, usage, device_options,
			      WQ_ACTION_ENABLE, ctx);

	return count >= 0 ? 0 : EXIT_FAILURE;
}
