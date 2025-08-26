/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 CismonX <admin@cismon.net>
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
 */

extern "C" {
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
}

#include "mockfs.hh"
#include "utils.hh"

using namespace testing;

using IoctlTestProcT = std::function<void (int)>;

static const char INPUT_DATA[] = "input_data";
static const char OUTPUT_DATA[] = "output_data";

class Ioctl: public FuseTest {
public:
void expect_ioctl(uint64_t ino, ProcessMockerT r)
{
	EXPECT_CALL(*m_mock, process(
		ResultOf([=](auto in) {
			return (in.header.opcode == FUSE_IOCTL &&
				in.header.nodeid == ino);
		}, Eq(true)), _)
	).WillOnce(Invoke(r)).RetiresOnSaturation();
}

void expect_ioctl_io(uint64_t ino, uint32_t flags, uint32_t cmd, uintptr_t arg)
{
	assert(cmd & IOC_VOID);

	EXPECT_CALL(*m_mock, process(
		ResultOf([=](auto in) {
			return (in.header.opcode == FUSE_IOCTL &&
				in.header.nodeid == ino &&
				in.body.ioctl.flags == flags &&
				in.body.ioctl.cmd == cmd &&
				in.body.ioctl.arg == arg
				);
		}, Eq(true)), _)
	).WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, ioctl);
	}))).RetiresOnSaturation();
}

void expect_ioctl_r(uint64_t ino, uint32_t flags, uint32_t cmd,
    const void *contents, uint64_t osize)
{
	assert(cmd & IOC_OUT);

	EXPECT_CALL(*m_mock, process(
		ResultOf([=](auto in) {
			return (in.header.opcode == FUSE_IOCTL &&
				in.header.nodeid == ino &&
				in.body.ioctl.flags == flags &&
				in.body.ioctl.cmd == cmd
				);
		}, Eq(true)), _)
	).WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		uint8_t *out_buf = out.body.bytes + sizeof(out.body.ioctl);

		assert(osize <= sizeof(out.body.bytes));

		SET_OUT_HEADER_LEN(out, ioctl);
		memcpy(out_buf, contents, osize);
		out.header.len += osize;
	}))).RetiresOnSaturation();
}

void expect_ioctl_w(uint64_t ino, uint32_t flags, uint32_t cmd,
    const void *contents, uint64_t isize)
{
	assert(cmd & IOC_IN);

	EXPECT_CALL(*m_mock, process(
		ResultOf([=](auto in) {
			const char *buf = (const char*)in.body.bytes +
				sizeof(struct fuse_ioctl_in);

			assert(isize <= sizeof(in.body.bytes) -
					sizeof(struct fuse_ioctl_in));

			return (in.header.opcode == FUSE_IOCTL &&
				in.header.nodeid == ino &&
				in.body.ioctl.flags == flags &&
				in.body.ioctl.cmd == cmd &&
				0 == bcmp(buf, contents, isize));
		}, Eq(true)), _)
	).WillOnce(Invoke(ReturnImmediate([=](auto in __unused, auto& out) {
		SET_OUT_HEADER_LEN(out, ioctl);
	}))).RetiresOnSaturation();
}

void expect_ioctl_rw(uint64_t ino)
{
	/*
	 * _IOR(): Compare the input data with INPUT_DATA.
	 * _IOW(): Copy out OUTPUT_DATA.
	 * _IOWR(): Combination of above.
	 * _IOWINT(): Return the integer argument value.
	 */
	expect_ioctl(ino, ReturnImmediate([](auto in, auto& out) {
		uint8_t *in_buf = in.body.bytes + sizeof(in.body.ioctl);
		uint8_t *out_buf = out.body.bytes + sizeof(out.body.ioctl);
		uint32_t cmd = in.body.ioctl.cmd;
		uint32_t arg_len = IOCPARM_LEN(cmd);
		int result = 0;

		out.header.error = 0;
		SET_OUT_HEADER_LEN(out, ioctl);
		if ((cmd & IOC_VOID) != 0 && arg_len > 0) {
			memcpy(&result, in_buf, sizeof(int));
			goto out;
		}
		if ((cmd & IOC_IN) != 0) {
			if (0 != strncmp(INPUT_DATA, (char *)in_buf, arg_len)) {
				result = -EINVAL;
				goto out;
			}
		}
		if ((cmd & IOC_OUT) != 0) {
			memcpy(out_buf, OUTPUT_DATA, sizeof(OUTPUT_DATA));
			out.header.len += sizeof(OUTPUT_DATA);
		}

out:
		out.body.ioctl.result = result;
	}));
}
};

