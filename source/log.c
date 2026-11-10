/* wafel_screen_log - lock-free text ring.
 *
 * Writers: debug_printf/semihosting call sites in wafel_core (kernel, abort,
 * svc and MCP user mode) and MCP's syslog ring writer (MCP threads).
 * There is no lock: user-mode threads can't mask IRQs and kernel/abort
 * contexts can't block. The only shared state is a monotonically growing
 * byte counter; all indexing is masked, so a race can at worst interleave
 * characters of two messages on screen. */
#include "scrlog.h"

char log_ring[LOG_RING_SIZE];
volatile u32 log_wpos;

void log_append(const char *s, u32 len)
{
    if (!s)
        return;
    u32 pos = log_wpos;
    for (u32 i = 0; i < len; i++) {
        char c = s[i];
        if (len == (u32)-1 && c == '\0')
            break;
        if (c == '\r' || c == '\0')
            continue;
        if (c == '\t')
            c = ' ';
        log_ring[pos & LOG_RING_MASK] = c;
        pos++;
        log_wpos = pos;
    }
}

void log_puts(const char *s)
{
    log_append(s, (u32)-1);
}
