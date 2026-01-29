#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

#include <string>

#include "capsicum.h"
#include "syscalls.h"
#include "capsicum-test.h"

TEST(Linux, TimerFD) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);

  cap_rights_t r_ro;
  cap_rights_init(&r_ro, CAP_READ);
  cap_rights_t r_wo;
  cap_rights_init(&r_wo, CAP_WRITE);
  cap_rights_t r_rw;
  cap_rights_init(&r_rw, CAP_READ, CAP_WRITE);
  cap_rights_t r_rwpoll;
  cap_rights_init(&r_rwpoll, CAP_READ, CAP_WRITE, CAP_EVENT);

  int cap_fd_ro = dup(fd);
  EXPECT_OK(cap_fd_ro);
  EXPECT_OK(cap_rights_limit(cap_fd_ro, &r_ro));
  int cap_fd_wo = dup(fd);
  EXPECT_OK(cap_fd_wo);
  EXPECT_OK(cap_rights_limit(cap_fd_wo, &r_wo));
  int cap_fd_rw = dup(fd);
  EXPECT_OK(cap_fd_rw);
  EXPECT_OK(cap_rights_limit(cap_fd_rw, &r_rw));
  int cap_fd_all = dup(fd);
  EXPECT_OK(cap_fd_all);
  EXPECT_OK(cap_rights_limit(cap_fd_all, &r_rwpoll));

  struct itimerspec old_ispec;
  struct itimerspec ispec;
  ispec.it_interval.tv_sec = 0;
  ispec.it_interval.tv_nsec = 0;
  ispec.it_value.tv_sec = 0;
  ispec.it_value.tv_nsec = 100000000;  // 100ms
  EXPECT_NOTCAPABLE(timerfd_settime(cap_fd_ro, 0, &ispec, NULL));
  EXPECT_NOTCAPABLE(timerfd_settime(cap_fd_wo, 0, &ispec, &old_ispec));
  EXPECT_OK(timerfd_settime(cap_fd_wo, 0, &ispec, NULL));
  EXPECT_OK(timerfd_settime(cap_fd_rw, 0, &ispec, NULL));
  EXPECT_OK(timerfd_settime(cap_fd_all, 0, &ispec, NULL));

  EXPECT_NOTCAPABLE(timerfd_gettime(cap_fd_wo, &old_ispec));
  EXPECT_OK(timerfd_gettime(cap_fd_ro, &old_ispec));
  EXPECT_OK(timerfd_gettime(cap_fd_rw, &old_ispec));
  EXPECT_OK(timerfd_gettime(cap_fd_all, &old_ispec));

  // To be able to poll() for the timer pop, still need CAP_EVENT.
  struct pollfd poll_fd;
  for (int ii = 0; ii < 3; ii++) {
    poll_fd.revents = 0;
    poll_fd.events = POLLIN;
    switch (ii) {
    case 0: poll_fd.fd = cap_fd_ro; break;
    case 1: poll_fd.fd = cap_fd_wo; break;
    case 2: poll_fd.fd = cap_fd_rw; break;
    }
    // Poll immediately returns with POLLNVAL
    EXPECT_OK(poll(&poll_fd, 1, 400));
    EXPECT_EQ(0, (poll_fd.revents & POLLIN));
    EXPECT_NE(0, (poll_fd.revents & POLLNVAL));
  }

  poll_fd.fd = cap_fd_all;
  EXPECT_OK(poll(&poll_fd, 1, 400));
  EXPECT_NE(0, (poll_fd.revents & POLLIN));
  EXPECT_EQ(0, (poll_fd.revents & POLLNVAL));

  EXPECT_OK(timerfd_gettime(cap_fd_all, &old_ispec));
  EXPECT_EQ(0, old_ispec.it_value.tv_sec);
  EXPECT_EQ(0, old_ispec.it_value.tv_nsec);
  EXPECT_EQ(0, old_ispec.it_interval.tv_sec);
  EXPECT_EQ(0, old_ispec.it_interval.tv_nsec);

  close(cap_fd_all);
  close(cap_fd_rw);
  close(cap_fd_wo);
  close(cap_fd_ro);
  close(fd);
}

