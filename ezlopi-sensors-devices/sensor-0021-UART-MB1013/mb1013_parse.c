/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    mb1013_parse.c
 * @brief   MB1013 range-frame parsing. See mb1013_parse.h for why this is a
 *          separate translation unit and what the bug was.
 */

/*******************************************************************************
 *                          Include Files
 *******************************************************************************/

/* For NULL. It used to arrive transitively through the cloud and item headers
   in sensor_0021_UART_MB1013.c; this translation unit includes none of those,
   which is the point, so it has to ask. */
#include <stddef.h>

#include "mb1013_parse.h"

/*******************************************************************************
 *                          Static Function Definitions
 *******************************************************************************/

/**
 * @brief Parse the four payload bytes of one frame.
 *
 * Reads exactly buffer[0..3] -- the caller has already established that those
 * four bytes are inside the buffer -- and never looks for a terminator, which
 * is the whole reason this does not call atoi(). strtol()'s habit of skipping
 * whitespace is what let the old code step over the frame's own '\r'.
 *
 * Returns false if there is no digit to parse, in which case *value is
 * untouched.
 */
static bool __parse_payload(const uint8_t *payload, uint32_t *value)
{
  uint32_t acc = 0;
  bool seen_digit = false;
  bool in_leading_space = true;

  for (uint32_t i = 0; i < (MB1013_FRAME_LEN - 2); i++)
  {
    uint8_t c = payload[i];

    /* The part emits zero-padded digits, but a space-padded payload was read
       correctly by the old atoi() path, so it still is. Only leading spaces
       are skipped: anything after a digit ends the number. */
    if (in_leading_space && ((' ' == c) || ('\t' == c)))
    {
      continue;
    }
    in_leading_space = false;

    if ((c < '0') || (c > '9'))
    {
      /* Where atoi() stopped as well, so "12z3" still reads as 12. */
      break;
    }

    seen_digit = true;
    acc = (acc * 10u) + (uint32_t)(c - '0');
  }

  if (!seen_digit)
  {
    return false;
  }

  *value = acc;
  return true;
}

/*******************************************************************************
 *                          Extern Function Definitions
 *******************************************************************************/

bool EZPI_mb1013_parse_range(const uint8_t *buffer, uint32_t output_len, float *out_value)
{
  if ((NULL == buffer) || (NULL == out_value))
  {
    return false;
  }

  /* Tested before the loop so that the bound below cannot wrap: a frame needs
     six bytes, and output_len is unsigned. */
  if (output_len < MB1013_FRAME_LEN)
  {
    return false;
  }

  for (uint32_t idx = 0; idx <= (output_len - MB1013_FRAME_LEN); idx++)
  {
    /* Identical frame acceptance to the original "(idx + 5) < output_len":
       both say the whole six-byte frame has to be inside the buffer. An 'R'
       within the final five bytes cannot be a complete reading, and matching
       one there against whatever happened to follow the allocation was the one
       way a truncated frame could be mistaken for a good one. */
    if (('R' != buffer[idx]) || ('\r' != buffer[idx + (MB1013_FRAME_LEN - 1)]))
    {
      continue;
    }

    uint32_t raw = 0;
    if (__parse_payload(&buffer[idx + 1], &raw))
    {
      /* Division in double and then narrowed, exactly as the original
         "atoi(...) / 10.0" assigned to a float did. */
      *out_value = (float)(raw / 10.0);
      return true;
    }

    /* A frame whose payload holds no digit is not a reading. The old code
       reported 0.0 for it and stopped looking; carrying on means a malformed
       frame no longer masks a good one later in the same buffer. */
  }

  return false;
}

/*******************************************************************************
 *                          End of File
 *******************************************************************************/
