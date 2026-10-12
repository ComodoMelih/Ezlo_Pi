/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice,
**    this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. Neither the name of the copyright holder nor the names of its
**    contributors may be used to endorse or promote products derived from
**    this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
** POSSIBILITY OF SUCH DAMAGE.
** ===========================================================================
*/

/**
 * @file    main.c
 * @brief   perform some function on data
 * @author
 * @version 0.1
 * @date    1st January 2024
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/
#include <stdlib.h>
#include <string.h>
#include <time.h>

// #include "config.h"
#include "ezlopi_util_uuid.h"

/*******************************************************************************
 *                          Extern Data Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Declarations
 *******************************************************************************/

/*******************************************************************************
 *                          Type & Macro Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Static Function Prototypes
 *******************************************************************************/
static int __hex2dec(char c);

/*******************************************************************************
 *                          Static Data Definitions
 *******************************************************************************/
/* Was `const unsigned char hex[16]`, which is legal C99 -- an array initialised
   from a string literal of exactly its own length drops the NUL (6.7.8p14) --
   but gcc 15 added -Wunterminated-string-initialization, which this would trip
   on the day the toolchain moves. Letting the compiler size it keeps the NUL
   and costs one byte of rodata. Nothing here reads it: both call sites index
   with (uuid[j] >> 4) and (0xf & uuid[j]) on a uint8_t, and __hex2dec() stops
   at i < 16, so index 16 was unreachable either way. Static because no header
   declares this and no other translation unit refers to it, so a name this
   generic has no business in the firmware's global namespace. */
static const unsigned char hex[] = "0123456789abcdef";

/*******************************************************************************
 *                          Extern Data Definitions
 *******************************************************************************/

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

/**
 * @brief Global/extern function template example
 * Convention : Use capital letter for initial word on extern function
 * @param arg
 */
void ezlopi_util_uuid_generate_random(ezlopi_uuid_t out)
{
    int i, j, rnd;
    static int seeded = 0;

    if (!seeded)
    {
        srand(time(NULL));
        seeded = 1;
    }

    for (i = 0; i < (16 / RAND_LENGTH); i++)
    {
        rnd = rand();

        for (j = 0; j < RAND_LENGTH; j++)
        {
            out[i * RAND_LENGTH + j] = (0xff & rnd >> (8 * j));
        }
    }

    // set the version to 4
    out[6] = (out[6] & 0x0f) | 0x40;

    // set the variant to 1 (a)
    out[8] = (out[8] & 0x0f) | 0xa0;
}

/* Rewritten. The previous version had four defects, none of them reachable --
   nothing in this repository calls any function in this component -- but all
   four demonstrated by compiling this file on a host and running it, because
   it depends on nothing but <stdlib.h> and <time.h>:

     - It read past the end of its input. The only bounds were j < 16 and
       i < 36 and the body read in[i + 1], so nothing consulted the
       terminator. Parsing a 9-byte heap buffer holding "550e8400" gave
       AddressSanitizer "heap-buffer-overflow READ of size 1 ... 0 bytes after
       9-byte region". That is the serious one: a UUID arriving from the cloud
       or a scene is attacker-influenced in both contents and length.

     - It advanced by 2 on the iteration that saw a '-'. A hyphen is one
       character, so that consumed the hyphen and the first digit after it,
       and every byte from there on was read one nibble out of step. The
       canonical form this component's own unparse() emits,
       "550e8400-e29b-41d4-a716-446655440000", came back as
       550e840029ff41d471ff446655440000 -- wrong from byte [4], which is
       exactly where the first hyphen sits. So the two halves of this
       component did not round-trip through each other.

     - __hex2dec() compared against a lowercase-only table with ==, so 'A'
       through 'F' returned -1. Verified to be an independent defect rather
       than a knock-on of the hyphen skip by parsing uppercase with the
       hyphens already removed, which was still wrong. RFC 4122 s3 requires a
       reader to accept both cases.

     - The -1 that __hex2dec() returns for an unrecognised character went
       straight into (x << 4) | y unchecked. Left-shifting a negative value is
       undefined behaviour, and UndefinedBehaviorSanitizer duly reported
       "left shift of negative value -1"; the result was stored as 0xff, so
       malformed input silently produced plausible-looking bytes.

   The signature is deliberately unchanged. Returning a bool would be
   source-compatible, since every call would be a statement that ignores it,
   but this is a published header in a library with no callers and an API
   change is not worth arguing about in review. Instead the failure mode is at
   least made deterministic: malformed or short input now yields the nil UUID
   (all zero) rather than a half-written buffer. A caller still cannot tell
   "the input was the nil UUID" from "the input was rejected", which is a real
   limitation and is noted in the header rather than papered over. */