TEST(Linux, EventFD) {
  int fd = eventfd(0, 0);
  EXPECT_OK(fd);

  cap_rights_t r_rs;
  cap_rights_init(&r_rs, CAP_READ, CAP_SEEK);
  cap_rights_t r_ws;
  cap_rights_init(&r_ws, CAP_WRITE, CAP_SEEK);
  cap_rights_t r_rws;
  cap_rights_init(&r_rws, CAP_READ, CAP_WRITE, CAP_SEEK);
  cap_rights_t r_rwspoll;
  cap_rights_init(&r_rwspoll, CAP_READ, CAP_WRITE, CAP_SEEK, CAP_EVENT);

  int cap_ro = dup(fd);
  EXPECT_OK(cap_ro);
  EXPECT_OK(cap_rights_limit(cap_ro, &r_rs));
  int cap_wo = dup(fd);
  EXPECT_OK(cap_wo);
  EXPECT_OK(cap_rights_limit(cap_wo, &r_ws));
  int cap_rw = dup(fd);
  EXPECT_OK(cap_rw);
  EXPECT_OK(cap_rights_limit(cap_rw, &r_rws));
  int cap_all = dup(fd);
  EXPECT_OK(cap_all);
  EXPECT_OK(cap_rights_limit(cap_all, &r_rwspoll));

  pid_t child = fork();
  if (child == 0) {
    // Child: write counter to eventfd
    uint64_t u = 42;
    EXPECT_NOTCAPABLE(write(cap_ro, &u, sizeof(u)));
    EXPECT_OK(write(cap_wo, &u, sizeof(u)));
    exit(HasFailure());
  }

  sleep(1);  // Allow child to write

  struct pollfd poll_fd;
  poll_fd.revents = 0;
  poll_fd.events = POLLIN;
  poll_fd.fd = cap_rw;
  EXPECT_OK(poll(&poll_fd, 1, 400));
  EXPECT_EQ(0, (poll_fd.revents & POLLIN));
  EXPECT_NE(0, (poll_fd.revents & POLLNVAL));

  poll_fd.fd = cap_all;
  EXPECT_OK(poll(&poll_fd, 1, 400));
  EXPECT_NE(0, (poll_fd.revents & POLLIN));
  EXPECT_EQ(0, (poll_fd.revents & POLLNVAL));

  uint64_t u;
  EXPECT_NOTCAPABLE(read(cap_wo, &u, sizeof(u)));
  EXPECT_OK(read(cap_ro, &u, sizeof(u)));
  EXPECT_EQ(42, (int)u);

  // Wait for the child.
  int status;
  EXPECT_EQ(child, waitpid(child, &status, 0));
  int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  EXPECT_EQ(0, rc);

  close(cap_all);
  close(cap_rw);
  close(cap_wo);
  close(cap_ro);
  close(fd);
}

TEST(Linux, fstatat) {
  int fd = open(TmpFile("cap_fstatat"), O_CREAT|O_RDWR, 0644);
  EXPECT_OK(fd);
  unsigned char buffer[] = {1, 2, 3, 4};
  EXPECT_OK(write(fd, buffer, sizeof(buffer)));
  cap_rights_t rights;
  int cap_rf = dup(fd);
  EXPECT_OK(cap_rf);
  EXPECT_OK(cap_rights_limit(cap_rf, cap_rights_init(&rights, CAP_READ, CAP_FSTATAT)));
  int cap_ro = dup(fd);
  EXPECT_OK(cap_ro);
  EXPECT_OK(cap_rights_limit(cap_ro, cap_rights_init(&rights, CAP_READ)));

  struct stat info;
  EXPECT_OK(fstatat(fd, "", &info, AT_EMPTY_PATH));
  EXPECT_NOTCAPABLE(fstatat(cap_ro, "", &info, AT_EMPTY_PATH));
  EXPECT_OK(fstatat(cap_rf, "", &info, AT_EMPTY_PATH));

  close(cap_ro);
  close(cap_rf);
  close(fd);

  int dir = open(tmpdir.c_str(), O_RDONLY);
  EXPECT_OK(dir);
  int dir_rf = dup(dir);
  EXPECT_OK(dir_rf);
  EXPECT_OK(cap_rights_limit(dir_rf, cap_rights_init(&rights, CAP_READ, CAP_FSTATAT)));
  int dir_ro = dup(fd);
  EXPECT_OK(dir_ro);
  EXPECT_OK(cap_rights_limit(dir_ro, cap_rights_init(&rights, CAP_READ)));

  EXPECT_OK(fstatat(dir, "cap_fstatat", &info, AT_EMPTY_PATH));
  EXPECT_NOTCAPABLE(fstatat(dir_ro, "cap_fstatat", &info, AT_EMPTY_PATH));
  EXPECT_OK(fstatat(dir_rf, "cap_fstatat", &info, AT_EMPTY_PATH));

  close(dir_ro);
  close(dir_rf);
  close(dir);

  unlink(TmpFile("cap_fstatat"));
}

