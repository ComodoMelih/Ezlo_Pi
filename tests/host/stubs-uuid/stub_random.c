/* esp_fill_random() for the host, as a counter rather than a PRNG.
 *
 * Randomness is not what the UUID test can check. What it can check is that
 * every one of the sixteen bytes was written from this source -- which a
 * predictable sequence makes decidable, and rand() would not.
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_random.h"
#include "stub_random.h"

size_t g_stub_random_bytes = 0;
size_t g_stub_random_calls = 0;

void stub_random_reset(void)
{
    g_stub_random_bytes = 0;
    g_stub_random_calls = 0;
}

/* 0xA5 offset by the index, so the sequence is neither all-distinct-from-zero
   by accident nor a run of small integers that a stack slot might plausibly
   already hold. */
unsigned char stub_random_byte_at(size_t n)
{
    return (unsigned char)(0xA5u + n);
}

void esp_fill_random(void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t i;

    g_stub_random_calls++;

    for (i = 0; i < len; i++)
    {
        p[i] = stub_random_byte_at(g_stub_random_bytes + i);
    }

    g_stub_random_bytes += len;
}

uint32_t esp_random(void)
{
    uint32_t v = 0;
    esp_fill_random(&v, sizeof(v));
    return v;
}
