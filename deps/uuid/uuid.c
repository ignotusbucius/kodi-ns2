/* Version-4 (random) UUIDs from the console RNG (libounce randomGet). */
#include "uuid.h"
#include <switch.h>

void uuid_generate_random(uuid_t out)
{
    randomGet(out, 16);
    out[6] = (unsigned char)((out[6] & 0x0F) | 0x40); /* version 4 */
    out[8] = (unsigned char)((out[8] & 0x3F) | 0x80); /* RFC 4122 variant */
}

void uuid_generate(uuid_t out)
{
    uuid_generate_random(out);
}
