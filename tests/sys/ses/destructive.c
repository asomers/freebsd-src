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

/* Tests that alter an enclosure's state */

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

#define	ENCSTAT_LINK	"encstat_link"

// Run a test function on every available ses device
/*static void*/
/*for_each_ses_dev(ses_cb cb)*/
/*{*/
	/*glob_t g;*/
	/*int r;*/
	/*unsigned i;*/

	/*g.gl_pathc = 0;*/
	/*g.gl_pathv = NULL;*/
	/*g.gl_offs = 0;*/

	/*r = glob("/dev/ses*", GLOB_NOSORT, NULL, &g);*/
	/*ATF_REQUIRE_EQ(r, 0);*/
	/*if (g.gl_pathc == 0)*/
		/*atf_tc_skip("No ses devices found");*/

	/*for(i = 0; i < g.gl_pathc; i++) {*/
		/*int fd;*/

		/*fd = open(g.gl_pathv[i], O_RDWR);*/
		/*ATF_REQUIRE(fd >= 0);*/
		/*cb(g.gl_pathv[i], fd);*/
		/*close(fd);*/
	/*}*/

	/*globfree(&g);*/
/*}*/

// Run a test function on just one ses device
static void
for_one_ses_dev(ses_cb cb)
{
	glob_t g;
	int fd, r;

	g.gl_pathc = 0;
	g.gl_pathv = NULL;
	g.gl_offs = 0;

	r = glob("/dev/ses*", GLOB_NOSORT, NULL, &g);
	ATF_REQUIRE_EQ(r, 0);
	if (g.gl_pathc == 0)
		atf_tc_skip("No ses devices found");

	fd = open(g.gl_pathv[0], O_RDWR);
	ATF_REQUIRE(fd >= 0);
	cb(g.gl_pathv[0], fd);
	close(fd);

	globfree(&g);
}

static void do_setencstat(const char *devname __unused, int fd) {
	unsigned char initial, commanded, altered;
	int r;
	char buf[80];

	r = ioctl(fd, ENCIOC_GETENCSTAT, (caddr_t) &initial);
	ATF_REQUIRE_EQ(r, 0);

	/* Store the initial state in a symlink for future cleanup */
	snprintf(buf, sizeof(buf), "%u", initial);
	ATF_REQUIRE_EQ(0, symlink(buf, ENCSTAT_LINK));
	
	/* Flip the info bit */
	commanded = initial ^= 0x80;
	r = ioctl(fd, ENCIOC_SETENCSTAT, (caddr_t) &commanded);
	ATF_REQUIRE_EQ(r, 0);

	/* Check that the status has changed */
	r = ioctl(fd, ENCIOC_GETENCSTAT, (caddr_t) &altered);
	ATF_REQUIRE_EQ(r, 0);
	ATF_CHECK_EQ(commanded, altered);


}

static void do_setencstat_cleanup(const char *devname __unused, int fd) {
	int n;
	char buf[80];
	unsigned char encstat;

	n = readlink(ENCSTAT_LINK, buf, sizeof(buf) - 1);
	encstat = atoi(buf);
	ioctl(fd, ENCIOC_SETENCSTAT, (caddr_t) &encstat);
}

ATF_TC_WITH_CLEANUP(setencstat);
ATF_TC_HEAD(setencstat, tc)
{
	atf_tc_set_md_var(tc, "descr", "Exercise ENCIOC_SETENCSTAT");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(setencstat, tc)
{
	// XXX Need to test this on an enclosure that supports it.
	for_one_ses_dev(do_setencstat);
}
ATF_TC_CLEANUP(setencstat, tc)
{
	for_one_ses_dev(do_setencstat_cleanup);
}

ATF_TP_ADD_TCS(tp)
{

	/*
	 * Untested ioctls:
	 *
	 * * ENCIOC_INIT because SES doesn't need it and I don't have any
	 *   SAF-TE devices.
	 *
	 * * ENCIOC_SETSTRING because it's seriously unsafe!  It's normally
	 *   used for stuff like firmware updates
	 */
	ATF_TP_ADD_TC(tp, setencstat);
	// TODO ENCIOC_SETELMSTAT

	return (atf_no_error());
}
