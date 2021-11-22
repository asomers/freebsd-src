/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2021 Alan Somers
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
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

extern "C" {
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <pthread.h>
#include <semaphore.h>
}

#include "mockfs.hh"
#include "utils.hh"

using namespace testing;

/*
 * "Last Local Modify" bugs
 *
 * This file tests a class of race conditions caused by one thread fetching a
 * file's size with FUSE_LOOKUP while another thread simultaneously modifies it
 * with FUSE_SETATTR, FUSE_WRITE, FUSE_COPY_FILE_RANGE or similar.  It's
 * possible for the second thread to start later yet finish first. If that
 * happens, the first thread must not override the size set by the second
 * thread.  
 *
 * FUSE_GETATTR is not vulnerable to the same race, because it is always called
 * with the vnode lock held.
 *
 * A few other operations like FUSE_LINK can also trigger the same race but
 * with the file's ctime instead of size.  However, the consequences of an
 * incorrect ctime are much less disastrous than an incorrect size, so fusefs
 * does not attempt to prevent such races.
 */

enum Reader {
	VOP_LOOKUP,
	VFS_VGET
};

enum Writer {
	VOP_SETATTR,
	VOP_WRITE,
	VOP_COPY_FILE_RANGE
};

class Nfs: public FuseTest {
public:
virtual void SetUp() {
	if (geteuid() != 0)
                GTEST_SKIP() << "This test requires a privileged user";
	FuseTest::SetUp();
}
};

class Exportable: public Nfs {
public:
virtual void SetUp() {
	m_init_flags = FUSE_EXPORT_SUPPORT;
	Nfs::SetUp();
}
};

//class LastLocalModify: public FuseTest {
class LastLocalModify: public FuseTest, public WithParamInterface<tuple<Reader, Writer>> {
//{};
public:
virtual void SetUp() {
	m_init_flags = FUSE_EXPORT_SUPPORT;
	tuple<Reader, Writer> params;

	params = GetParam();
	if (get<0>(params) == VFS_VGET && geteuid() != 0)
                GTEST_SKIP() << "This test requires a privileged user";

	FuseTest::SetUp();
}
};

static void* setattr0(void* arg) {
	ssize_t r;
	sem_t *sem = (sem_t*) arg;

	sem_wait(sem);
	r = truncate("mountpoint/some_file.txt", 5);
	if (r >= 0)
		return 0;
	else
		return (void*)(intptr_t)errno;
}

