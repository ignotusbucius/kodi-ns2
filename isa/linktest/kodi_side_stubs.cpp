// Test-only stand-ins for the symbols the packed add-on takes from kodi.bin itself (not from the
// toolchain libraries). In kodi.bin they come from xbmc/platform/switch2/PosixCompat.cpp.
#include <ctime>
extern "C" time_t timegm(struct tm*)
{
  return 0;
}
