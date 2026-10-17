/* ===========================================================================
** Copyright (C) 2025 Ezlo Innovation Inc
**
** Under EZLO AVAILABLE SOURCE LICENSE (EASL) AGREEMENT
** ===========================================================================
*/

/**
 * @file    stub_httpurl.h
 * @brief   What test_httpurl.c reads out of stub_httpurl.c.
 *
 * Only the recorder is shared. The thirteen stubbed firmware symbols are
 * declared by the real headers, which is the point -- a prototype written here
 * could disagree with the firmware's and the link would still succeed.
 */

#ifndef _TEST_STUB_HTTPURL_H_
#define _TEST_STUB_HTTPURL_H_

/* Nine is the most EZPI_parse_http_url can produce for one URL (url,
 * web_server, target_page, header, username, password, and the header's own
 * fresh-allocation) with room to spare; 32 leaves space for a test that drives
 * several fields through one struct. The stub aborts rather than silently
 * dropping records if this is ever too small.
 */
#define STUB_MALLOC_LOG_MAX 32

typedef struct s_stub_malloc_log
{
    char **dest; /* which struct member the subject aimed at */
    char src[512];
} s_stub_malloc_log_t;

extern s_stub_malloc_log_t g_stub_malloc_log[STUB_MALLOC_LOG_MAX];
extern int g_stub_malloc_log_n;

void stub_httpurl_reset(void);

/* The nth recorded source string, or NULL if there were fewer than n+1. */
const char *stub_malloc_arg(int nth);

#endif /* _TEST_STUB_HTTPURL_H_ */
