/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2019 The FreeBSD Foundation
 *
 * This software was developed by BFF Storage Systems, LLC under sponsorship
 * from the FreeBSD Foundation.
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
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
}

#include "mockfs.hh"
#include "utils.hh"

using namespace testing;

class Lookup: public FuseTest {};
class Lookup_7_8: public Lookup {
public:
virtual void SetUp() {
	m_kernel_minor_version = 8;
	Lookup::SetUp();
}
};

/*
 * If lookup returns a non-zero cache timeout, then subsequent VOP_GETATTRs
 * should use the cached attributes, rather than query the daemon
 */
TEST_F(Lookup, attr_cache)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";
	const uint64_t ino = 42;
	const uint64_t generation = 13;
	struct stat sb;

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.nodeid = ino;
		out.body.entry.attr_valid = UINT64_MAX;
		out.body.entry.attr.ino = ino;	// Must match nodeid
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.attr.size = 1;
		out.body.entry.attr.blocks = 2;
		out.body.entry.attr.atime = 3;
		out.body.entry.attr.mtime = 4;
		out.body.entry.attr.ctime = 5;
		out.body.entry.attr.atimensec = 6;
		out.body.entry.attr.mtimensec = 7;
		out.body.entry.attr.ctimensec = 8;
		out.body.entry.attr.nlink = 9;
		out.body.entry.attr.uid = 10;
		out.body.entry.attr.gid = 11;
		out.body.entry.attr.rdev = 12;
		out.body.entry.generation = generation;
	})));
	/* stat(2) issues a VOP_LOOKUP followed by a VOP_GETATTR */
	ASSERT_EQ(0, stat(FULLPATH, &sb)) << strerror(errno);
	EXPECT_EQ(1, sb.st_size);
	EXPECT_EQ(2, sb.st_blocks);
	EXPECT_EQ(3, sb.st_atim.tv_sec);
	EXPECT_EQ(6, sb.st_atim.tv_nsec);
	EXPECT_EQ(4, sb.st_mtim.tv_sec);
	EXPECT_EQ(7, sb.st_mtim.tv_nsec);
	EXPECT_EQ(5, sb.st_ctim.tv_sec);
	EXPECT_EQ(8, sb.st_ctim.tv_nsec);
	EXPECT_EQ(9ull, sb.st_nlink);
	EXPECT_EQ(10ul, sb.st_uid);
	EXPECT_EQ(11ul, sb.st_gid);
	EXPECT_EQ(12ul, sb.st_rdev);
	EXPECT_EQ(ino, sb.st_ino);
	EXPECT_EQ(S_IFREG | 0644, sb.st_mode);

	// fuse(4) does not _yet_ support inode generations
	//EXPECT_EQ(generation, sb.st_gen);

	//st_birthtim and st_flags are not supported by protocol 7.8.  They're
	//only supported as OS-specific extensions to OSX.
	//EXPECT_EQ(, sb.st_birthtim);
	//EXPECT_EQ(, sb.st_flags);
	
	//FUSE can't set st_blksize until protocol 7.9
}

/*
 * If lookup returns a finite but non-zero cache timeout, then we should discard
 * the cached attributes and requery the daemon.
 */
TEST_F(Lookup, attr_cache_timeout)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";
	const uint64_t ino = 42;
	struct stat sb;

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.Times(2)
	.WillRepeatedly(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.nodeid = ino;
		out.body.entry.attr_valid_nsec = NAP_NS / 2;
		out.body.entry.attr.ino = ino;	// Must match nodeid
		out.body.entry.attr.mode = S_IFREG | 0644;
	})));

	/* access(2) will issue a VOP_LOOKUP and fill the attr cache */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
	/* Next access(2) will use the cached attributes */
	nap();
	/* The cache has timed out; VOP_GETATTR should query the daemon*/
	ASSERT_EQ(0, stat(FULLPATH, &sb)) << strerror(errno);
}

TEST_F(Lookup, dot)
{
	const char FULLPATH[] = "mountpoint/some_dir/.";
	const char RELDIRPATH[] = "some_dir";
	uint64_t ino = 42;

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELDIRPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.attr.mode = S_IFDIR | 0755;
		out.body.entry.nodeid = ino;
		out.body.entry.attr_valid = UINT64_MAX;
		out.body.entry.entry_valid = UINT64_MAX;
	})));

	/*
	 * access(2) is one of the few syscalls that will not (always) follow
	 * up a successful VOP_LOOKUP with another VOP.
	 */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

TEST_F(Lookup, dotdot)
{
	const char FULLPATH[] = "mountpoint/some_dir/..";
	const char RELDIRPATH[] = "some_dir";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELDIRPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.attr.mode = S_IFDIR | 0755;
		out.body.entry.nodeid = 14;
		out.body.entry.attr_valid = UINT64_MAX;
		out.body.entry.entry_valid = UINT64_MAX;
	})));

	/*
	 * access(2) is one of the few syscalls that will not (always) follow
	 * up a successful VOP_LOOKUP with another VOP.
	 */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

