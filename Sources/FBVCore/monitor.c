#include "FBVMonitor.h"
#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUEUE_SIZE 256

typedef struct { uint8_t status, first, used; } CCParser;
struct FbvMonitor {
    bool demo;
    uintptr_t token;
    FbvMonitorState state;
    MIDIClientRef client;
    MIDIPortRef input;
    CCParser parsers[3];
    FbvCCEvent events[QUEUE_SIZE];
    size_t head, count;
    uint64_t dropped;
    struct FbvMonitor *next;
};

/* Callback refcons are never dereferenced. Removing a token under this lock
   makes even a late callback harmless after disposal or restart. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static FbvMonitor *registry;
static uintptr_t next_token = 1;

static FbvMonitor *lookup(uintptr_t token) {
    for (FbvMonitor *m = registry; m; m = m->next)
        if (m->token == token) return m;
    return NULL;
}

static void clear(FbvMonitor *m) {
    memset(m->parsers, 0, sizeof(m->parsers));
    m->head = m->count = 0;
    m->dropped = 0;
}

static double receipt(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static void feed(FbvMonitor *m, unsigned port, const uint8_t *bytes, size_t count) {
    if (m->state != FBV_MONITOR_LISTENING || port < 1 || port > 3) return;
    CCParser *p = &m->parsers[port - 1];
    for (size_t i = 0; i < count; ++i) {
        uint8_t byte = bytes[i];
        if (byte >= 0xf8) continue;
        if (byte & 0x80) {
            p->status = byte < 0xf0 ? byte : 0;
            p->used = 0;
            continue;
        }
        if (!p->status) continue;
        unsigned length = (p->status & 0xe0) == 0xc0 ? 1 : 2;
        if (!p->used) { p->first = byte; p->used = 1; }
        else {
            if ((p->status & 0xf0) == 0xb0) {
                if (m->count == QUEUE_SIZE) {
                    m->head = (m->head + 1) % QUEUE_SIZE;
                    --m->count;
                    if (m->dropped != UINT64_MAX) ++m->dropped;
                }
                size_t tail = (m->head + m->count++) % QUEUE_SIZE;
                m->events[tail] = (FbvCCEvent){(uint8_t)port, (p->status & 15) + 1,
                                              p->first, byte, receipt()};
            }
            p->used = 0;
        }
        if (length == 1) p->used = 0;
    }
}

static void receive(const MIDIPacketList *packets, void *context, void *source) {
    pthread_mutex_lock(&mutex);
    FbvMonitor *m = lookup((uintptr_t)context);
    if (m) {
        const MIDIPacket *packet = &packets->packet[0];
        for (UInt32 i = 0; i < packets->numPackets; ++i) {
            feed(m, (unsigned)(uintptr_t)source, packet->data, packet->length);
            packet = MIDIPacketNext(packet);
        }
    }
    pthread_mutex_unlock(&mutex);
}

static void notification(const MIDINotification *note, void *context) {
    if (note->messageID != kMIDIMsgSetupChanged && note->messageID != kMIDIMsgObjectRemoved &&
        note->messageID != kMIDIMsgObjectAdded && note->messageID != kMIDIMsgPropertyChanged &&
        note->messageID != kMIDIMsgIOError) return;
    pthread_mutex_lock(&mutex);
    FbvMonitor *m = lookup((uintptr_t)context);
    if (m) { m->state = FBV_MONITOR_DISCONNECTED; clear(m); }
    pthread_mutex_unlock(&mutex);
}

static bool named(MIDIObjectRef object, CFStringRef expected) {
    CFStringRef value = NULL;
    bool match = MIDIObjectGetStringProperty(object, kMIDIPropertyName, &value) == noErr &&
                 value && CFEqual(value, expected);
    if (value) CFRelease(value);
    return match;
}

static bool sources(MIDIEndpointRef endpoints[3]) {
    MIDIDeviceRef device = 0;
    unsigned matches = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfDevices(); ++i) {
        MIDIDeviceRef candidate = MIDIGetDevice(i);
        if (named(candidate, CFSTR("FBV Express Mk II"))) { device = candidate; ++matches; }
    }
    SInt32 offline = 1;
    if (matches != 1 || MIDIObjectGetIntegerProperty(device, kMIDIPropertyOffline, &offline) != noErr || offline)
        return false;
    unsigned counts[3] = {0};
    CFStringRef names[] = {CFSTR("Port 1"), CFSTR("Port 2"), CFSTR("Port 3")};
    for (ItemCount e = 0; e < MIDIDeviceGetNumberOfEntities(device); ++e) {
        MIDIEntityRef entity = MIDIDeviceGetEntity(device, e);
        for (ItemCount s = 0; s < MIDIEntityGetNumberOfSources(entity); ++s) {
            MIDIEndpointRef endpoint = MIDIEntityGetSource(entity, s);
            for (unsigned p = 0; p < 3; ++p) {
                if (named(endpoint, names[p])) { endpoints[p] = endpoint; ++counts[p]; }
            }
        }
    }
    return counts[0] == 1 && counts[1] == 1 && counts[2] == 1;
}

FbvMonitor *fbv_monitor_create(bool demo) {
    FbvMonitor *m = calloc(1, sizeof(*m));
    if (m) m->demo = demo;
    return m;
}

void fbv_monitor_stop(FbvMonitor *m) {
    if (!m) return;
    pthread_mutex_lock(&mutex);
    FbvMonitor **link = &registry;
    while (*link && *link != m) link = &(*link)->next;
    if (*link) *link = m->next;
    m->next = NULL;
    m->state = FBV_MONITOR_STOPPED;
    clear(m);
    pthread_mutex_unlock(&mutex);
    if (m->input) MIDIPortDispose(m->input);
    if (m->client) MIDIClientDispose(m->client);
    m->input = m->client = 0;
}

bool fbv_monitor_start(FbvMonitor *m) {
    if (!m) return false;
    fbv_monitor_stop(m);
    MIDIEndpointRef endpoints[3] = {0};
    if (!m->demo && !sources(endpoints)) {
        pthread_mutex_lock(&mutex);
        m->state = FBV_MONITOR_DISCONNECTED;
        pthread_mutex_unlock(&mutex);
        return false;
    }
    pthread_mutex_lock(&mutex);
    if (!next_token) { pthread_mutex_unlock(&mutex); return false; }
    m->token = next_token++;
    m->next = registry; registry = m;
    m->state = FBV_MONITOR_LISTENING;
    pthread_mutex_unlock(&mutex);
    if (m->demo) return true;
    void *context = (void *)m->token;
    bool okay = MIDIClientCreate(CFSTR("FBV input"), notification, context, &m->client) == noErr &&
                MIDIInputPortCreate(m->client, CFSTR("Test controls"), receive, context, &m->input) == noErr;
    for (unsigned p = 0; okay && p < 3; ++p)
        okay = MIDIPortConnectSource(m->input, endpoints[p], (void *)(uintptr_t)(p + 1)) == noErr;
    if (!okay) {
        fbv_monitor_stop(m);
        pthread_mutex_lock(&mutex);
        m->state = FBV_MONITOR_DISCONNECTED;
        pthread_mutex_unlock(&mutex);
    }
    return okay;
}

void fbv_monitor_destroy(FbvMonitor *m) { if (m) { fbv_monitor_stop(m); free(m); } }

FbvMonitorState fbv_monitor_drain(FbvMonitor *m, FbvCCEvent *events, size_t capacity,
                                 size_t *count, uint64_t *dropped) {
    if (count) *count = 0;
    if (dropped) *dropped = 0;
    if (!m) return FBV_MONITOR_STOPPED;
    pthread_mutex_lock(&mutex);
    size_t n = events ? (capacity < m->count ? capacity : m->count) : 0;
    for (size_t i = 0; i < n; ++i) events[i] = m->events[(m->head + i) % QUEUE_SIZE];
    m->head = (m->head + n) % QUEUE_SIZE; m->count -= n;
    if (count) *count = n;
    if (dropped) *dropped = m->dropped;
    FbvMonitorState state = m->state;
    pthread_mutex_unlock(&mutex);
    return state;
}

bool fbv_monitor_demo_feed(FbvMonitor *m, unsigned port, const uint8_t *bytes, size_t count) {
    if (!m || !m->demo || port < 1 || port > 3 || !bytes || count > 4096) return false;
    pthread_mutex_lock(&mutex);
    bool okay = m->state == FBV_MONITOR_LISTENING;
    if (okay) feed(m, port, bytes, count);
    pthread_mutex_unlock(&mutex);
    return okay;
}

bool fbv_monitor_demo_disconnect(FbvMonitor *m) {
    if (!m || !m->demo) return false;
    MIDINotification note = {kMIDIMsgObjectRemoved, sizeof(MIDINotification)};
    notification(&note, (void *)m->token);
    return true;
}