TEST_P(LastLocalModify, last_local_modify)
//TEST_F(Exportable, fhstat_during_setattr)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";
	Sequence seq;
	uint64_t ino = 3;
	uint64_t lookup_unique;
	const uint64_t oldsize = 10;
	const uint64_t newsize = 5;
	pthread_t th0;
	void *thr0_value;
	struct stat sb;
	static sem_t sem;
	fhandle_t fhp;
	tuple<Reader, Writer> params;
	uint32_t mutator_op;

	params = GetParam();
	printf("reader=%d writer=%d\n", get<0>(params), get<1>(params));
	switch(get<1>(params)) {
	case VOP_SETATTR:
		mutator_op = FUSE_SETATTR;
		break;
	case VOP_WRITE:
		mutator_op = FUSE_WRITE;
		break;
	default:
		// TODO
		break;
	}

	ASSERT_EQ(0, sem_init(&sem, 0, 0)) << strerror(errno);

	switch(get<0>(params)) {
	case VOP_LOOKUP:
		EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
		.Times(1)
		.InSequence(seq)
		.WillOnce(Invoke([&](auto in, auto &out __unused) {
			/* Called by stat.  Block to simulate a race */
			lookup_unique = in.header.unique;
			sem_post(&sem);
		}));
		break;
	case VFS_VGET:
		EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
		.Times(1)
		.InSequence(seq)
		.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out)
		{
			/* Called by getfh, caches attributes but not entries */
			SET_OUT_HEADER_LEN(out, entry);
			out.body.entry.nodeid = ino;
			out.body.entry.attr.size = oldsize;
			out.body.entry.nodeid = ino;
			out.body.entry.attr_valid_nsec = NAP_NS / 2;
			out.body.entry.attr.ino = ino;
			out.body.entry.attr.mode = S_IFREG | 0644;
		})));
		EXPECT_LOOKUP(ino, ".")
		.InSequence(seq)
		.WillOnce(Invoke([&](auto in, auto &out __unused) {
			/* Called by fhstat.  Block to simulate a race */
			lookup_unique = in.header.unique;
			sem_post(&sem);
		}));
		break;
	}

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.Times(1)
	.InSequence(seq)
	.WillRepeatedly(Invoke(ReturnImmediate([=](auto in __unused, auto& out)
	{
		/* Called by VOP_SETATTR, caches attributes but not entries */
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.nodeid = ino;
		out.body.entry.attr.size = oldsize;
		out.body.entry.nodeid = ino;
		out.body.entry.attr_valid_nsec = NAP_NS / 2;
		out.body.entry.attr.ino = ino;
		out.body.entry.attr.mode = S_IFREG | 0644;
	})));

	EXPECT_CALL(*m_mock, process(
		ResultOf([=](auto in) {
			return (in.header.opcode == mutator_op &&
				in.header.nodeid == ino);
		}, Eq(true)),
		_)
	).InSequence(seq)
	.WillOnce(Invoke([&](auto in __unused, auto& out) {
		std::unique_ptr<mockfs_buf_out> out0(new mockfs_buf_out);
		std::unique_ptr<mockfs_buf_out> out1(new mockfs_buf_out);

		/* First complete the lookup request, returning the old size */
		out0->header.unique = lookup_unique;
		SET_OUT_HEADER_LEN(*out0, entry);
		out0->body.entry.attr.mode = S_IFREG | 0644;
		out0->body.entry.nodeid = ino;
		out0->body.entry.entry_valid = UINT64_MAX;
		out0->body.entry.attr_valid = UINT64_MAX;
		out0->body.entry.attr.size = oldsize;
		out.push_back(std::move(out0));

		switch(get<1>(params)) {
		case VOP_SETATTR:
			/* Then, respond to the setattr request */
			out1->header.unique = in.header.unique;
			SET_OUT_HEADER_LEN(*out1, attr);
			out1->body.attr.attr.ino = ino;
			out1->body.attr.attr.mode = S_IFREG | 0644;
			out1->body.attr.attr.size = newsize;	// Changed size
			out1->body.attr.attr_valid = UINT64_MAX;
			out.push_back(std::move(out1));
			break;
		case VOP_WRITE:
			// TODO
			break;
		default:
			// TODO
			break;
		}
	}));

	/* First get a file handle */
	switch(get<0>(params)) {
	case VOP_LOOKUP:
		// TODO:
		//ASSERT_EQ(0, stat(FULLPATH, &sb)) << strerror(errno);
		break;
	case VFS_VGET:
		ASSERT_EQ(0, getfh(FULLPATH, &fhp)) << strerror(errno);
		break;
	}

	/* Start the mutator thread */
	switch(get<1>(params)) {
	case VOP_SETATTR:
		ASSERT_EQ(0, pthread_create(&th0, NULL, setattr0, (void*)&sem))
			<< strerror(errno);
		break;
	default:
		// TODO:
		break;
	}

	/* Lookup again, which will race with setattr */
	switch(get<0>(params)) {
	case VOP_LOOKUP:
		ASSERT_EQ(0, stat(FULLPATH, &sb)) << strerror(errno);
		break;
	case VFS_VGET:
		ASSERT_EQ(0, fhstat(&fhp, &sb)) << strerror(errno);
		break;
	}

	ASSERT_EQ((off_t)newsize, sb.st_size);

	/* mutator should've completed without error */
	pthread_join(th0, &thr0_value);
	EXPECT_EQ(0, (intptr_t)thr0_value);
}
//TEST_P(LastLocalModify, last_local_modify)
//{
	//const char FULLPATH[] = "mountpoint/some_file.txt";
	//const char RELPATH[] = "some_file.txt";
	//Sequence seq;
	//const off_t filesize = 2 * m_maxbcachebuf;
	//void *contents;
	//uint64_t ino = 42;
	//uint64_t attr_valid = 0;
	//uint64_t attr_valid_nsec = 0;
	//mode_t mode = S_IFREG | 0644;
	//int fd;
	//int ngetattrs;

	//ngetattrs = GetParam();
//}


INSTANTIATE_TEST_CASE_P(LLM, LastLocalModify,
	Combine(
		Values(VOP_LOOKUP, VFS_VGET),
		Values(VOP_SETATTR, VOP_WRITE, VOP_COPY_FILE_RANGE)
	)
);