TEST(Linux, inotify) {
  int i_fd = inotify_init();
  EXPECT_OK(i_fd);

  cap_rights_t r_rs;
  cap_rights_init(&r_rs, CAP_READ, CAP_SEEK);
  cap_rights_t r_ws;
  cap_rights_init(&r_ws, CAP_WRITE, CAP_SEEK);
  cap_rights_t r_rws;
  cap_rights_init(&r_rws, CAP_READ, CAP_WRITE, CAP_SEEK);
  cap_rights_t r_rwsnotify;
  cap_rights_init(&r_rwsnotify, CAP_READ, CAP_WRITE, CAP_SEEK, CAP_INOTIFY_ADD, CAP_INOTIFY_RM);

  int cap_fd_ro = dup(i_fd);
  EXPECT_OK(cap_fd_ro);
  EXPECT_OK(cap_rights_limit(cap_fd_ro, &r_rs));
  int cap_fd_wo = dup(i_fd);
  EXPECT_OK(cap_fd_wo);
  EXPECT_OK(cap_rights_limit(cap_fd_wo, &r_ws));
  int cap_fd_rw = dup(i_fd);
  EXPECT_OK(cap_fd_rw);
  EXPECT_OK(cap_rights_limit(cap_fd_rw, &r_rws));
  int cap_fd_all = dup(i_fd);
  EXPECT_OK(cap_fd_all);
  EXPECT_OK(cap_rights_limit(cap_fd_all, &r_rwsnotify));

  int fd = open(TmpFile("cap_inotify"), O_CREAT|O_RDWR, 0644);
  EXPECT_NOTCAPABLE(inotify_add_watch(cap_fd_rw, TmpFile("cap_inotify"), IN_ACCESS|IN_MODIFY));
  int wd = inotify_add_watch(i_fd, TmpFile("cap_inotify"), IN_ACCESS|IN_MODIFY);
  EXPECT_OK(wd);

  unsigned char buffer[] = {1, 2, 3, 4};
  EXPECT_OK(write(fd, buffer, sizeof(buffer)));

  struct inotify_event iev;
  memset(&iev, 0, sizeof(iev));
  EXPECT_NOTCAPABLE(read(cap_fd_wo, &iev, sizeof(iev)));
  int rc = read(cap_fd_ro, &iev, sizeof(iev));
  EXPECT_OK(rc);
  EXPECT_EQ((int)sizeof(iev), rc);
  EXPECT_EQ(wd, iev.wd);

  EXPECT_NOTCAPABLE(inotify_rm_watch(cap_fd_wo, wd));
  EXPECT_OK(inotify_rm_watch(cap_fd_all, wd));

  close(fd);
  close(cap_fd_all);
  close(cap_fd_rw);
  close(cap_fd_wo);
  close(cap_fd_ro);
  close(i_fd);
  unlink(TmpFile("cap_inotify"));
}

