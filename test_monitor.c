#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>

static ItemCount device_count = 1, source_count = 4;
static bool offline_device, duplicate_port, wrong_name;
static ItemCount mock_devices(void) { return device_count; }
static MIDIDeviceRef mock_device(ItemCount index) { return (MIDIDeviceRef)(10 + index); }
static ItemCount mock_entities(MIDIDeviceRef device) { (void)device; return 1; }
static MIDIEntityRef mock_entity(MIDIDeviceRef device, ItemCount index) { (void)device; (void)index; return 20; }
static ItemCount mock_sources(MIDIEntityRef entity) { (void)entity; return source_count; }
static MIDIEndpointRef mock_source(MIDIEntityRef entity, ItemCount index) { (void)entity; return (MIDIEndpointRef)(30 + index); }
static OSStatus mock_integer(MIDIObjectRef object, CFStringRef key, SInt32 *value) {
    (void)object; (void)key; *value = offline_device; return noErr;
}
static OSStatus mock_string(MIDIObjectRef object, CFStringRef key, CFStringRef *value) {
    (void)key;
    CFStringRef names[] = {CFSTR("Port 1"), CFSTR("Port 2"), CFSTR("Port 3"), CFSTR("Port 4")};
    *value = object < 20 ? (wrong_name ? CFSTR("Other FBV Express Mk II") : CFSTR("FBV Express Mk II")) :
             names[duplicate_port ? 0 : object - 30];
    CFRetain(*value); return noErr;
}
#define MIDIGetNumberOfDevices mock_devices
#define MIDIGetDevice mock_device
#define MIDIDeviceGetNumberOfEntities mock_entities
#define MIDIDeviceGetEntity mock_entity
#define MIDIEntityGetNumberOfSources mock_sources
#define MIDIEntityGetSource mock_source
#define MIDIObjectGetIntegerProperty mock_integer
#define MIDIObjectGetStringProperty mock_string
#include "Sources/FBVCore/monitor.c"

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static FbvCCEvent events[256];
static size_t drain(FbvMonitor *m, FbvMonitorState expected) {
    size_t count = 999;
    CHECK(fbv_monitor_drain(m, events, 256, &count, NULL) == expected);
    return count;
}
static void bytes(FbvMonitor *m, unsigned port, const uint8_t *data, size_t count) {
    CHECK(fbv_monitor_demo_feed(m, port, data, count));
}
#define FEED(m, port, ...) do { const uint8_t data[] = {__VA_ARGS__}; bytes(m, port, data, sizeof(data)); } while (0)

static void identity(void) {
    MIDIEndpointRef endpoints[3] = {0};
    CHECK(sources(endpoints));
    CHECK(endpoints[0] == 30 && endpoints[1] == 31 && endpoints[2] == 32);
    device_count = 2; CHECK(!sources(endpoints)); device_count = 0; CHECK(!sources(endpoints)); device_count = 1;
    offline_device = true; CHECK(!sources(endpoints)); offline_device = false;
    wrong_name = true; CHECK(!sources(endpoints)); wrong_name = false;
    duplicate_port = true; CHECK(!sources(endpoints)); duplicate_port = false;
    source_count = 2; CHECK(!sources(endpoints)); source_count = 4;
}

