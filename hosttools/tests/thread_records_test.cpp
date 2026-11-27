// Host test of xbmc/platform/switch2/ThreadRecords.inc (the rev 38f thread records) on host pthreads.
// Built and run by hosttools/check-net-select.sh (H2). Exit 0 = all cases pass.
//   T0 create + wrapped join drops the record.
//   T1 pthread_t value reuse after a join outside the wrapper: the new thread has exactly one record (a).
//   T2 self-detach before the creator links, with a stale record for the same value: the thread's own
//      record is detached and reaped; nothing is left (c).
//   T3 detach of a thread without a record: counted as leaked, never joined by anyone but its owner (b).
//   T4 ordinary detach of a recorded thread: reaped once it ends.
//   T5 two "launches" in one process (rev 38g): launch 1's reaper is stopped and joined at its exit; launch 2
//      (fresh state, as a reload at the same address gives) starts its own reaper, which alone reaps.
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define SWITCH2_DIAG 0

namespace
{
std::atomic<bool> g_holdCreate{false}; // T2: creator waits until the child has detached itself
std::atomic<bool> g_childDetached{false};
std::atomic<pthread_t> g_watch{};
std::atomic<int> g_watchJoins{0}; // __real_pthread_join calls on g_watch
} // namespace

extern "C" int __real_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*f)(void*), void* arg)
{
  const int rc = pthread_create(t, a, f, arg);
  if (rc == 0 && g_holdCreate.load())
    for (int i = 0; i < 2000 && !g_childDetached.load(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return rc;
}
extern "C" int __real_pthread_join(pthread_t t, void** v)
{
  if (pthread_equal(t, g_watch.load()))
    g_watchJoins.fetch_add(1);
  return pthread_join(t, v);
}
extern "C" void __real_pthread_exit(void* v)
{
  pthread_exit(v);
}

std::atomic<long> g_switch2ThreadsLive{0};
std::atomic<long> g_switch2ThreadsZombie{0};
std::atomic<long> g_switch2ThreadsReapQueued{0};
namespace
{
std::atomic<long> g_threadsUntracked{0};
void ThreadHandleBegin(int*) {}
void ThreadHandleEnded(int) {}
void ThreadHandleJoined(int) {}
void DetachUnknownLeaked(long) {}
void ReaperStarted(int) {}
std::atomic<int> g_reaperStoppedRc{-1};
void ReaperStopped(int rc)
{
  g_reaperStoppedRc.store(rc);
}
} // namespace

#include "../../xbmc/xbmc/platform/switch2/ThreadRecords.inc"

namespace
{
int g_fail = 0;
void Check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  g_fail += !ok;
}
int Count(pthread_t t)
{
  std::lock_guard<std::mutex> lock(g_thrMutex);
  return CountRecordsLocked(t);
}
int Total()
{
  std::lock_guard<std::mutex> lock(g_thrMutex);
  int n = 0;
  for (ThreadRec* r = g_thrHead; r; r = r->next)
    ++n;
  return n;
}
bool WaitFor(bool (*cond)(), int ms)
{
  for (int i = 0; i < ms; ++i)
  {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return cond();
}
void* Quick(void*)
{
  return nullptr;
}
void* Sleep20(void*)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return nullptr;
}
void* SelfDetach(void*)
{
  __wrap_pthread_detach(pthread_self());
  g_childDetached.store(true);
  return nullptr;
}
// A record whose thread was joined outside the wrapper; returns its value.
pthread_t MakeStale()
{
  pthread_t a;
  CountedCreate(&a, nullptr, Quick, nullptr, false);
  pthread_join(a, nullptr); // not __wrap_pthread_join: the record stays linked (stale)
  return a;
}
} // namespace

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // T0
  {
    pthread_t t;
    CountedCreate(&t, nullptr, Quick, nullptr, false);
    const int rc = __wrap_pthread_join(t, nullptr);
    Check(rc == 0 && Count(t) == 0 && Total() == 0 && g_switch2ThreadsZombie.load() == 0,
          "T0 create + wrapped join: no record left, zombie 0");
  }
  // T4 first: it also starts the reaper, which T2's held create must not be the first to do
  {
    pthread_t d;
    CountedCreate(&d, nullptr, Sleep20, nullptr, false);
    __wrap_pthread_detach(d);
    static pthread_t s_d;
    s_d = d;
    const bool gone = WaitFor([] { return Count(s_d) == 0; }, 2000);
    Check(gone && Total() == 0 && g_switch2ThreadsReapQueued.load() == 0 &&
              g_switch2ThreadsZombie.load() == 0,
          "T4 detach of a recorded thread: reaped after it ends, nothing left");
  }
  // T1
  {
    bool reused = false;
    for (int i = 0; i < 50 && !reused; ++i)
    {
      const pthread_t a = MakeStale();
      const bool staleSeen = Count(a) == 1;
      pthread_t b;
      CountedCreate(&b, nullptr, Sleep20, nullptr, false);
      if (pthread_equal(a, b))
      {
        reused = true;
        Check(staleSeen, "T1 setup: the out-of-wrapper join left a stale record");
        Check(Count(b) == 1, "T1 value reused: the new thread has exactly one record (stale one dropped)");
        __wrap_pthread_join(b, nullptr);
        Check(Count(b) == 0, "T1 wrapped join of the new thread: no record for the value");
      }
      else
      {
        __wrap_pthread_join(b, nullptr);
        std::lock_guard<std::mutex> lock(g_thrMutex);
        RemoveStaleLocked(a, g_thrHead); // clean the unrelated stale record
      }
    }
    Check(reused, "T1 host reused a pthread_t value within 50 tries");
  }
  // T2
  {
    bool reused = false;
    for (int i = 0; i < 50 && !reused; ++i)
    {
      const pthread_t a = MakeStale();
      g_childDetached.store(false);
      g_holdCreate.store(true);
      pthread_t c;
      CountedCreate(&c, nullptr, SelfDetach, nullptr, false); // child detaches itself before the link
      g_holdCreate.store(false);
      if (!pthread_equal(a, c))
      {
        // unrelated: wait for the reaper, drop the stale record
        WaitFor([] { return g_switch2ThreadsReapQueued.load() == 0; }, 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lock(g_thrMutex);
        RemoveStaleLocked(a, g_thrHead);
        continue;
      }
      reused = true;
      static pthread_t s_c;
      s_c = c;
      const bool gone = WaitFor([] { return Count(s_c) == 0; }, 2000);
      Check(g_childDetached.load(), "T2 setup: the child detached itself before the creator linked it");
      Check(gone && Total() == 0 && g_switch2ThreadsZombie.load() == 0,
            "T2 self-detach with a stale record: own record reaped, stale dropped, nothing left");
    }
    Check(reused, "T2 host reused a pthread_t value within 50 tries");
  }
  // T3
  {
    const long before = g_threadsDetachLeaked.load();
    pthread_t u;
    pthread_create(&u, nullptr, Sleep20, nullptr); // no record
    g_watch.store(u);
    g_watchJoins.store(0);
    __wrap_pthread_detach(u);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int joinsByWrapper = g_watchJoins.load();
    Check(joinsByWrapper == 0 && g_threadsDetachLeaked.load() == before + 1,
          "T3 detach without a record: counted leaked, never joined by the wrapper/reaper");
    if (joinsByWrapper == 0)
    {
      timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;
      Check(pthread_timedjoin_np(u, nullptr, &ts) == 0, "T3 the owner can still join it (no double join)");
    }
    g_watch.store(pthread_t{});
  }
  // T5
  {
    const int stopped = Switch2StopReaper();
    Check(stopped == 1 && g_reaperStoppedRc.load() == 0 && Switch2StopReaper() == 0,
          "T5 launch 1 exit: reaper stopped and joined (rc 0), second stop is a no-op");
    pthread_t late;
    CountedCreate(&late, nullptr, Quick, nullptr, false);
    __wrap_pthread_detach(late); // after the stop: not reaped, no reaper restarted in this launch
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Check(Count(late) == 1 && !g_reaperRunning, "T5 detach after the stop: not reaped, reaper not restarted");
    __wrap_pthread_join(late, nullptr); // clean up for the second launch
    // "Launch 2": the reload re-zeroes .bss; the stopped reaper no longer exists.
    {
      std::lock_guard<std::mutex> ctl(g_reaperCtl);
      g_reaperStarted = false;
    }
    {
      std::lock_guard<std::mutex> lock(g_thrMutex);
      g_reaperStop = false;
    }
    pthread_t d2;
    CountedCreate(&d2, nullptr, Sleep20, nullptr, false);
    __wrap_pthread_detach(d2);
    static pthread_t s_d2;
    s_d2 = d2;
    const bool gone = WaitFor([] { return Count(s_d2) == 0; }, 2000);
    Check(gone && g_reaperRunning,
          "T5 launch 2: a new reaper started and reaped its detached thread");
    Switch2StopReaper();
  }
  std::printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  std::fflush(stdout);
  std::_Exit(g_fail ? 1 : 0); // the reaper still waits on g_reapCv: no static destructors
}
