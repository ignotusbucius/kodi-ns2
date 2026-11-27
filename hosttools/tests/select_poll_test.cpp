// Host test of xbmc/platform/switch2/SelectPoll.h (the rev 38 __wrap_select), against the host's poll().
// Built and run by hosttools/check-net-select.sh. Exit 0 = all cases pass.
#include "../../xbmc/xbmc/platform/switch2/SelectPoll.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
int g_fail = 0;
void Check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  g_fail += !ok;
}
int Sel(int n, fd_set* r, fd_set* w, fd_set* e, timeval* tv)
{
  return SWITCH2NET::SelectViaPoll(
      n, r, w, e, tv, [](pollfd* f, nfds_t c, int ms) { return poll(f, c, ms); },
      [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms < 0 ? 0 : ms)); });
}
long long Ms(std::chrono::steady_clock::time_point t0)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
      .count();
}
} // namespace

int main()
{
  // A socket pair whose first end is fd 63: libounce's select() refused nfds 64 (EINVAL).
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || dup2(sv[0], 63) != 63)
  {
    std::perror("setup");
    return 2;
  }
  close(sv[0]);
  const int a = 63, b = sv[1];
  fd_set r, w, e;
  timeval tv{};

  FD_ZERO(&r);
  FD_SET(a, &r);
  tv = {0, 50000};
  auto t0 = std::chrono::steady_clock::now();
  int rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == 0 && !FD_ISSET(a, &r) && Ms(t0) >= 49, "fd 63, nothing to read: 0 after the 50 ms timeout, bit cleared");

  (void)!write(b, "x", 1);
  FD_ZERO(&r);
  FD_SET(a, &r);
  tv = {1, 0};
  rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == 1 && FD_ISSET(a, &r), "fd 63 readable: 1, bit kept (nfds 64 = FD_SETSIZE on the console)");

  FD_ZERO(&r);
  FD_ZERO(&w);
  FD_ZERO(&e);
  FD_SET(a, &w);
  FD_SET(a, &e);
  FD_SET(b, &r);
  tv = {0, 0};
  rc = Sel(64, &r, &w, &e, &tv);
  Check(rc == 1 && FD_ISSET(a, &w) && !FD_ISSET(a, &e) && !FD_ISSET(b, &r),
        "write-ready fd 63 counted, no exception, idle peer read bit cleared");

  FD_ZERO(&r);
  FD_SET(a, &r);
  FD_SET(b, &r);
  tv = {0, 0};
  rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == 1 && FD_ISSET(a, &r) && !FD_ISSET(b, &r), "two fds in one set: only the ready one kept");

  t0 = std::chrono::steady_clock::now();
  tv = {0, 30000};
  rc = Sel(0, nullptr, nullptr, nullptr, &tv);
  Check(rc == 0 && Ms(t0) >= 29, "no fds: sleeps the 30 ms timeout, returns 0 (CurlFile's select(0, ...))");

  FD_ZERO(&r);
  t0 = std::chrono::steady_clock::now();
  tv = {0, 20000};
  rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == 0 && Ms(t0) >= 19, "empty sets with nfds 64: sleep, 0");

  errno = 0;
  rc = Sel(SWITCH2NET::kSelectMaxFds + 1, &r, nullptr, nullptr, &tv);
  Check(rc == -1 && errno == EINVAL, "nfds > FD_SETSIZE: EINVAL");
  errno = 0;
  rc = Sel(-1, &r, nullptr, nullptr, &tv);
  Check(rc == -1 && errno == EINVAL, "nfds < 0: EINVAL");
  tv = {0, 1000000};
  errno = 0;
  rc = Sel(1, &r, nullptr, nullptr, &tv);
  Check(rc == -1 && errno == EINVAL, "tv_usec 1000000: EINVAL");

  tv = {0, 500};
  Check(SWITCH2NET::SelectTimeoutMs(&tv) == 1, "0.5 ms timeout rounds up to 1 ms (no busy poll(0))");
  tv = {2, 1};
  Check(SWITCH2NET::SelectTimeoutMs(&tv) == 2001, "2 s + 1 us = 2001 ms");
  Check(SWITCH2NET::SelectTimeoutMs(nullptr) == -1, "NULL timeout: wait forever");

  // Peer closed: hang-up counts as readable, as libounce's select maps POLLHUP.
  close(b);
  FD_ZERO(&r);
  FD_SET(a, &r);
  (void)!read(a, &tv, 1);
  tv = {1, 0};
  rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == 1 && FD_ISSET(a, &r), "peer closed: fd 63 readable (POLLHUP)");

  // A closed fd: poll reports POLLNVAL -> select fails with EBADF (POSIX; libounce's poll() itself
  // returns -1 for an fd it cannot map).
  close(a);
  FD_ZERO(&r);
  FD_SET(a, &r);
  tv = {0, 0};
  errno = 0;
  rc = Sel(64, &r, nullptr, nullptr, &tv);
  Check(rc == -1 && errno == EBADF, "closed fd 63: -1 / EBADF (POLLNVAL)");

  // POLLERR alone (what a failed non-blocking connect can report) counts as readable and writable;
  // POLLPRI/POLLERR as exception. Fake poll() so the revents are exact.
  {
    fd_set rr, ww, ee;
    FD_ZERO(&rr);
    FD_ZERO(&ww);
    FD_ZERO(&ee);
    FD_SET(5, &rr);
    FD_SET(5, &ww);
    FD_SET(6, &ee);
    timeval z{0, 0};
    const int frc = SWITCH2NET::SelectViaPoll(
        7, &rr, &ww, &ee, &z,
        [](pollfd* f, nfds_t c, int) {
          for (nfds_t i = 0; i < c; ++i)
            f[i].revents = f[i].fd == 5 ? POLLERR : ((f[i].events & POLLPRI) ? POLLPRI : 0);
          return static_cast<int>(c);
        },
        [](int) {});
    Check(frc == 3 && FD_ISSET(5, &rr) && FD_ISSET(5, &ww) && FD_ISSET(6, &ee),
          "POLLERR -> readable + writable, POLLPRI requested and -> exception (3 bits)");
  }

  // Real failed non-blocking connect (refused on loopback): select() waiting for writability wakes.
  {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t sl = sizeof(sa);
    bind(l, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    getsockname(l, reinterpret_cast<sockaddr*>(&sa), &sl);
    close(l); // nobody listens on that port now
    int c = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
    const int crc = connect(c, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    const int cerr = errno;
    FD_ZERO(&w);
    FD_SET(c, &w);
    tv = {2, 0};
    t0 = std::chrono::steady_clock::now();
    rc = Sel(c + 1, nullptr, &w, nullptr, &tv);
    int soerr = 0;
    socklen_t el = sizeof(soerr);
    getsockopt(c, SOL_SOCKET, SO_ERROR, &soerr, &el);
    Check((crc == 0 || cerr == EINPROGRESS || cerr == ECONNREFUSED) && rc == 1 && FD_ISSET(c, &w) &&
              Ms(t0) < 1000,
          "failed non-blocking connect wakes a writability wait at once");
    std::printf("     (connect rc=%d errno=%d, SO_ERROR=%d)\n", crc, cerr, soerr);
    close(c);
  }

  std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