/**
 * If the server does not implement FUSE_IOCTL handler (returns ENOSYS),
 * the kernel should return ENOTTY to the user instead.
 */
TEST_F(Ioctl, enosys)
{
	unsigned long req = _IO(0xff, 0);
	int fd;

	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl(FUSE_ROOT_ID, ReturnErrno(ENOSYS));

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	EXPECT_EQ(-1, ioctl(fd, req));
	EXPECT_EQ(ENOTTY, errno);

	leak(fd);
}

/* _IO() ioctls pack their payload into the arg value.  */
TEST_F(Ioctl, io)
{
	unsigned long req = _IO(0xff, 6);
	unsigned long arg = 0xdeadbeef;
	int fd;

	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl_io(FUSE_ROOT_ID, FUSE_IOCTL_DIR, req, arg);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	EXPECT_EQ(0, ioctl(fd, req, arg)) << strerror(errno);

	leak(fd);
}

/*
 * For _IOR() and _IOWR(), The server is allowed to write fewer bytes
 * than IOCPARM_LEN(req).
 */
TEST_F(Ioctl, ior)
{
	const char *CONTENTS = "abcdefgh";
	const ssize_t clen = sizeof(CONTENTS) + 1;
	char buf[clen] = { 0 };
	unsigned long req = _IOR(0xff, 1, buf);
	int fd;

	//printf("req = %#lx\n", req);
	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl_r(FUSE_ROOT_ID, FUSE_IOCTL_DIR, req, (const void*)CONTENTS,
	    clen);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	EXPECT_EQ(0, ioctl(fd, req, buf)) << strerror(errno);
	EXPECT_EQ(0, memcmp(buf, CONTENTS, clen));

	leak(fd);
}

/*
 * For _IOR() and _IOWR(), if the server attempts to write more bytes
 * than IOCPARM_LEN(req), the kernel should fail the syscall with EIO.
 */
TEST_F(Ioctl, ior_overflow)
{
	const char *CONTENTS = "abcdefgh";
	const ssize_t clen = sizeof(CONTENTS) + 1;
	char buf[clen] = { 0 };
	unsigned long req = _IOR(0xff, 2, buf);
	int fd;

	//printf("req = %#lx\n", req);
	expect_opendir(FUSE_ROOT_ID);
	//expect_ioctl_rw(FUSE_ROOT_ID);
	expect_ioctl_r(FUSE_ROOT_ID, FUSE_IOCTL_DIR, req, (const void*)CONTENTS,
			clen + 1);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	EXPECT_EQ(-1, ioctl(fd, req, buf));
	EXPECT_EQ(EIO, errno);

	leak(fd);
}

TEST_F(Ioctl, iow)
{
	const char *CONTENTS = "abcdefgh";
	const ssize_t clen = sizeof(CONTENTS) + 1;
	unsigned long req = _IOW(0xff, 3, CONTENTS);
	int fd;

	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl_w(FUSE_ROOT_ID, FUSE_IOCTL_DIR, req, (const void*)CONTENTS,
		clen);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	EXPECT_EQ(0, ioctl(fd, req, CONTENTS)) << strerror(errno);

	leak(fd);
}

TEST_F(Ioctl, iowr)
{
	char buf[std::max(sizeof(INPUT_DATA), sizeof(OUTPUT_DATA))] = { 0 };
	unsigned long req = _IOWR(0xff, 4, buf);
	int fd;

	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl_rw(FUSE_ROOT_ID);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	memcpy(buf, INPUT_DATA, sizeof(INPUT_DATA));
	EXPECT_EQ(0, ioctl(fd, req, buf)) << strerror(errno);
	EXPECT_EQ(0, memcmp(buf, OUTPUT_DATA, sizeof(OUTPUT_DATA)));

	leak(fd);
}

TEST_F(Ioctl, iowint)
{
	unsigned long req = _IOWINT(0xff, 5);
	int arg = 1337;
	int fd, r;

	expect_opendir(FUSE_ROOT_ID);
	expect_ioctl_rw(FUSE_ROOT_ID);

	fd = open("mountpoint", O_RDONLY | O_DIRECTORY);
	ASSERT_LE(0, fd) << strerror(errno);

	/* The server is allowed to return a positive value on success */
	r = ioctl(fd, req, arg);
	EXPECT_LE(0, r) << strerror(errno);
	EXPECT_EQ(arg, r);

	leak(fd);
}
