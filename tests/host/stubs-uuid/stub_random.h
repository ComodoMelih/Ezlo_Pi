/* Test-side controls for the esp_fill_random() stub.
 *
 * Kept out of the esp_random.h stub so that stub stays a faithful copy of the
 * real header's surface -- nothing the firmware includes can see these.
 */
#ifndef _HOST_STUB_RANDOM_H_
#define _HOST_STUB_RANDOM_H_

#include <stddef.h>

/* Bytes handed out so far, and calls made. Reset before an assertion that
   depends on the sequence. */
extern size_t g_stub_random_bytes;
extern size_t g_stub_random_calls;

void stub_random_reset(void);

/* The byte this stub would hand out as the n'th of the sequence. The test uses
   it to predict the whole buffer, which is the point: a counter is
   predictable, so "every byte came from esp_fill_random()" becomes a
   checkable claim rather than a hope. */
unsigned char stub_random_byte_at(size_t n);

#endif /* _HOST_STUB_RANDOM_H_ */
