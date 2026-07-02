/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 voidanix <voidanix@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libgeom.h>
#include <geom/zoned/g_zoned.h>

#include "core/geom.h"
#include "misc/subr.h"

uint32_t lib_version = G_LIB_VERSION;
uint32_t version = G_ZONED_VERSION;

#define GZONED_ZONESIZE		"256M"
#define GZONED_NONSEQZONESIZE	"1M"

static void zoned_main(struct gctl_req *req, unsigned flags);
static void zoned_create(struct gctl_req *req);
static void zoned_destroy(struct gctl_req *req);

struct g_command class_commands[] = {
	{ "create", G_FLAG_VERBOSE | G_FLAG_LOADKLD, zoned_main,
	    { { 's', "zonesize", GZONED_ZONESIZE, G_TYPE_NUMBER },
		/*
		 * TODO: add the possibility of adding/configuring a non-seq
		 * zone at the beginning of the drive. Used for GPT label.
		 *
		 * Breaks several assumptions like all zones of same size...
		 */
		{ 'r', "nonseqzonesize", GZONED_NONSEQZONESIZE, G_TYPE_NUMBER },
		G_OPT_SENTINEL },
	    "[-v] -r nonseqzonesize -s zonesize name dev" },
	{ "destroy", G_FLAG_VERBOSE, zoned_main, G_NULL_OPTS, "[-v] dev ..." },
	G_CMD_SENTINEL
};

static int verbose = 0;

static void
zoned_main(struct gctl_req *req, unsigned flags)
{
	const char *name;

	if ((flags & G_FLAG_VERBOSE) != 0)
		verbose = 1;

	name = gctl_get_ascii(req, "verb");
	if (name == NULL) {
		gctl_error(req, "No '%s' argument.", "verb");
		return;
	}
	if (strcmp(name, "create") == 0)
		zoned_create(req);
	else if (strcmp(name, "destroy") == 0)
		zoned_destroy(req);
	else
		gctl_error(req, "Unknown command: %s.", name);
}

static void
zoned_create(struct gctl_req *req)
{
	struct g_zoned_metadata md;
	u_char sector[512];
	const char *name, *dev;
	off_t msize, zonesize;
	unsigned int secsize;
	uint32_t nzones;
	int error, nargs;

	bzero(sector, sizeof(sector));
	bzero(&md, sizeof(md));
	nargs = gctl_get_int(req, "nargs");
	if (nargs != 2) {
		gctl_error(req,
		    "Usage: create -s zonesize -r nonseqzonesize name dev");
		return;
	}
	zonesize = (off_t)gctl_get_intmax(req, "zonesize");
	/* TODO: not implemented yet */
	/* nonseqzonesize = (off_t)gctl_get_intmax(req, "nonseqzonesize"); */
	name = gctl_get_ascii(req, "arg0");
	dev = gctl_get_ascii(req, "arg1");

	msize = g_get_mediasize(dev);
	secsize = g_get_sectorsize(dev);
	if (msize == 0 || secsize == 0) {
		gctl_error(req, "Can't get information about %s: %s.", dev,
		    strerror(errno));
		return;
	}
	if (zonesize <= 0 || (zonesize % secsize) != 0) {
		gctl_error(req, "Zone size must be a positive multiple of %u.",
		    secsize);
		return;
	}
	nzones = g_zoned_nzones(msize, zonesize, secsize);
	if (nzones == 0) {
		gctl_error(req, "Zone size %jd is too large for %s.",
		    (intmax_t)zonesize, dev);
		return;
	}

	strlcpy(md.md_magic, G_ZONED_MAGIC, sizeof(md.md_magic));
	md.md_version = G_ZONED_VERSION;
	strlcpy(md.md_name, name, sizeof(md.md_name));
	md.md_id = arc4random();
	md.md_zonesize = zonesize;
	md.md_nzones = nzones;
	md.md_sectorsize = secsize;
	md.md_provsize = msize;

	zoned_metadata_encode(&md, sector);
	error = g_metadata_store(dev, sector, sizeof(sector));
	if (error != 0) {
		gctl_error(req, "Can't store metadata on %s: %s.", dev,
		    strerror(error));
		return;
	}
	if (verbose)
		printf("Device %s zoned: %u zones of %jd bytes.\n", dev, nzones,
		    (intmax_t)zonesize);
}

static void
zoned_destroy(struct gctl_req *req)
{
	const char *name;
	int error, i, nargs;

	nargs = gctl_get_int(req, "nargs");
	if (nargs < 1) {
		gctl_error(req, "Missing device(s).");
		return;
	}

	for (i = 0; i < nargs; i++) {
		name = gctl_get_ascii(req, "arg%d", i);
		error = g_metadata_clear(name, G_ZONED_MAGIC);
		if (error != 0) {
			fprintf(stderr, "Can't clear metadata on %s: %s.\n",
			    name, strerror(error));
			gctl_error(req, "Not fully done.");
			continue;
		}
		if (verbose)
			printf("Metadata cleared on %s.\n", name);
	}
}
