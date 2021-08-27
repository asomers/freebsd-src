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

/* Basic smoke test of the ioctl interface */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <atf-c.h>
#include <fcntl.h>
#include <glob.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <cam/scsi/scsi_enc.h>

typedef void(*ses_cb)(const char *devname, int fd);

// Run a test function on every available ses device
static void
for_each_ses_dev(ses_cb cb, int oflags)
{
	glob_t g;
	int r;
	unsigned i;

	g.gl_pathc = 0;
	g.gl_pathv = NULL;
	g.gl_offs = 0;

	r = glob("/dev/ses*", GLOB_NOSORT, NULL, &g);
	ATF_REQUIRE_EQ(r, 0);
	if (g.gl_pathc == 0)
		atf_tc_skip("No ses devices found");

	for(i = 0; i < g.gl_pathc; i++) {
		int fd;

		fd = open(g.gl_pathv[i], oflags);
		ATF_REQUIRE(fd >= 0);
		cb(g.gl_pathv[i], fd);
		close(fd);
	}

	globfree(&g);
}

static void do_getelmdesc(const char *devname, int fd) {
	regex_t re;
	FILE *pipe;
	char cmd[256];
	char line[256];
	char *actual;
	unsigned nobj;
	unsigned elm_idx = 0;
	int r;

	actual = calloc(UINT16_MAX, sizeof(char));
	ATF_REQUIRE(actual != NULL);
	r = regcomp(&re, "(Overall|Element [0-9]+) descriptor: ", REG_EXTENDED);
	ATF_REQUIRE_EQ(r, 0);

	r = ioctl(fd, ENCIOC_GETNELM, (caddr_t) &nobj);
	ATF_REQUIRE_EQ(r, 0);

	snprintf(cmd, sizeof(cmd), "sg_ses -p7 %s", devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	while(NULL != fgets(line, sizeof(line), pipe)) {
		regmatch_t matches[1];
		encioc_elm_desc_t e_desc;
		char *expected;
		size_t elen;

		if (regexec(&re, line, 1, matches, 0) == REG_NOMATCH) {
			/*printf("no match: %s\n", line);*/
			continue;
		}

		expected = &line[matches[0].rm_eo];
		/* Remove trailing newline */
		elen = strnlen(expected, sizeof(line) - matches[0].rm_eo);
		expected[elen - 1] = '\0';
		/* 
		 * Zero the result string.  XXX we wouldn't have to do this if
		 * the kernel would nul-terminate the result.
		 */
		memset(actual, 0, UINT16_MAX);
		e_desc.elm_idx = elm_idx;
		e_desc.elm_desc_len = UINT16_MAX;
		e_desc.elm_desc_str = actual;
		r = ioctl(fd, ENCIOC_GETELMDESC, (caddr_t) &e_desc);
		ATF_REQUIRE_EQ(r, 0);
		ATF_CHECK_STREQ(expected, actual);
		elm_idx++;
	}

	ATF_CHECK_EQ_MSG(nobj, elm_idx,
			"Did not find the expected number of element "
			"descriptors in sg_ses's output");
	pclose(pipe);
	regfree(&re);
	free(actual);
}

ATF_TC(getelmdesc);
ATF_TC_HEAD(getelmdesc, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETELMDESC's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_ses");
}
ATF_TC_BODY(getelmdesc, tc)
{

	for_each_ses_dev(do_getelmdesc, O_RDONLY);
}

static void do_getencname(const char *devname __unused, int fd) {
	encioc_string_t stri;
	int r;
	char str[32];

	stri.bufsiz = sizeof(str);
	stri.buf = &str[0];
	r = ioctl(fd, ENCIOC_GETENCNAME, (caddr_t) &stri);
	ATF_REQUIRE_EQ(r, 0);
	printf("Enclosure Name: %s\n", stri.buf);
}

ATF_TC_WITHOUT_HEAD(getencname);
ATF_TC_BODY(getencname, tc)
{

	for_each_ses_dev(do_getencname, O_RDONLY);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, getelmdesc);
	ATF_TP_ADD_TC(tp, getencname);

	return (atf_no_error());
}