TEST(Linux, ArchChangeIfAvailable) {
  const char* prog_candidates[] = {"./mini-me.32", "./mini-me.x32", "./mini-me.64"};
  const char* progs[] = {NULL, NULL, NULL};
  char* argv_pass[] = {(char*)"to-come", (char*)"--capmode", NULL};
  char* null_envp[] = {NULL};
  int fds[3];
  int count = 0;

  for (int ii = 0; ii < 3; ii++) {
    fds[count] = open(prog_candidates[ii], O_RDONLY);
    if (fds[count] >= 0) {
      progs[count] = prog_candidates[ii];
      count++;
    }
  }
  if (count == 0) {
    GTEST_SKIP() << "no different-architecture programs available";
  }

  for (int ii = 0; ii < count; ii++) {
    // Fork-and-exec a binary of this architecture.
    pid_t child = fork();
    if (child == 0) {
      EXPECT_OK(cap_enter());  // Enter capability mode
      if (verbose) fprintf(stderr, "[%d] call fexecve(%s, %s)\n",
                           getpid_(), progs[ii], argv_pass[1]);
      argv_pass[0] = (char *)progs[ii];
      int rc = fexecve_(fds[ii], argv_pass, null_envp);
      fprintf(stderr, "fexecve(%s) returned %d errno %d\n", progs[ii], rc, errno);
      exit(99);  // Should not reach here.
    }
    int status;
    EXPECT_EQ(child, waitpid(child, &status, 0));
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    EXPECT_EQ(0, rc);
    close(fds[ii]);
  }
}

static void SendFD(int fd, int over) {
  struct msghdr mh;
  mh.msg_name = NULL;  // No address needed
  mh.msg_namelen = 0;
  char buffer1[1024];
  struct iovec iov[1];
  iov[0].iov_base = buffer1;
  iov[0].iov_len = sizeof(buffer1);
  mh.msg_iov = iov;
  mh.msg_iovlen = 1;
  char buffer2[1024];
  mh.msg_control = buffer2;
  mh.msg_controllen = CMSG_LEN(sizeof(int));
  struct cmsghdr *cmptr = CMSG_FIRSTHDR(&mh);
  cmptr->cmsg_level = SOL_SOCKET;
  cmptr->cmsg_type = SCM_RIGHTS;
  cmptr->cmsg_len = CMSG_LEN(sizeof(int));
  *(int *)CMSG_DATA(cmptr) = fd;
  buffer1[0] = 0;
  iov[0].iov_len = 1;
  int rc = sendmsg(over, &mh, 0);
  EXPECT_OK(rc);
}

static int ReceiveFD(int over) {
  struct msghdr mh;
  mh.msg_name = NULL;  // No address needed
  mh.msg_namelen = 0;
  char buffer1[1024];
  struct iovec iov[1];
  iov[0].iov_base = buffer1;
  iov[0].iov_len = sizeof(buffer1);
  mh.msg_iov = iov;
  mh.msg_iovlen = 1;
  char buffer2[1024];
  mh.msg_control = buffer2;
  mh.msg_controllen = sizeof(buffer2);
  int rc = recvmsg(over, &mh, 0);
  EXPECT_OK(rc);
  EXPECT_LE(CMSG_LEN(sizeof(int)), mh.msg_controllen);
  struct cmsghdr *cmptr = CMSG_FIRSTHDR(&mh);
  int fd = *(int*)CMSG_DATA(cmptr);
  EXPECT_EQ(CMSG_LEN(sizeof(int)), cmptr->cmsg_len);
  cmptr = CMSG_NXTHDR(&mh, cmptr);
  EXPECT_TRUE(cmptr == NULL);
  return fd;
}

static int shared_sock_fds[2];

int NSInit(void *data) {
  // This function is running in a new PID namespace, and so is pid 1.
  if (verbose) fprintf(stderr, "  NSInit: pid=%d, ppid=%d\n", getpid_(), getppid());
  EXPECT_EQ(1, getpid_());
  EXPECT_EQ(0, getppid());

  int pd;
  pid_t child = pdfork(&pd, 0);
  EXPECT_OK(child);
  if (child == 0) {
    // Child: loop forever until terminated.
    if (verbose) fprintf(stderr, "    child of NSInit: pid=%d, ppid=%d\n", getpid_(), getppid());
    while (true) {
      if (verbose) fprintf(stderr, "    child of NSInit: \"I aten't dead\"\n");
      usleep(100000);
    }
    exit(0);
  }
  EXPECT_EQ(2, child);
  EXPECT_PID_ALIVE(child);
  if (verbose) fprintf(stderr, "  NSInit: pdfork() -> pd=%d, corresponding pid=%d state='%c'\n",
                       pd, child, ProcessState(child));
  sleep(1);

  // Send the process descriptor over UNIX domain socket back to parent.
  SendFD(pd, shared_sock_fds[1]);
  close(pd);

  // Wait for a byte back in the other direction.
  int value;
  if (verbose) fprintf(stderr, "  NSInit: block waiting for value\n");
  read(shared_sock_fds[1], &value, sizeof(value));

  if (verbose) fprintf(stderr, "  NSInit: return 0\n");
  return 0;
}

