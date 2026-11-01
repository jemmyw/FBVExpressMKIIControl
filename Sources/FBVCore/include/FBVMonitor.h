#ifndef FBV_MONITOR_H
#define FBV_MONITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FbvMonitor FbvMonitor;
typedef enum { FBV_MONITOR_STOPPED, FBV_MONITOR_LISTENING, FBV_MONITOR_DISCONNECTED } FbvMonitorState;
typedef struct {
    uint8_t port, channel, cc, value;
    double received; /* Monotonic receipt seconds, not the device timestamp. */
} FbvCCEvent;
/* Public calls must be serialised by the owner. Callbacks never call the owner.
   Demo creates no CoreMIDI objects. Drain is bounded to 256 queued events. */
FbvMonitor *fbv_monitor_create(bool demo);
bool fbv_monitor_start(FbvMonitor *monitor);
void fbv_monitor_stop(FbvMonitor *monitor);
void fbv_monitor_destroy(FbvMonitor *monitor);
FbvMonitorState fbv_monitor_drain(FbvMonitor *monitor, FbvCCEvent *events,
                                 size_t capacity, size_t *count, uint64_t *dropped);
/* Offline injection uses the actual receive/parser/queue path; refused on real monitors.
   At most 4096 bytes per call, ports 1..3 only. */
bool fbv_monitor_demo_feed(FbvMonitor *monitor, unsigned port, const uint8_t *bytes, size_t count);
bool fbv_monitor_demo_disconnect(FbvMonitor *monitor);
#endif