TEST_F(Lookup, enoent)
{
	const char FULLPATH[] = "mountpoint/does_not_exist";
	const char RELPATH[] = "does_not_exist";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnErrno(ENOENT)));
	EXPECT_NE(0, access(FULLPATH, F_OK));
	EXPECT_EQ(ENOENT, errno);
}

TEST_F(Lookup, enotdir)
{
	const char FULLPATH[] = "mountpoint/not_a_dir/some_file.txt";
	const char RELPATH[] = "not_a_dir";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.entry_valid = UINT64_MAX;
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = 42;
	})));

	ASSERT_EQ(-1, access(FULLPATH, F_OK));
	ASSERT_EQ(ENOTDIR, errno);
}

/*
 * If lookup returns a non-zero entry timeout, then subsequent VOP_LOOKUPs
 * should use the cached inode rather than requery the daemon
 */
TEST_F(Lookup, entry_cache)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.entry_valid = UINT64_MAX;
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = 14;
	})));
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
	/* The second access(2) should use the cache */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

/* 
 * If the daemon returns an error of 0 and an inode of 0, that's a flag for
 * "ENOENT and cache it" with the given entry_timeout
 */
TEST_F(Lookup, entry_cache_negative)
{
	struct timespec entry_valid = {.tv_sec = TIME_T_MAX, .tv_nsec = 0};

	EXPECT_LOOKUP(FUSE_ROOT_ID, "does_not_exist")
	.Times(1)
	.WillOnce(Invoke(ReturnNegativeCache(&entry_valid)));

	EXPECT_NE(0, access("mountpoint/does_not_exist", F_OK));
	EXPECT_EQ(ENOENT, errno);
	EXPECT_NE(0, access("mountpoint/does_not_exist", F_OK));
	EXPECT_EQ(ENOENT, errno);
}

/* Negative entry caches should timeout, too */
TEST_F(Lookup, entry_cache_negative_timeout)
{
	const char *RELPATH = "does_not_exist";
	const char *FULLPATH = "mountpoint/does_not_exist";
	struct timespec entry_valid = {.tv_sec = 0, .tv_nsec = NAP_NS / 2};

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.Times(2)
	.WillRepeatedly(Invoke(ReturnNegativeCache(&entry_valid)));

	EXPECT_NE(0, access(FULLPATH, F_OK));
	EXPECT_EQ(ENOENT, errno);

	nap();

	/* The cache has timed out; VOP_LOOKUP should requery the daemon*/
	EXPECT_NE(0, access(FULLPATH, F_OK));
	EXPECT_EQ(ENOENT, errno);
}

/*
 * If lookup returns a finite but non-zero entry cache timeout, then we should
 * discard the cached inode and requery the daemon
 */
TEST_F(Lookup, entry_cache_timeout)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.Times(2)
	.WillRepeatedly(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.entry_valid_nsec = NAP_NS / 2;
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = 14;
	})));

	/* access(2) will issue a VOP_LOOKUP and fill the entry cache */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
	/* Next access(2) will use the cached entry */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
	nap();
	/* The cache has timed out; VOP_LOOKUP should requery the daemon*/
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

static void* setattr0(void* arg __unused) {
	ssize_t r;

	r = truncate("mountpoint/some_file.txt", 5);
	if (r >= 0)
		return 0;
	else
		return (void*)(intptr_t)errno;
}

/*
 * VOP_LOOKUP should discard attributes returned by the server if they were
 * modified by VOP_SETATTR while the VOP_LOOKUP was in progress.
 *
 * Sequence of operations:
 * * Thread 1 calls truncate, which acquires the vnode lock exclusively
 * * Thread 2 calls stat, which does VOP_LOOKUP, which sends FUSE_LOOKUP to the
 *   server.  The server replies with the old file length.  Thread 2 blocks
 *   waiting for the vnode lock.
 * * Thread 1 does FUSE_SETATTR to change the file's size and updates the
 *   attribute cache.  Then it releases the vnode lock.
 * * Thread 2 acquires the vnode lock.  At this point it must not add the
 *   now-stale file size to the attribute cache.
 *
 * Regression test for https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=259071
 */