TEST(Linux, ProcFS) {
  cap_rights_t rights;
  cap_rights_init(&rights, CAP_READ, CAP_SEEK);
  int fd = open("/etc/passwd", O_RDONLY);
  EXPECT_OK(fd);
  lseek(fd, 4, SEEK_SET);
  int cap = dup(fd);
  EXPECT_OK(cap);
  EXPECT_OK(cap_rights_limit(cap, &rights));
  pid_t me = getpid_();

  char buffer[1024];
  sprintf(buffer, "/proc/%d/fdinfo/%d", me, cap);
  int procfd = open(buffer, O_RDONLY);
  EXPECT_OK(procfd) << " failed to open " << buffer;
  if (procfd < 0) return;
  int proccap = dup(procfd);
  EXPECT_OK(proccap);
  EXPECT_OK(cap_rights_limit(proccap, &rights));

  EXPECT_OK(read(proccap, buffer, sizeof(buffer)));
  // The fdinfo should include the file pos of the underlying file
  EXPECT_NE((char*)NULL, strstr(buffer, "pos:\t4"));
  // ...and the rights of the Capsicum capability.
  EXPECT_NE((char*)NULL, strstr(buffer, "rights:\t0x"));

  close(procfd);
  close(proccap);
  close(cap);
  close(fd);
}

FORK_TEST(Linux, ProcessClocks) {
  pid_t self = getpid_();
  pid_t child = fork();
  EXPECT_OK(child);
  if (child == 0) {
    child = getpid_();
    usleep(100000);
    exit(0);
  }

  EXPECT_OK(cap_enter());  // Enter capability mode.

  // Nefariously build a clock ID for the child's CPU time.
  // This relies on knowledge of the internal layout of clock IDs.
  clockid_t child_clock;
  child_clock = ((~child) << 3) | 0x0;
  struct timespec ts;
  memset(&ts, 0, sizeof(ts));

  // TODO(drysdale): Should not be possible to retrieve info about a
  // different process, as the PID global namespace should be locked
  // down.
  EXPECT_OK(clock_gettime(child_clock, &ts));
  if (verbose) fprintf(stderr, "[parent: %d] clock_gettime(child=%d->0x%08x) is %ld.%09ld \n",
                       self, child, child_clock, (long)ts.tv_sec, (long)ts.tv_nsec);

  child_clock = (int)0xfffffff8;
  memset(&ts, 0, sizeof(ts));
  EXPECT_OK(clock_gettime(child_clock, &ts));
  if (verbose) fprintf(stderr, "[parent: %d] clock_gettime(init=1->0x%08x) is %ld.%09ld \n",
                       self, child_clock, (long)ts.tv_sec, (long)ts.tv_nsec);

  // Orphan the child.
}

FORK_TEST(Linux, GetRandom) {
  EXPECT_OK(cap_enter());
  unsigned char buffer[1024];
  unsigned char buffer2[1024];
  EXPECT_OK(getrandom(buffer, sizeof(buffer), GRND_NONBLOCK));
  EXPECT_OK(getrandom(buffer2, sizeof(buffer2), GRND_NONBLOCK));
  EXPECT_NE(0, memcmp(buffer, buffer2, sizeof(buffer)));
}