static void parser(void) {
    FbvMonitor *m = fbv_monitor_create(true);
    CHECK(m && fbv_monitor_start(m));
    FEED(m, 1, 0xb0, 0);
    FEED(m, 2, 127, 127);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    FEED(m, 1, 0xf8, 0, 127, 0xfa, 127);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 2);
    CHECK(events[0].cc == 0 && events[0].value == 0 && events[0].channel == 1 && events[0].port == 1);
    CHECK(events[1].cc == 127 && events[1].value == 127 && events[1].received >= events[0].received);
    FEED(m, 3, 0xbf, 95, 64);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 1);
    CHECK(events[0].channel == 16 && events[0].port == 3);
    FEED(m, 1, 0xf0, 0xb0, 20, 127, 0xf7, 20, 127);
    /* A channel status terminates malformed SysEx, as MIDI requires. */
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 1);
    FEED(m, 1, 0xb0, 20, 127, 0xf0, 1, 2, 0xf8, 3, 0xf7, 20, 127);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 1);
    for (int status = 0xf1; status <= 0xf7; ++status) {
        FEED(m, 1, 0xb0, 20, (uint8_t)status, 127, 20, 127);
        CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    }
    for (int status = 0x80; status < 0xf0; ++status) {
        FEED(m, 1, (uint8_t)status, 42, 1, 43, 2);
        size_t count = drain(m, FBV_MONITOR_LISTENING);
        CHECK(count == ((status & 0xf0) == 0xb0 ? 2u : 0u));
    }
    for (int value = 0; value < 128; ++value) {
        FEED(m, 1, 0xb0, (uint8_t)value, (uint8_t)value);
        CHECK(drain(m, FBV_MONITOR_LISTENING) == 1);
        CHECK(events[0].value == value && events[0].cc == value);
    }
    CHECK(!fbv_monitor_demo_feed(m, 0, (uint8_t[]){0}, 1));
    CHECK(!fbv_monitor_demo_feed(m, 4, (uint8_t[]){0}, 1));
    CHECK(!fbv_monitor_demo_feed(m, 1, NULL, 1));
    CHECK(!fbv_monitor_demo_feed(m, 1, (uint8_t[]){0}, 4097));
    for (unsigned i = 0; i < 300; ++i) FEED(m, 1, 0xb0, 20, (uint8_t)(i % 128));
    uint64_t dropped = 0; size_t count = 0;
    CHECK(fbv_monitor_drain(m, events, 1, &count, &dropped) == FBV_MONITOR_LISTENING);
    CHECK(count == 1 && dropped == 44 && events[0].value == 44);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 255);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    FEED(m, 1, 0xb0, 42);
    fbv_monitor_stop(m);
    CHECK(drain(m, FBV_MONITOR_STOPPED) == 0);
    CHECK(!fbv_monitor_demo_feed(m, 1, (uint8_t[]){127}, 1));
    CHECK(fbv_monitor_start(m)); FEED(m, 1, 127);
    CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    FEED(m, 1, 0xb0, 20, 127);
    CHECK(fbv_monitor_demo_disconnect(m));
    CHECK(drain(m, FBV_MONITOR_DISCONNECTED) == 0);
    CHECK(fbv_monitor_start(m)); CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    fbv_monitor_destroy(m);
    FbvMonitor *real = fbv_monitor_create(false);
    CHECK(!fbv_monitor_demo_feed(real, 1, (uint8_t[]){0xb0, 20, 127}, 3));
    CHECK(!fbv_monitor_demo_disconnect(real));
    fbv_monitor_destroy(real);
    CHECK(!fbv_monitor_start(NULL));
    CHECK(drain(NULL, FBV_MONITOR_STOPPED) == 0);
    fbv_monitor_stop(NULL); fbv_monitor_destroy(NULL);
}

static void *late_callbacks(void *context) {
    MIDIPacketList packets = {0};
    packets.numPackets = 1; packets.packet[0].length = 3;
    memcpy(packets.packet[0].data, (uint8_t[]){0xb0, 20, 127}, 3);
    for (unsigned i = 0; i < 20000; ++i) receive(&packets, context, (void *)(uintptr_t)1);
    MIDINotification note = {kMIDIMsgSetupChanged, sizeof(MIDINotification)};
    notification(&note, context);
    return NULL;
}
static void lifecycle(void) {
    FbvMonitor *m = fbv_monitor_create(true);
    CHECK(fbv_monitor_start(m));
    uintptr_t old_token = m->token;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, late_callbacks, (void *)old_token) == 0);
    for (int i = 0; i < 500; ++i) {
        fbv_monitor_stop(m); CHECK(fbv_monitor_start(m));
        CHECK(drain(m, FBV_MONITOR_LISTENING) == 0);
    }
    fbv_monitor_destroy(m);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(lookup(old_token) == NULL);
}
int main(void) {
    identity(); parser(); lifecycle();
    printf("%u offline monitor checks passed. No CoreMIDI clients opened.\n", checks);
    return 0;
}
