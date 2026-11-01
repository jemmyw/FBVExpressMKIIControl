#include <CoreMIDI/CoreMIDI.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static MIDIReadProc callback;
static void *callback_token, *source_token;
static unsigned step, fail_step, clients, ports;
static MIDIPacketList packets;
static bool failing(void) { return ++step == fail_step; }
static OSStatus mock_client(CFStringRef name, MIDINotifyProc proc, void *context, MIDIClientRef *client) {
    (void)name; (void)proc; (void)context;
    if (failing()) return -1;
    *client = 1; ++clients; return noErr;
}
static OSStatus mock_input(MIDIClientRef client, CFStringRef name, MIDIReadProc proc, void *context, MIDIPortRef *port) {
    (void)client; (void)name;
    if (failing()) return -1;
    callback = proc; callback_token = context; *port = 2; ++ports; return noErr;
}
static OSStatus mock_output(MIDIClientRef client, CFStringRef name, MIDIPortRef *port) {
    (void)client; (void)name;
    if (failing()) return -1;
    *port = 3; ++ports; return noErr;
}
static OSStatus mock_connect(MIDIPortRef port, MIDIEndpointRef source, void *context) {
    (void)port; (void)source; source_token = context;
    return failing() ? -1 : noErr;
}
static OSStatus mock_disconnect(MIDIPortRef port, MIDIEndpointRef source) {
    (void)port; (void)source;
    callback(&packets, callback_token, source_token); return noErr;
}
static OSStatus mock_port_dispose(MIDIPortRef port) {
    (void)port; --ports;
    callback(&packets, callback_token, callback_token); return noErr;
}
static OSStatus mock_client_dispose(MIDIClientRef client) { (void)client; --clients; return noErr; }
static ItemCount mock_count(void) { return fail_step == 5 ? 0 : 1; }
static MIDIEndpointRef mock_endpoint(ItemCount index) { (void)index; return 4; }
static OSStatus mock_entity(MIDIEndpointRef endpoint, MIDIEntityRef *entity) { (void)endpoint; *entity = 5; return noErr; }
static OSStatus mock_device(MIDIEntityRef entity, MIDIDeviceRef *device) { (void)entity; *device = 6; return noErr; }
static OSStatus mock_property(MIDIObjectRef object, CFStringRef key, CFStringRef *value) {
    (void)key; *value = object == 6 ? CFSTR("FBV Express Mk II") : CFSTR("Port 4");
    CFRetain(*value); return noErr;
}

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t *pause_mutex, *closing_mutex;
static bool entered, proceed, closing;
static int observed_lock(pthread_mutex_t *mutex) {
    pthread_mutex_lock(&gate);
    if (mutex == pause_mutex) {
        entered = true; pthread_cond_broadcast(&changed);
        while (!proceed) pthread_cond_wait(&changed, &gate);
    }
    if (mutex == closing_mutex) { closing = true; pthread_cond_broadcast(&changed); }
    pthread_mutex_unlock(&gate);
    return pthread_mutex_lock(mutex);
}
#define MIDIClientCreate mock_client
#define MIDIInputPortCreate mock_input
#define MIDIOutputPortCreate mock_output
#define MIDIPortConnectSource mock_connect
#define MIDIPortDisconnectSource mock_disconnect
#define MIDIPortDispose mock_port_dispose
#define MIDIClientDispose mock_client_dispose
#define MIDIGetNumberOfSources mock_count
#define MIDIGetNumberOfDestinations mock_count
#define MIDIGetSource mock_endpoint
#define MIDIGetDestination mock_endpoint
#define MIDIEndpointGetEntity mock_entity
#define MIDIEntityGetDevice mock_device
#define MIDIObjectGetStringProperty mock_property
#define pthread_mutex_lock observed_lock
#include "Sources/FBVCore/engine.c"
#undef pthread_mutex_lock

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
static void *deliver(void *token) { callback(&packets, token, token); return NULL; }
static atomic_bool closed;
static void *close_thread(void *channel) { close_channel(channel); atomic_store(&closed, true); return NULL; }

int main(void) {
    MIDIPacket *packet = MIDIPacketListInit(&packets);
    CHECK(MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, DUMP_SIZE, captured));
    FbvSession session = {0};
    Channel *channel = &session.channel;
    CHECK(session_open(&session));
    arm(channel, false);
    void *old = callback_token;
    CHECK(old == source_token);
    callback(&packets, old, NULL);
    CHECK(!channel->reader.parser.ready);

    pause_mutex = &channel->reader.mutex;
    pthread_t delivery, teardown;
    CHECK(pthread_create(&delivery, NULL, deliver, old) == 0);
    pthread_mutex_lock(&gate);
    while (!entered) pthread_cond_wait(&changed, &gate);
    closing_mutex = &reader_registry_mutex;
    pthread_mutex_unlock(&gate);
    CHECK(pthread_create(&teardown, NULL, close_thread, channel) == 0);
    pthread_mutex_lock(&gate);
    while (!closing) pthread_cond_wait(&changed, &gate);
    CHECK(!atomic_load(&closed));
    proceed = true; pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    CHECK(pthread_join(delivery, NULL) == 0);
    CHECK(pthread_join(teardown, NULL) == 0);
    pause_mutex = closing_mutex = NULL;
    CHECK(atomic_load(&closed) && !reader_registry && !clients && !ports);
    close_channel(channel);
    callback(&packets, old, old);
    CHECK(session_open(&session));
    CHECK(callback_token != old);
    arm(channel, false);
    callback(&packets, old, old);
    CHECK(!channel->reader.parser.ready);
    callback(&packets, callback_token, source_token);
    CHECK(channel->reader.parser.ready);
    close_channel(channel);

    FbvSession *allocated = fbv_session_create(false, ".");
    CHECK(allocated && session_open(allocated));
    void *freed_token = callback_token;
    close_channel(&allocated->channel);
    fbv_session_destroy(allocated);
    callback(&packets, freed_token, freed_token);
    CHECK(!reader_registry);

    for (fail_step = 1; fail_step <= 5; ++fail_step) {
        step = 0;
        CHECK(!session_open(&session));
        close_channel(channel);
        CHECK(!reader_registry && !clients && !ports);
        callback(&packets, callback_token, callback_token);
    }
    fail_step = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(session_open(&session));
        arm(channel, false);
        CHECK(pthread_create(&delivery, NULL, deliver, callback_token) == 0);
        close_channel(channel);
        CHECK(pthread_join(delivery, NULL) == 0);
        callback(&packets, old, old);
        CHECK(!reader_registry && !clients && !ports);
    }
    next_reader_token = UINTPTR_MAX;
    CHECK(session_open(&session));
    CHECK(channel->reader.token == UINTPTR_MAX);
    close_channel(channel);
    CHECK(!session_open(&session));
    close_channel(channel);
    CHECK(!reader_registry && !clients && !ports);
    printf("Configuration callback lifecycle: %u checks passed, including 1000 teardown races; no hardware access.\n", checks);
    return 0;
}
