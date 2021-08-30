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

static int
elm_type_name2int(const char *name) {
	const char *elm_type_names[] = ELM_TYPE_NAMES;
	int i;

	for (i = 0; i <= ELMTYP_LAST; i++) {
		/* sg_ses uses different case than ses(4) */
		if (0 == strcasecmp(name, elm_type_names[i]))
			return i;
	}
	return (-1);
}

static void do_getelmmap(const char *devname, int fd) {
	encioc_element_t *map;
	FILE *pipe;
	char cmd[256];
	char line[256];
	unsigned elm_idx = 0;
	unsigned nobj, subenc_id;
	int r, elm_type;

	r = ioctl(fd, ENCIOC_GETNELM, (caddr_t) &nobj);
	ATF_REQUIRE_EQ(r, 0);

	map = calloc(nobj, sizeof(encioc_element_t));
	ATF_REQUIRE(map != NULL);
	r = ioctl(fd, ENCIOC_GETELMMAP, (caddr_t) map);

	snprintf(cmd, sizeof(cmd), "sg_ses -p1 %s", devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	while(NULL != fgets(line, sizeof(line), pipe)) {
		char elm_type_name[80];
		int i, num_elm;

		r = sscanf(line,
		    "    Element type: %[a-zA-Z0-9_ /], subenclosure id: %d",
		    elm_type_name, &subenc_id);
		if (r == 2) {
			elm_type = elm_type_name2int(elm_type_name);
			continue;
		}
		r = sscanf(line, "      number of possible elements: %d",
		    &num_elm);
		if (r != 1)
			continue;

		/* Skip the Overall elements */
		elm_idx++;
		for (i = 0; i < num_elm; i++, elm_idx++) {
			ATF_CHECK_EQ(map[elm_idx].elm_idx, elm_idx);
			ATF_CHECK_EQ(map[elm_idx].elm_subenc_id, subenc_id);
			ATF_CHECK_EQ((int)map[elm_idx].elm_type, elm_type);
		}
	}

	ATF_CHECK_EQ_MSG(nobj, elm_idx,
			"Did not find the expected number of element "
			"descriptors in sg_ses's output");
	pclose(pipe);
	free(map);
}

ATF_TC(getelmmap);
ATF_TC_HEAD(getelmmap, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETELMMAP's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_ses");
}
ATF_TC_BODY(getelmmap, tc)
{
	for_each_ses_dev(do_getelmmap, O_RDONLY);
}

static void do_getencid(const char *devname, int fd) {
	encioc_string_t stri;
	FILE *pipe;
	char cmd[256];
	char encid[32];
	char line[256];
	int r;

	snprintf(cmd, sizeof(cmd), "sg_ses -p1 %s "
		"| awk '/enclosure logical identifier/ {printf $NF}'",
		devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	ATF_REQUIRE(NULL != fgets(line, sizeof(line), pipe));

	stri.bufsiz = sizeof(encid);
	stri.buf = &encid[0];
	r = ioctl(fd, ENCIOC_GETENCID, (caddr_t) &stri);
	ATF_REQUIRE_EQ(r, 0);
	ATF_CHECK_STREQ(line, (char*)stri.buf);

	pclose(pipe);
}

ATF_TC(getencid);
ATF_TC_HEAD(getencid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETENCID's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_ses");
}
ATF_TC_BODY(getencid, tc)
{
	for_each_ses_dev(do_getencid, O_RDONLY);
}

static void do_getencname(const char *devname, int fd) {
	encioc_string_t stri;
	FILE *pipe;
	char cmd[256];
	char encname[32];
	char line[256];
	int r;

	snprintf(cmd, sizeof(cmd), "sg_inq -o %s | awk '"
		"/Vendor identification/ {vi=$NF} "
		"/Product identification/ {pi=$NF} "
		"/Product revision level/ {prl=$NF} "
		"END {printf(vi \" \" pi \" \" prl)}'", devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	ATF_REQUIRE(NULL != fgets(line, sizeof(line), pipe));

	stri.bufsiz = sizeof(encname);
	stri.buf = &encname[0];
	r = ioctl(fd, ENCIOC_GETENCNAME, (caddr_t) &stri);
	ATF_REQUIRE_EQ(r, 0);
	ATF_CHECK_STREQ(line, (char*)stri.buf);

	pclose(pipe);
}

ATF_TC(getencname);
ATF_TC_HEAD(getencname, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETENCNAME's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_inq");
}
ATF_TC_BODY(getencname, tc)
{
	for_each_ses_dev(do_getencname, O_RDONLY);
}

static void do_getencstat(const char *devname, int fd) {
	FILE *pipe;
	char cmd[256];
	unsigned char e, estat, invop, info, noncrit, crit, unrecov;
	int r;

	snprintf(cmd, sizeof(cmd), "sg_ses -p2 %s "
		"| grep 'INVOP='",
		devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	r = fscanf(pipe,
	    "  INVOP=%hhu, INFO=%hhu, NON-CRIT=%hhu, CRIT=%hhu, UNRECOV=%hhu",
	    &invop, &info, &noncrit, &crit, &unrecov);
	ATF_REQUIRE_EQ(r, 5);

	r = ioctl(fd, ENCIOC_GETENCSTAT, (caddr_t) &estat);
	ATF_REQUIRE_EQ(r, 0);
	e = (invop << 4) | (info << 3) | (noncrit << 2) | (crit << 1) | unrecov;
	ATF_CHECK_EQ(estat, e);

	pclose(pipe);
}

ATF_TC(getencstat);
ATF_TC_HEAD(getencstat, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETENCSTAT's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_ses");
}
ATF_TC_BODY(getencstat, tc)
{
	for_each_ses_dev(do_getencstat, O_RDONLY);
}

static void do_getnelm(const char *devname, int fd) {
	FILE *pipe;
	char cmd[256];
	char line[256];
	unsigned nobj, expected;
	int r;

	snprintf(cmd, sizeof(cmd), "sg_ses -p1 %s | awk '"
		"/number of possible elements:/ {nelm = nelm + 1 + $NF} "
		"END {print(nelm)}'"
		, devname);
	pipe = popen(cmd, "r");
	ATF_REQUIRE(pipe != NULL);
	ATF_REQUIRE(NULL != fgets(line, sizeof(line), pipe));
	expected = atoi(line);

	r = ioctl(fd, ENCIOC_GETNELM, (caddr_t) &nobj);
	ATF_REQUIRE_EQ(r, 0);
	ATF_CHECK_EQ(expected, nobj);

	pclose(pipe);
}

ATF_TC(getnelm);
ATF_TC_HEAD(getnelm, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Compare ENCIOC_GETNELM's output to sg3_utils'");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "require.progs", "sg_ses");
}
ATF_TC_BODY(getnelm, tc)
{
	for_each_ses_dev(do_getnelm, O_RDONLY);
}

ATF_TP_ADD_TCS(tp)
{

	/*
	 * Untested ioctls:
	 *
	 * * ENCIOC_GETTEXT because it was never implemented
	 *
	 */
	ATF_TP_ADD_TC(tp, getelmdesc);
	ATF_TP_ADD_TC(tp, getelmmap);
	ATF_TP_ADD_TC(tp, getencid);
	ATF_TP_ADD_TC(tp, getencname);
	ATF_TP_ADD_TC(tp, getencstat);
	ATF_TP_ADD_TC(tp, getnelm);
	// TODO ENCIOC_GETELMSTAT
	// TODO ENCIOC_GETELMDEVNAMES
	// TODO ENCIOC_GETSTRING


	return (atf_no_error());
}
