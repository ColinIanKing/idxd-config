// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include "dsa.h"

#define DSA_TEST_SIZE 20000
#define SHARED 1
#define DEDICATED 0

static void usage(void)
{
	printf("<app_name> [options]\n"
	"-w <wq_type> ; 0=dedicated, 1=shared\n"
	"-l <length>  ; total test buffer size\n"
	"-f <flags_mask> ; bit 0=block-on-fault, 1=block-in-user,\n"
	"                ; 2=reserved, 3=prefault buffers\n"
	"-o <opcode>     ; opcode, same value as in DSA spec\n"
	"-b <opcode> ; if batch opcode, opcode in the batch\n"
	"-c <batch_size> ; if batch opcode, number of descriptors for batch\n"
	"-t <ms timeout> ; ms to wait for descs to complete\n"
	"-h              ; print this message\n");
}

static int test_batch(struct dsa_context *ctx, size_t buf_size,
				int flags, uint32_t bopcode, unsigned int bsize)
{
	struct dsa_ring_ent *desc;
	unsigned long dflags;
	int rc = 0;
	unsigned int remaining;
	struct dsa_batch *batch;
	uint32_t total_size;
	unsigned int i;
	uint64_t pat_val = 0xcdef090234872389;
	uint8_t *mc_src = NULL, *mc_dest = NULL;
	uint8_t *mc_orig_src = NULL, *mc_orig_dest = NULL;
	uint64_t *ms_dest = NULL, *ms_orig_dest = NULL;
	uint8_t *c_src1 = NULL, *c_src2 = NULL;
	uint8_t *c_orig_src1 = NULL, *c_orig_src2 = NULL;
	uint64_t *cv_src = NULL, *cv_orig_src = NULL;
	uint8_t *dc_src = NULL, *dc_dest1 = NULL;
	uint8_t *dc_dest2 = NULL, *dc_orig_src = NULL;
	uint8_t *dc_orig_dest1 = NULL, *dc_orig_dest2 = NULL;
	uint32_t mc_buf_size, ms_buf_size, c_buf_size;
	uint32_t cv_buf_size, dc_buf_size;
	dsa_completion_t c;
	struct dsa_hw_desc *hw;
	struct dsa_completion_record *comp;

	mc_buf_size = ms_buf_size = c_buf_size = cv_buf_size =
		dc_buf_size = 0;

	if (bopcode == DSA_OPCODE_BATCH) {
		fprintf(stderr, "Can't have batch op inside batch op\n");
		return -EINVAL;
	}

	total_size = buf_size * bsize;

	printf("batch: len %lx flags %x bopcode %x op_sz %lx batch_no %d\n",
			buf_size, flags, bopcode, buf_size, bsize);

	desc = dsa_reserve_space(ctx, 1);
	if (!desc)
		return -ENOMEM;

	batch = dsa_alloc_batch_buffers(ctx, bsize);
	if (!batch)
		return -ENOMEM;

	desc->batch = batch;
	/* allocate buffers for various operations */
	switch (bopcode) {
	case DSA_OPCODE_MEMMOVE: {
		mc_buf_size = total_size;
		mc_orig_src = mc_src = malloc(mc_buf_size);
		if (!mc_src) {
			rc = -ENOMEM;
			goto free_batch;
		}

		mc_orig_dest = mc_dest = malloc(mc_buf_size);
		if (!mc_dest) {
			rc = -ENOMEM;
			free(mc_src);
			goto free_batch;
		}

		if (flags & DSA_FLAGS_PREF)
			memset(mc_dest, 0, mc_buf_size);

		/* Fill in src buffer */
		for (i = 0; i < mc_buf_size; i++)
			mc_src[i] = i;
		break;
	}

	case DSA_OPCODE_MEMFILL: {
		ms_buf_size = total_size;
		ms_orig_dest = ms_dest = malloc(ms_buf_size);
		if (!ms_dest) {
			rc = -ENOMEM;
			goto free_mc;
		}

		if (flags & DSA_FLAGS_PREF)
			memset(ms_dest, 0, ms_buf_size);

		break;
	}

	case DSA_OPCODE_COMPARE: {
		c_buf_size = total_size;
		c_orig_src1 = c_src1 = malloc(c_buf_size);
		if (!c_src1) {
			rc = -ENOMEM;
			goto free_ms;
		}

		c_orig_src2 = c_src2 = malloc(c_buf_size);
		if (!c_src2) {
			free(c_src1);
			rc = -ENOMEM;
			goto free_ms;
		}

		/* Fill in src buffer */
		for (i = 0; i < c_buf_size; i++) {
			c_src1[i] = i;
			c_src2[i] = i;
		}
		break;
	}

	case DSA_OPCODE_COMPVAL: {
		cv_buf_size = total_size;
		cv_orig_src = cv_src = malloc(cv_buf_size);
		if (!cv_src) {
			rc = -ENOMEM;
			goto free_c;
		}

		/* Fill in src buffer */
		if (cv_buf_size < 8) {
			memcpy(cv_src, &pat_val, cv_buf_size);
		} else {
			for (i = 0; i < cv_buf_size / 8; i++)
				cv_src[i] = pat_val;
			remaining = cv_buf_size - (i * 8);
			memcpy(&cv_src[i], &pat_val, remaining);
		}
		break;
	}

	case DSA_OPCODE_DUALCAST: {
		uint32_t dc_aligned_size;

		dc_buf_size = total_size;
		dc_orig_src = dc_src = malloc(dc_buf_size);
		if (!dc_src) {
			rc = -ENOMEM;
			goto free_cv;
		}

		dc_orig_dest1 = dc_dest1 =
			malloc(dc_buf_size * 2 + 0x1000);
		if (!dc_dest1) {
			free(dc_src);
			rc = -ENOMEM;
			goto free_cv;
		}

		/*
		 * dest1 and dest2 lower 12 bits must
		 * be same
		 */
		dc_aligned_size = dc_buf_size;
		if (dc_aligned_size & 0xFFF) {
			dc_aligned_size = dc_buf_size + 4096;
			dc_aligned_size &= ~0xFFF;
		}
		dc_orig_dest2 = dc_dest2 = dc_dest1 +
					dc_aligned_size;

		memset(dc_dest1, 0, dc_buf_size);
		memset(dc_dest2, 0, dc_buf_size);

		/* Fill in src buffer */
		for (i = 0; i < dc_buf_size; i++)
			dc_src[i] = i;

		break;
	}

	default:
		fprintf(stderr, "Invalid or unsupported opcode for batch\n");
		rc = -EINVAL;
		goto free_batch;
	}


	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	if ((flags & DSA_FLAGS_BOF) && ctx->bof)
		dflags |= IDXD_OP_FLAG_BOF;

	switch (bopcode) {
	case DSA_OPCODE_MEMMOVE:
		dsa_prep_batch_memcpy(batch, 0, bsize,
				(uint64_t)mc_dest, (uint64_t)mc_src,
				buf_size, dflags);
		break;

	case DSA_OPCODE_MEMFILL:
		dsa_prep_batch_memset(batch, 0, bsize,
				(uint64_t)ms_dest, pat_val,
				buf_size, dflags);
		break;

	case DSA_OPCODE_COMPARE:
		dsa_prep_batch_compare(batch, 0, bsize,
				(uint64_t)c_src1, (uint64_t)c_src2,
				 buf_size, dflags);
		break;

	case DSA_OPCODE_COMPVAL:
		dsa_prep_batch_compval(batch, 0, bsize,
				pat_val, (uint64_t)cv_src, buf_size, dflags);
		break;
	case DSA_OPCODE_DUALCAST:
		dsa_prep_batch_dualcast(batch, 0, bsize,
				(uint64_t)dc_dest1, (uint64_t)dc_dest2,
				(uint64_t)dc_src, buf_size, dflags);
		break;
	default:
		fprintf(stderr, "Unsupported op %#x\n", bopcode);
		return -EINVAL;
	}

	dflags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	dsa_prep_submit_batch(batch, 0, bsize, desc, dflags);

	rc = dsa_wait_batch(ctx, desc, &c, 0, batch->num_descs);
	if (rc != DSA_STATUS_OK) {
		fprintf(stderr, "batch failed stat %d\n", rc);
		rc = -ENXIO;
		goto free_batch;
	}

	switch (bopcode) {
	case DSA_OPCODE_MEMMOVE:
		/* compare the two buffers */
		if (memcmp(mc_orig_src, mc_orig_dest, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr, "copy test failed\n");
			goto free_batch;
		}

		break;

	case DSA_OPCODE_MEMFILL:
		/* compare the dest buffer */
		if (buf_size < 8) {
			if (memcmp(ms_orig_dest, &pat_val, buf_size)) {
				rc = -ENXIO;
				fprintf(stderr, "memset failed\n");
				goto free_batch;
			}
		} else {
			for (i = 0; i < buf_size / 8; i++) {
				if (ms_orig_dest[i] != pat_val) {
					fprintf(stderr, "memset failed %d\n",
							i * 8);
					rc = -ENXIO;
					goto free_batch;
				}
			}

			remaining = buf_size - i * 8;
			if (memcmp(&ms_orig_dest[i], &pat_val, remaining)) {
				rc = -ENXIO;
				fprintf(stderr, "memset failed %d\n", i * 8);
				goto free_batch;
			}
		}
		break;

	case DSA_OPCODE_DUALCAST:
		/* compare the two buffers */
		if (memcmp(dc_orig_src, dc_orig_dest1, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr, "dualcast fail with dest1\n");
			goto free_batch;
		}

		if (memcmp(dc_orig_src, dc_orig_dest2, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr, "dualcast fail with dest2\n");
			goto free_batch;
		}
		break;
	}

	comp = &batch->comp[0];
	hw = &batch->descs[0];
	for (i = 0; i < batch->num_descs; i++, hw++, comp++) {
		switch (hw->opcode) {
		case DSA_OPCODE_COMPARE:
			if (comp->status == DSA_COMP_SUCCESS
					&& comp->result != 0) {
				rc = -ENXIO;
				fprintf(stderr, "compare mismatch %x\n",
						comp->bytes_completed);
				goto free_batch;
			}
			break;

		case DSA_OPCODE_COMPVAL:
			if (comp->status == DSA_COMP_SUCCESS
					&& comp->result != 0) {
				fprintf(stderr, "compval mismatch %x\n",
						comp->bytes_completed);
				rc = -ENXIO;
				goto free_batch;
			}
			break;
		}
	}

free_batch:
	dsa_free_batch_buffers(batch);

	if (bopcode == DSA_OPCODE_DUALCAST) {
		free(dc_orig_src);
		free(dc_orig_dest1);
	}

free_cv:
	if (bopcode == DSA_OPCODE_COMPVAL)
		free(cv_orig_src);

free_c:
	if (bopcode == DSA_OPCODE_COMPARE) {
		free(c_orig_src1);
		free(c_orig_src2);
	}

free_ms:
	if (bopcode == DSA_OPCODE_MEMFILL)
		free(ms_orig_dest);

free_mc:
	if (bopcode == DSA_OPCODE_MEMMOVE) {
		free(mc_orig_src);
		free(mc_orig_dest);
	}

	return rc;
}

