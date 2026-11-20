/* Minimal libuuid stand-in for the Switch 2 Kodi port: version-4 UUIDs from the console RNG. */
#ifndef SWITCH2_UUID_H
#define SWITCH2_UUID_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned char uuid_t[16];
void uuid_generate(uuid_t out);
void uuid_generate_random(uuid_t out);
#ifdef __cplusplus
}
#endif
#endif
