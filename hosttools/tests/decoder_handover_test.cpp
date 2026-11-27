// Host test for the rev 39c decoder-slot handover (Switch2HwCodecUtil.h SlotTryClaim/SlotClaimWithin).
// Build: g++ -std=c++17 -Wall -Wextra -O1 -pthread -I../../xbmc/xbmc/cores/VideoPlayer/DVDCodecs/Video
//        decoder_handover_test.cpp
// Model of CDVDVideoCodecSwitch2's slot use in VideoPlayer's order of calls:
//   VideoPlayer thread: new codec Open() while the old one decodes;
//   VideoPlayerVideo thread: old->Reconfigure() fails -> old destroyed (release) -> new first packet (claim).
#include "Switch2HwCodecUtil.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> g_slot{false};
std::atomic<int> g_live{0}; // live "hardware decoders"
std::atomic<int> g_maxLive{0};
std::atomic<bool> g_unavailable{false};
int g_fail = 0;

unsigned long long NowMs()
{
  return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
}

void Check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    ++g_fail;
}

struct Codec
{
  bool claimed = false;
  bool handover = false;
  bool hw = false; // decoder created
  bool software = false;
  unsigned waited = 0;

  void Open()
  {
    if (SWITCH2HW::SlotTryClaim(g_slot))
      claimed = true;
    else
      handover = true;
  }
  void FirstPacket(unsigned waitMs = 500)
  {
    if (handover)
    {
      handover = false;
      if (!SWITCH2HW::SlotClaimWithin(
              g_slot, waitMs, NowMs,
              [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); },
              [] { return g_unavailable.load(); }, waited))
      {
        software = true;
        return;
      }
      claimed = true;
    }
    if (!claimed)
    {
      software = true;
      return;
    }
    hw = true;
    const int now = ++g_live;
    int prev = g_maxLive.load();
    while (now > prev && !g_maxLive.compare_exchange_weak(prev, now))
    {
    }
  }
  ~Codec()
  {
    if (hw)
      --g_live;
    if (claimed)
      g_slot.store(false);
  }
};
} // namespace

int main()
{
  // T1: adaptive up-switch (Reconfigure refused): handover, no wait, new codec on hardware.
  {
    auto old = std::make_unique<Codec>();
    old->Open();
    old->FirstPacket();
    auto fresh = std::make_unique<Codec>();
    fresh->Open(); // VideoPlayer thread, old still decoding
    Check(fresh->handover && !fresh->claimed, "T1 open during the old decoder's life: handover pending");
    old.reset(); // CVideoPlayerVideo::OpenStream: m_pVideoCodec.reset()
    fresh->FirstPacket();
    Check(fresh->hw && fresh->waited < 20, "T1 first packet after the old codec's destruction: hardware");
    Check(g_maxLive.load() == 1, "T1 never two live decoders");
  }
  Check(!g_slot.load(), "T1 slot free after both destroyed");

  // T2: down-switch kept by Reconfigure: the unused new codec is destroyed without touching the slot.
  {
    Codec old;
    old.Open();
    old.FirstPacket();
    {
      Codec unused;
      unused.Open();
      Check(unused.handover, "T2 unused codec pending");
    }
    Check(g_slot.load() && old.hw, "T2 old decoder keeps the slot after the unused codec is destroyed");
  }

  // T3: release in flight on another thread within the bounded wait.
  {
    auto old = std::make_unique<Codec>();
    old->Open();
    old->FirstPacket();
    Codec fresh;
    fresh.Open();
    std::thread t([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      old.reset();
    });
    fresh.FirstPacket(500);
    t.join();
    Check(fresh.hw && fresh.waited >= 100 && fresh.waited < 500, "T3 claim after a 120 ms release");
  }

  // T4: holder never released: bounded wait, then software; slot untouched.
  {
    Codec holder;
    holder.Open();
    holder.FirstPacket();
    Codec fresh;
    fresh.Open();
    const auto t0 = NowMs();
    fresh.FirstPacket(200);
    const auto took = NowMs() - t0;
    Check(fresh.software && !fresh.claimed && took >= 200 && took < 400,
          "T4 holder kept: software after the 200 ms bound");
    Check(holder.hw && g_slot.load(), "T4 holder still owns the slot");
  }

  // T5: old decoder's work memory lost (hardware off for the run): no claim, no wait.
  {
    auto old = std::make_unique<Codec>();
    old->Open();
    old->FirstPacket();
    Codec fresh;
    fresh.Open();
    old.reset();
    g_unavailable = true;
    fresh.FirstPacket();
    Check(fresh.software && !g_slot.load(), "T5 hardware unavailable: software, slot not taken");
    g_unavailable = false;
  }

  // T6: stress: 8 threads open/claim/destroy 2000 times; at most one live decoder at any time.
  {
    g_maxLive = 0;
    std::vector<std::thread> ts;
    std::atomic<int> hwCount{0};
    for (int i = 0; i < 8; ++i)
      ts.emplace_back([&] {
        for (int k = 0; k < 2000; ++k)
        {
          Codec c;
          c.Open();
          c.FirstPacket(2);
          if (c.hw)
            ++hwCount;
        }
      });
    for (auto& t : ts)
      t.join();
    Check(g_maxLive.load() == 1 && hwCount.load() > 0 && !g_slot.load(),
          "T6 stress: one live decoder at most, slot free at the end");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
