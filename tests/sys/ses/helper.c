/*-
 * Copyright (C) 2021 Axcient, Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <err.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cam/scsi/scsi_enc.h>

static void do_getelmdesc(int fd, int idx) {
	encioc_elm_desc_t e_desc;

	memset(&e_desc, 0, sizeof(e_desc));
	e_desc.elm_idx = idx;
	e_desc.elm_desc_len = UINT16_MAX;
	e_desc.elm_desc_str = calloc(UINT16_MAX, sizeof(char));
	if (e_desc.elm_desc_str == NULL)
		err(1, "calloc");
	if (ioctl(fd, ENCIOC_GETELMDESC, (caddr_t) &e_desc) < 0)
		err(1, "ioctl");
	printf("%s\n", e_desc.elm_desc_str);
	free(e_desc.elm_desc_str);
}

static void do_getencid(int fd) {
	encioc_string_t stri;
	char str[32];

	stri.bufsiz = sizeof(str);
	stri.buf = &str[0];
	if (ioctl(fd, ENCIOC_GETENCID, (caddr_t) &stri))
		err(1, "ioctl");
	printf("%s\n", stri.buf);
}

static void do_getencname(int fd) {
	encioc_string_t stri;
	char str[32];

	stri.bufsiz = sizeof(str);
	stri.buf = &str[0];
	if (ioctl(fd, ENCIOC_GETENCNAME, (caddr_t) &stri))
		err(1, "ioctl");
	printf("%s\n", stri.buf);
}

static void do_getencstat(int fd) {
	unsigned char estat;

	if (ioctl(fd, ENCIOC_GETENCSTAT, (caddr_t) &estat))
		err(1, "ioctl");
	/* Print it in the format used by sg_ses */
	printf("  INVOP=%d, INFO=%d, NON-CRIT=%d, CRIT=%d, UNRECOV=%d\n",
		estat >> 4,
		(estat >> 3) && 0x1,
		(estat >> 2) && 0x1,
		(estat >> 1) && 0x1,
		estat && 0x1);
}

static void do_getnelm(int fd) {
	unsigned nobj;

	if (ioctl(fd, ENCIOC_GETNELM, (caddr_t) &nobj))
		err(1, "ioctl");
	printf("%u\n", nobj);
}

int main(int argc __unused, char **argv)
{
	int fd;
	char *dev = argv[1];
	char *cmd = argv[2];

	fd = open(dev, O_RDONLY);
	if (fd < 0)
		err(1, "open");
	if (0 == strcmp(cmd, "ENCIOC_GETELMDESC")) {
		int idx = atoi(argv[3]);
		do_getelmdesc(fd, idx);
	} else if (0 == strcmp(cmd, "ENCIOC_GETENCID"))
		do_getencid(fd);
	else if (0 == strcmp(cmd, "ENCIOC_GETENCNAME"))
		do_getencname(fd);
	else if (0 == strcmp(cmd, "ENCIOC_GETENCSTAT"))
		do_getencstat(fd);
	else if (0 == strcmp(cmd, "ENCIOC_GETNELM"))
		do_getnelm(fd);
	else
		errx(1, "Unknown cmd %s", cmd);
	close(fd);

	return (0);
}
