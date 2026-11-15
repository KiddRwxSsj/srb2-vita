#ifndef SRB2_REWRITE_DIAGNOSTICS_H
#define SRB2_REWRITE_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint64_t sequence;
    uint64_t value;
    uint32_t metric;
} rewrite_diagnostic_event_t;

typedef struct
{
    rewrite_diagnostic_event_t *events;
    size_t capacity;
    size_t count;
    size_t dropped;
    bool enabled;
} rewrite_diagnostic_buffer_t;

typedef bool (*rewrite_diagnostic_sink_t)(void *context,
    const rewrite_diagnostic_event_t *events, size_t count, size_t dropped);

enum { REWRITE_DIAGNOSTIC_TIC_TIME = 1 };

void RewriteDiagnostics_Attach(rewrite_diagnostic_buffer_t *buffer);
void RewriteDiagnostics_RecordTick(uint64_t sequence, uint64_t duration);

bool RewriteDiagnostics_Flush(rewrite_diagnostic_buffer_t *buffer,
    rewrite_diagnostic_sink_t sink, void *context);
bool RewriteDiagnostics_Init(rewrite_diagnostic_buffer_t *buffer,
    rewrite_diagnostic_event_t *storage, size_t capacity);
void RewriteDiagnostics_Enable(rewrite_diagnostic_buffer_t *buffer, bool enabled);
bool RewriteDiagnostics_Record(rewrite_diagnostic_buffer_t *buffer,
    uint64_t sequence, uint32_t metric, uint64_t value);
void RewriteDiagnostics_Clear(rewrite_diagnostic_buffer_t *buffer);

#ifdef SRB2_REWRITE_DIAGNOSTICS
#define REWRITE_DIAGNOSTICS_RECORD(buffer, sequence, metric, value) \
    RewriteDiagnostics_Record((buffer), (sequence), (metric), (value))
#else
#define REWRITE_DIAGNOSTICS_RECORD(buffer, sequence, metric, value) (false)
#endif

#endif