// TODO: test VOP_LOOKUP/VOP_WRITE and VOP_LOOKUP/VOP_COPY_FILE_RANGE race
TEST_F(Lookup, lookup_during_setattr)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";
	Sequence seq;
	uint64_t ino = 3;
	uint64_t setattr_unique;
	const uint64_t oldsize = 10;
	const uint64_t newsize = 5;
	pthread_t th0;
	void *thr0_value;
	struct stat sb;
	static sem_t sem;

	ASSERT_EQ(0, sem_init(&sem, 0, 0)) << strerror(errno);

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.InSequence(seq)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
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
			return (in.header.opcode == FUSE_SETATTR &&
				in.header.nodeid == ino);
		}, Eq(true)),
		_)
	).InSequence(seq)
	.WillOnce(Invoke([&](auto in, auto &out __unused) {
		/*
		 * FUSE_SETATTR changes the file size, but in order to simulate
		 * a race, don't reply.  Instead, just save the unique for
		 * later.
		 */
		setattr_unique = in.header.unique;
		/* Allow the lookup thread to proceed */
		sem_post(&sem);
	}));
	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.InSequence(seq)
	.WillOnce(Invoke([&](auto in __unused, auto& out) {
		/* First complete the lookup request, returning the old size */
		std::unique_ptr<mockfs_buf_out> out0(new mockfs_buf_out);
		out0->header.unique = in.header.unique;
		SET_OUT_HEADER_LEN(*out0, entry);
		out0->body.entry.attr.mode = S_IFREG | 0644;
		out0->body.entry.nodeid = ino;
		out0->body.entry.entry_valid = UINT64_MAX;
		out0->body.entry.attr_valid = UINT64_MAX;
		out0->body.entry.attr.size = oldsize;
		out.push_back(std::move(out0));

		/* Then, respond to the setattr request */
		std::unique_ptr<mockfs_buf_out> out1(new mockfs_buf_out);
		out1->header.unique = setattr_unique;
		SET_OUT_HEADER_LEN(*out1, attr);
		out1->body.attr.attr.ino = ino;
		out1->body.attr.attr.mode = S_IFREG | 0644;
		out1->body.attr.attr.size = newsize;	// Changed size
		out1->body.attr.attr_valid = UINT64_MAX;
		out.push_back(std::move(out1));
	}));

	/* Start the setattr thread */
	ASSERT_EQ(0, pthread_create(&th0, NULL, setattr0, NULL))
		<< strerror(errno);

	/* Wait for FUSE_SETATTR to be sent */
	sem_wait(&sem);

	/* Lookup again, which will race with setattr */
	ASSERT_EQ(0, stat(FULLPATH, &sb)) << strerror(errno);
	ASSERT_EQ((off_t)newsize, sb.st_size);

	/* truncate should've completed without error */
	pthread_join(th0, &thr0_value);
	EXPECT_EQ(0, (intptr_t)thr0_value);
}

TEST_F(Lookup, ok)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = 14;
	})));
	/*
	 * access(2) is one of the few syscalls that will not (always) follow
	 * up a successful VOP_LOOKUP with another VOP.
	 */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

// Lookup in a subdirectory of the fuse mount
TEST_F(Lookup, subdir)
{
	const char FULLPATH[] = "mountpoint/some_dir/some_file.txt";
	const char DIRPATH[] = "some_dir";
	const char RELPATH[] = "some_file.txt";
	uint64_t dir_ino = 2;
	uint64_t file_ino = 3;

	EXPECT_LOOKUP(FUSE_ROOT_ID, DIRPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.attr.mode = S_IFDIR | 0755;
		out.body.entry.nodeid = dir_ino;
	})));
	EXPECT_LOOKUP(dir_ino, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry);
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = file_ino;
	})));
	/*
	 * access(2) is one of the few syscalls that will not (always) follow
	 * up a successful VOP_LOOKUP with another VOP.
	 */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}

/* 
 * The server returns two different vtypes for the same nodeid.  This is a bad
 * server!  But we shouldn't crash.
 */
TEST_F(Lookup, vtype_conflict)
{
	const char FIRSTFULLPATH[] = "mountpoint/foo";
	const char SECONDFULLPATH[] = "mountpoint/bar";
	const char FIRSTRELPATH[] = "foo";
	const char SECONDRELPATH[] = "bar";
	uint64_t ino = 42;

	expect_lookup(FIRSTRELPATH, ino, S_IFREG | 0644, 0, 1, UINT64_MAX);
	expect_lookup(SECONDRELPATH, ino, S_IFDIR | 0755, 0, 1, UINT64_MAX);

	ASSERT_EQ(0, access(FIRSTFULLPATH, F_OK)) << strerror(errno);
	ASSERT_EQ(-1, access(SECONDFULLPATH, F_OK));
	ASSERT_EQ(EAGAIN, errno);
}

TEST_F(Lookup_7_8, ok)
{
	const char FULLPATH[] = "mountpoint/some_file.txt";
	const char RELPATH[] = "some_file.txt";

	EXPECT_LOOKUP(FUSE_ROOT_ID, RELPATH)
	.WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, entry_7_8);
		out.body.entry.attr.mode = S_IFREG | 0644;
		out.body.entry.nodeid = 14;
	})));
	/*
	 * access(2) is one of the few syscalls that will not (always) follow
	 * up a successful VOP_LOOKUP with another VOP.
	 */
	ASSERT_EQ(0, access(FULLPATH, F_OK)) << strerror(errno);
}


