#include "diagnostics.h"

static rewrite_diagnostic_buffer_t *tick_buffer;

void RewriteDiagnostics_Attach(rewrite_diagnostic_buffer_t *buffer)
{
    tick_buffer = buffer;
}

void RewriteDiagnostics_RecordTick(uint64_t sequence, uint64_t duration)
{
    RewriteDiagnostics_Record(tick_buffer, sequence, REWRITE_DIAGNOSTIC_TIC_TIME, duration);
}

bool RewriteDiagnostics_Init(rewrite_diagnostic_buffer_t *buffer,
    rewrite_diagnostic_event_t *storage, size_t capacity)
{
    if (!buffer || (!storage && capacity))
        return false;
    buffer->events = storage;
    buffer->capacity = capacity;
    buffer->count = 0;
    buffer->dropped = 0;
    buffer->enabled = false;
    return true;
}

void RewriteDiagnostics_Enable(rewrite_diagnostic_buffer_t *buffer, bool enabled)
{
    if (buffer)
        buffer->enabled = enabled;
}

bool RewriteDiagnostics_Record(rewrite_diagnostic_buffer_t *buffer,
    uint64_t sequence, uint32_t metric, uint64_t value)
{
    rewrite_diagnostic_event_t *event;
    if (!buffer || !buffer->enabled)
        return false;
    if (buffer->count >= buffer->capacity)
    {
        if (buffer->dropped != SIZE_MAX)
            buffer->dropped++;
        return false;
    }
    event = &buffer->events[buffer->count++];
    event->sequence = sequence;
    event->metric = metric;
    event->value = value;
    return true;
}

void RewriteDiagnostics_Clear(rewrite_diagnostic_buffer_t *buffer)
{
    if (buffer)
    {
        buffer->count = 0;
        buffer->dropped = 0;
    }
}

bool RewriteDiagnostics_Flush(rewrite_diagnostic_buffer_t *buffer,
    rewrite_diagnostic_sink_t sink, void *context)
{
    if (!buffer || !sink)
        return false;
    if (!buffer->count && !buffer->dropped)
        return true;
    if (!sink(context, buffer->events, buffer->count, buffer->dropped))
        return false;
    buffer->count = 0;
    buffer->dropped = 0;
    return true;
}