void ezlopi_util_uuid_parse(const char *in, ezlopi_uuid_t uuid)
{
    int i = 0;
    int j = 0;
    int bad = 0;

    if (NULL == uuid)
    {
        return;
    }

    if (NULL == in)
    {
        bad = 1;
    }

    while ((0 == bad) && (j < 16) && ('\0' != in[i]))
    {
        /* A hyphen is one character, so advance by one. Advancing by two here
           was the misalignment bug: it consumed the hyphen and the first digit
           after it. */
        if ('-' == in[i])
        {
            i++;
            continue;
        }

        /* Check for the terminator before looking at the second digit, so a
           string ending mid-byte stops here rather than being read past its
           NUL. This is the test the old loop did not have. */
        const int hi = __hex2dec(in[i]);
        const int lo = ('\0' == in[i + 1]) ? -1 : __hex2dec(in[i + 1]);

        if ((hi < 0) || (lo < 0))
        {
            bad = 1;
            break;
        }

        uuid[j++] = (unsigned char)((hi << 4) | lo);
        i += 2;
    }

    /* Anything short of a complete 16 bytes is a failure. Zero all of it
       rather than leaving the bytes that did parse in place: a half-written
       UUID is the worst of the three options, because it looks like a
       successful parse of a different UUID. All-zero is at least a value a
       caller can recognise as suspicious. */
    if ((0 != bad) || (16 != j))
    {
        memset(uuid, 0, sizeof(ezlopi_uuid_t));
    }
}

void ezlopi_util_uuid_unparse(const ezlopi_uuid_t uuid, char *out)
{
    int i, j;

    i = j = 0;

    do
    {
        switch (j)
        {
        case 4:
        case 6:
        case 8:
        case 10:
            out[i++] = '-';
        }

        out[i++] = hex[(uuid[j] >> 4)];
        out[i++] = hex[(0xf & uuid[j])];

        j++;
    } while (j < 16);

    out[36] = 0;
}

void ezlopi_util_uuid_copy(ezlopi_uuid_t dst, const ezlopi_uuid_t src)
{
    /* Was `int i`, compared against sizeof -- a signed/unsigned comparison
       that -Wextra reports and that the host test suite's -Werror rejects.
       Harmless in itself; fixed because this file now compiles under those
       flags rather than only under the firmware build's looser ones. */
    size_t i;
    for (i = 0; i < sizeof(ezlopi_uuid_t); i++)
    {
        dst[i] = src[i];
    }
}

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

static int __hex2dec(char c)
{
    int i;

    /* hex[] holds lowercase digits only and the comparison below is exact, so
       'A' through 'F' used to fall through to the -1 return. RFC 4122 s3 says
       a reader must accept either case. Folded with an explicit range rather
       than tolower() to avoid pulling in <ctype.h> and its locale dependency
       for six characters. */
    if ((c >= 'A') && (c <= 'F'))
    {
        c = (char)(c - 'A' + 'a');
    }

    for (i = 0; i < 16; i++)
    {
        if (hex[i] == c)
            return i;
    }

    return -1;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
