#include "../doomdef.h"
#include "../command.h"
#include "../i_system.h"
#include "diagnostics.h"
#include "diagnostics_console.h"

#define REWRITE_DIAGNOSTIC_CAPACITY 256

static rewrite_diagnostic_event_t diagnostic_events[REWRITE_DIAGNOSTIC_CAPACITY];
static rewrite_diagnostic_buffer_t buffer;

static void RewriteDiagnostics_StartCommand(void)
{
    if (!RewriteDiagnostics_Init(&buffer, diagnostic_events, REWRITE_DIAGNOSTIC_CAPACITY))
        return;
    RewriteDiagnostics_Enable(&buffer, true);
    RewriteDiagnostics_Attach(&buffer);
    CONS_Printf("Rewrite diagnostics started.\n");
}

static void RewriteDiagnostics_StopCommand(void)
{
    RewriteDiagnostics_Attach(NULL);
    RewriteDiagnostics_Enable(&buffer, false);
    CONS_Printf("Rewrite diagnostics stopped.\n");
}

static bool RewriteDiagnostics_ConsoleSink(void *context,
    const rewrite_diagnostic_event_t *items, size_t count, size_t dropped)
{
    size_t i;
    (void)context;
    CONS_Printf("Rewrite diagnostics: %llu events, %llu dropped, %llu units per second.\n",
        (unsigned long long)count, (unsigned long long)dropped,
        (unsigned long long)I_GetPrecisePrecision());
    for (i = 0; i < count; i++)
        CONS_Printf("%llu %u %llu\n", (unsigned long long)items[i].sequence,
            (unsigned)items[i].metric, (unsigned long long)items[i].value);
    return true;
}

static void RewriteDiagnostics_DumpCommand(void)
{
    if (!buffer.count && !buffer.dropped)
    {
        CONS_Printf("Rewrite diagnostics: no events.\n");
        return;
    }
    RewriteDiagnostics_Flush(&buffer, RewriteDiagnostics_ConsoleSink, NULL);
}

void RewriteDiagnostics_ConsoleInit(void)
{
    COM_AddCommand("rwdiag_start", RewriteDiagnostics_StartCommand, COM_LOCAL);
    COM_AddCommand("rwdiag_stop", RewriteDiagnostics_StopCommand, COM_LOCAL);
    COM_AddCommand("rwdiag_dump", RewriteDiagnostics_DumpCommand, COM_LOCAL);
}