int main(int argc, char *argv[])
{
	struct dsa_context *dsa;
	int i, rc = 0;
	unsigned long buf_size = DSA_TEST_SIZE;
	int wq_type = SHARED;
	struct dsa_ring_ent *desc;
	int opcode = DSA_OPCODE_MEMMOVE;
	int bopcode = DSA_OPCODE_MEMMOVE;
	int flags = DSA_FLAGS_BOF | DSA_FLAGS_BLOCK;
	int opt;
	unsigned int bsize = 0;
	dsa_completion_t c = {0};

	while ((opt = getopt(argc, argv, "w:l:f:o:b:c:t:p:h")) != -1) {
		switch (opt) {
		case 'w':
			wq_type = atoi(optarg);
			break;
		case 'l':
			buf_size = strtoul(optarg, NULL, 0);
			break;
		case 'f':
			flags = strtoul(optarg, NULL, 0);
			break;
		case 'o':
			opcode = strtoul(optarg, NULL, 0);
			break;
		case 'b':
			bopcode = strtoul(optarg, NULL, 0);
			break;
		case 'c':
			bsize = strtoul(optarg, NULL, 0);
			break;
		case 't':
			ms_timeout = strtoul(optarg, NULL, 0);
			break;
		case 'h':
			usage();
			exit(0);
		default:
			break;
		}
	}

	dsa = dsa_init();

	if (dsa == NULL)
		return -ENOMEM;

	rc = dsa_alloc(dsa, wq_type);
	if (rc < 0)
		return -ENOMEM;

	if (buf_size > dsa->max_xfer_size) {
		fprintf(stderr, "invalid transfer size: %lu\n", buf_size);
		return -EINVAL;
	}

	switch (opcode) {
	case DSA_OPCODE_BATCH:
		if (bsize > dsa->max_batch_size || bsize < 2) {
			fprintf(stderr, "invalid num descs: %d\n", bsize);
			rc = -EINVAL;
			goto error;
		}

		rc = test_batch(dsa, buf_size, flags, bopcode, bsize);
		if (rc < 0)
			goto error;
		break;

	case DSA_OPCODE_MEMMOVE: {
		uint32_t *src, *dest;

		printf("memcpy: len %lx flags %x\n", buf_size, flags);

		src = malloc(buf_size);
		if (!src) {
			rc = -ENOMEM;
			goto error;
		}

		dest = malloc(buf_size);
		if (!dest) {
			rc = -ENOMEM;
			free(src);
			goto error;
		}

		if (flags & DSA_FLAGS_PREF)
			memset(dest, 0, buf_size);

		/* Fill in src buffer */
		for (i = 0; (unsigned long)i < buf_size/4; i++)
			src[i] = i;

		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_memcpy(dsa, dest, src, buf_size, flags, &c);
		else {
			desc = dsa_memcpy_nb(dsa, dest, src, buf_size, flags);
			rc = dsa_wait_memcpy(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "memcpy failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		/* compare the two buffers */
		if (memcmp(src, dest, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr, "copy test failed compare\n");
		}

		free(src);
		free(dest);
		break;
	}

	case DSA_OPCODE_MEMFILL: {
		uint8_t *dest;
		uint64_t val = 0xcdef090234872389;

		printf("memset: len %lx flags %x\n", buf_size, flags);

		dest = malloc(buf_size);
		if (!dest) {
			rc = -ENOMEM;
			goto error;
		}

		if (flags & DSA_FLAGS_PREF)
			memset(dest, 0, buf_size);

		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_memset(dsa, dest, val, buf_size, flags, &c);
		else {
			desc = dsa_memset_nb(dsa, dest, val, buf_size, flags);
			rc = dsa_wait_memset(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "memset failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		/* compare the dest buffer */
		for (i = 0; (unsigned long)i < buf_size; i += 8) {
			if (*(uint64_t *)&dest[i] != val) {
				fprintf(stderr, "memset test failed %d\n", i);
				rc = -ENXIO;
				break;
			}
		}

		free(dest);
		break;
	}

	case DSA_OPCODE_COMPARE: {
		uint32_t *src1;
		uint32_t *src2;

		src1 = malloc(buf_size);
		if (!src1) {
			rc = -ENOMEM;
			goto error;
		}

		src2 = malloc(buf_size);
		if (!src2) {
			free(src1);
			rc = -ENOMEM;
			goto error;
		}

		/* Fill in src buffer */
		for (i = 0; (unsigned long)i < buf_size/4; i++) {
			src1[i] = i;
			src2[i] = i;
		}

		printf("compare: matching buffers len %lx flags %x\n",
					buf_size, flags);
		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_compare(dsa, src1, src2, buf_size,
					flags, &c);
		else {
			desc = dsa_compare_nb(dsa, src1, src2,
					buf_size, flags);
			rc = dsa_wait_compare(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "compare1 failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		if (c.status == DSA_COMP_SUCCESS && c.result != 0) {
			fprintf(stderr, "compare failed %d\n",
					c.bytes_completed);
			rc = -ENXIO;
		}

		printf("DSA says matching buffers as expected\n");
		printf("creating a diff at %lx\n", buf_size/2);
		src1[buf_size/8] = 0;

		memset(&c, 0, sizeof(c));
		if (flags & DSA_FLAGS_BLOCK) {
			rc = dsa_compare(dsa, src1, src2, buf_size,
					flags, &c);
		} else {
			desc = dsa_compare_nb(dsa, src1, src2,
					buf_size, flags);
			rc = dsa_wait_compare(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "compare2 failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		if (c.status == DSA_COMP_SUCCESS && c.result != 0)
			fprintf(stderr, "compare mismatch at %x\n",
					c.bytes_completed);
		else
			fprintf(stderr, "DSA wrongly says matching buffers\n");

		free(src1);
		free(src2);
		break;
	}

	case DSA_OPCODE_COMPVAL: {
		uint64_t *src;
		uint64_t val = 0xcdef090234872389;

		src = malloc(buf_size);
		if (!src) {
			rc = -ENOMEM;
			goto error;
		}

		/* Fill in src buffer */
		for (i = 0; (unsigned long)i < buf_size/8; i++)
			src[i] = val;

		printf("compval: matching buffer len %lx flags %x\n",
					buf_size, flags);
		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_compval(dsa, val, src, buf_size, flags, &c);
		else {
			desc = dsa_compval_nb(dsa, val, src, buf_size, flags);
			rc = dsa_wait_compval(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "compval1 failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		if (c.status == DSA_COMP_SUCCESS && c.result != 0) {
			fprintf(stderr,
				"compval failed %d\n", c.bytes_completed);
			rc = -ENXIO;
		}

		printf("DSA says matching buffer as expected\n");
		printf("creating a diff at %lx\n", buf_size/2);
		src[buf_size/16] = 0;

		memset(&c, 0, sizeof(c));
		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_compval(dsa, val, src, buf_size, flags, &c);
		else {
			desc = dsa_compval_nb(dsa, val, src, buf_size, flags);
			rc = dsa_wait_compval(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			fprintf(stderr, "compval2 failed stat %d\n", rc);
			rc = -ENXIO;
			break;
		}

		if (c.status == DSA_COMP_SUCCESS && c.result != 0)
			fprintf(stderr, "compval mismatch at %x\n",
					c.bytes_completed);
		else
			fprintf(stderr, "DSA wrongly says matching buffer\n");

		free(src);
		break;
	}

	case DSA_OPCODE_DUALCAST: {
		uint32_t *src, *dest1, *dest2;
		uint32_t aligned_size = buf_size;

		printf("dualcast: len %lx flags %x\n", buf_size, flags);

		src = malloc(buf_size);
		if (!src) {
			rc = -ENOMEM;
			goto error;
		}

		dest1 = malloc(buf_size * 2 + 0x1000);
		if (!dest1) {
			rc = -ENOMEM;
			free(src);
			goto error;
		}

		/* dest1 and dest2 lower 12 bits must be same */
		aligned_size = buf_size;
		if (aligned_size & 0xFFF)
			aligned_size = (buf_size + 0x1000) & ~0xFFF;

		dest2 = dest1 + aligned_size/sizeof(uint32_t);

		if (flags & DSA_FLAGS_PREF) {
			memset(dest1, 0, buf_size);
			memset(dest2, 0, buf_size);
		}

		/* Fill in src buffer */
		for (i = 0; (unsigned long)i < buf_size/sizeof(uint32_t); i++)
			src[i] = i;

		if (flags & DSA_FLAGS_BLOCK)
			rc = dsa_dualcast(dsa, dest1, dest2, src,
						buf_size, flags, &c);
		else {
			desc = dsa_dualcast_nb(dsa, dest1, dest2, src,
					buf_size, flags);
			rc = dsa_wait_dualcast(dsa, desc, &c);
		}

		if (rc != DSA_STATUS_OK) {
			rc = -ENXIO;
			fprintf(stderr, "memcpy failed stat %d\n", rc);
			break;
		}

		/* compare the two buffers */
		if (memcmp(src, dest1, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr,
				"copy test fail compare with dest1\n");
		}

		if (memcmp(src, dest2, buf_size)) {
			rc = -ENXIO;
			fprintf(stderr,
				"copy test fail compare with dest2\n");
		}

		free(src);
		free(dest1);
		break;
	}

	default:
		rc = -EINVAL;
		break;
	}

 error:
	dsa_free(dsa);
	return rc;
}