TEST(Linux, MemFDDeathTestIfAvailable) {
  int memfd = memfd_create("capsicum-test", MFD_ALLOW_SEALING);
  if (memfd == -1 && errno == ENOSYS) {
    GTEST_SKIP() << "memfd_create(2) gives -ENOSYS";
  }
  const int LEN = 16;
  EXPECT_OK(ftruncate(memfd, LEN));
  int memfd_ro = dup(memfd);
  int memfd_rw = dup(memfd);
  EXPECT_OK(memfd_ro);
  EXPECT_OK(memfd_rw);
  cap_rights_t rights;
  EXPECT_OK(cap_rights_limit(memfd_ro, cap_rights_init(&rights, CAP_MMAP_R, CAP_FSTAT)));
  EXPECT_OK(cap_rights_limit(memfd_rw, cap_rights_init(&rights, CAP_MMAP_RW, CAP_FCHMOD)));

  unsigned char *p_ro = (unsigned char *)mmap(NULL, LEN, PROT_READ, MAP_SHARED, memfd_ro, 0);
  EXPECT_NE((unsigned char *)MAP_FAILED, p_ro);
  unsigned char *p_rw = (unsigned char *)mmap(NULL, LEN, PROT_READ|PROT_WRITE, MAP_SHARED, memfd_rw, 0);
  EXPECT_NE((unsigned char *)MAP_FAILED, p_rw);
  EXPECT_EQ(MAP_FAILED,
            mmap(NULL, LEN, PROT_READ|PROT_WRITE, MAP_SHARED, memfd_ro, 0));

  *p_rw = 42;
  EXPECT_EQ(42, *p_ro);
  EXPECT_DEATH(*p_ro = 42, "");

#ifndef F_ADD_SEALS
  // Hack for when libc6 does not yet include the updated linux/fcntl.h from kernel 3.17
#define _F_LINUX_SPECIFIC_BASE F_SETLEASE
#define F_ADD_SEALS	(_F_LINUX_SPECIFIC_BASE + 9)
#define F_GET_SEALS	(_F_LINUX_SPECIFIC_BASE + 10)
#define F_SEAL_SEAL	0x0001	/* prevent further seals from being set */
#define F_SEAL_SHRINK	0x0002	/* prevent file from shrinking */
#define F_SEAL_GROW	0x0004	/* prevent file from growing */
#define F_SEAL_WRITE	0x0008	/* prevent writes */
#endif

  // Reading the seal information requires CAP_FSTAT.
  int seals = fcntl(memfd, F_GET_SEALS);
  EXPECT_OK(seals);
  if (verbose) fprintf(stderr, "seals are %08x on base fd\n", seals);
  int seals_ro = fcntl(memfd_ro, F_GET_SEALS);
  EXPECT_EQ(seals, seals_ro);
  if (verbose) fprintf(stderr, "seals are %08x on read-only fd\n", seals_ro);
  int seals_rw = fcntl(memfd_rw, F_GET_SEALS);
  EXPECT_NOTCAPABLE(seals_rw);

  // Fail to seal as a writable mapping exists.
  EXPECT_EQ(-1, fcntl(memfd_rw, F_ADD_SEALS, F_SEAL_WRITE));
  EXPECT_EQ(EBUSY, errno);
  *p_rw = 42;

  // Seal the rw version; need to unmap first.
  munmap(p_rw, LEN);
  munmap(p_ro, LEN);
  EXPECT_OK(fcntl(memfd_rw, F_ADD_SEALS, F_SEAL_WRITE));

  seals = fcntl(memfd, F_GET_SEALS);
  EXPECT_OK(seals);
  if (verbose) fprintf(stderr, "seals are %08x on base fd\n", seals);
  seals_ro = fcntl(memfd_ro, F_GET_SEALS);
  EXPECT_EQ(seals, seals_ro);
  if (verbose) fprintf(stderr, "seals are %08x on read-only fd\n", seals_ro);

  // Remove the CAP_FCHMOD right, can no longer add seals.
  EXPECT_OK(cap_rights_limit(memfd_rw, cap_rights_init(&rights, CAP_MMAP_RW)));
  EXPECT_NOTCAPABLE(fcntl(memfd_rw, F_ADD_SEALS, F_SEAL_WRITE));

  close(memfd);
  close(memfd_ro);
  close(memfd_rw);
}
