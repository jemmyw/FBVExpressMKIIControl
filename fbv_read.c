#include <CoreMIDI/CoreMIDI.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DUMP_SIZE 81
#define TIMEOUT_SECONDS 3

static const Byte read_request[] = {0xf0, 0x00, 0x01, 0x0c, 0x11, 0x01, 0x00, 0xf7};
static const Byte reply_prefix[] = {0xf0, 0x00, 0x01, 0x0c, 0x11, 0x01, 0x01};
static const Byte known_a[] = {2, 3, 1, 1, 1, 0, 18, 127, 0, 0};
static const Byte captured[] = {
    0xf0, 0x00, 0x01, 0x0c, 0x11, 0x01, 0x01, 0x04, 0x08,
    0x02, 0x03, 0x01, 0x01, 0x01, 0x00, 0x12, 0x7f, 0x00, 0x00,
    0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x15, 0x7f, 0x00, 0x00,
    0x04, 0x01, 0x02, 0x01, 0x01, 0x00, 0x16, 0x7f, 0x00, 0x00,
    0x05, 0x01, 0x03, 0x01, 0x01, 0x00, 0x17, 0x7f, 0x00, 0x00,
    0x0d, 0x01, 0x00, 0x01, 0x04, 0x00, 0x5f, 0x7f, 0x00, 0x00,
    0x0e, 0x01, 0x00, 0x01, 0x04, 0x00, 0x5f, 0x7f, 0x00, 0x00,
    0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x73, 0xf7
};

typedef struct {
    Byte assembling[DUMP_SIZE], reply[DUMP_SIZE];
    size_t used;
    bool active, ready, expect_ack, ack_ready, ack_failed;
    unsigned rejected;
} Parser;

typedef struct {
    pthread_mutex_t mutex;
    Parser parser;
    bool armed;
} Reader;

static unsigned checksum(const Byte *bytes) {
    unsigned sum = 0;
    for (size_t i = 7; i < DUMP_SIZE - 2; ++i) sum += bytes[i];
    return sum & 127;
}

static bool valid_reply(const Byte *bytes, size_t size) {
    if (size != DUMP_SIZE || memcmp(bytes, reply_prefix, sizeof(reply_prefix)) != 0 ||
        bytes[size - 1] != 0xf7) return false;
    for (size_t i = 1; i < size - 1; ++i)
        if (bytes[i] & 0x80) return false;
    return bytes[size - 2] == checksum(bytes);
}

static void feed(Parser *parser, const Byte *bytes, size_t size) {
    for (size_t i = 0; i < size && !parser->ready &&
         !parser->ack_ready && !parser->ack_failed; ++i) {
        Byte byte = bytes[i];
        if (byte >= 0xf8) continue;
        if (byte == 0xf0) {
            if (parser->active) ++parser->rejected;
            parser->active = true;
            parser->used = 0;
        } else if (!parser->active) {
            continue;
        } else if (byte & 0x80 && byte != 0xf7) {
            parser->active = false;
            parser->used = 0;
            ++parser->rejected;
            continue;
        }
        if (parser->used == sizeof(parser->assembling)) {
            parser->active = false;
            parser->used = 0;
            ++parser->rejected;
            continue;
        }
        parser->assembling[parser->used++] = byte;
        if (byte == 0xf7) {
            static const Byte ack_prefix[] = {0xf0, 0, 1, 0x0c, 0x11, 1, 7};
            if (parser->expect_ack && parser->used == 9 &&
                memcmp(parser->assembling, ack_prefix, sizeof(ack_prefix)) == 0) {
                parser->ack_ready = parser->assembling[7] == 0;
                parser->ack_failed = !parser->ack_ready;
            } else if (!parser->expect_ack && valid_reply(parser->assembling, parser->used)) {
                memcpy(parser->reply, parser->assembling, DUMP_SIZE);
                parser->ready = true;
            } else {
                ++parser->rejected;
            }
            parser->active = false;
            parser->used = 0;
        }
    }
}

static void receive(const MIDIPacketList *list, void *context, void *connection) {
    Reader *reader = context;
    if (connection != reader) return;
    pthread_mutex_lock(&reader->mutex);
    if (reader->armed && list->numPackets <= 4096) {
        size_t total = 0;
        const MIDIPacket *packet = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            total += packet->length;
            if (total > 1024 * 1024) break;
            feed(&reader->parser, packet->data, packet->length);
            packet = MIDIPacketNext(packet);
        }
    }
    pthread_mutex_unlock(&reader->mutex);
}

static bool property(MIDIObjectRef object, CFStringRef key, char *text, size_t size) {
    CFStringRef value = NULL;
    text[0] = '\0';
    if (MIDIObjectGetStringProperty(object, key, &value) != noErr || !value) return false;
    bool ok = CFStringGetCString(value, text, (CFIndex)size, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}

static MIDIDeviceRef device_of(MIDIEndpointRef endpoint) {
    MIDIEntityRef entity = 0;
    MIDIDeviceRef device = 0;
    if (MIDIEndpointGetEntity(endpoint, &entity) != noErr || !entity ||
        MIDIEntityGetDevice(entity, &device) != noErr) return 0;
    return device;
}

static bool is_fbv(MIDIDeviceRef device) {
    char name[256];
    return device && property(device, kMIDIPropertyName, name, sizeof(name)) &&
           strcmp(name, "FBV Express Mk II") == 0;
}

static bool resolve(MIDIEndpointRef *source, MIDIEndpointRef *destination) {
    unsigned sources = 0, destinations = 0;
    MIDIDeviceRef source_device = 0, destination_device = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        MIDIEndpointRef endpoint = MIDIGetSource(i);
        MIDIDeviceRef device = device_of(endpoint);
        char name[256];
        if (is_fbv(device) && property(endpoint, kMIDIPropertyName, name, sizeof(name)) &&
            strcmp(name, "Port 4") == 0) {
            *source = endpoint;
            source_device = device;
            ++sources;
        }
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        MIDIEndpointRef endpoint = MIDIGetDestination(i);
        MIDIDeviceRef device = device_of(endpoint);
        char name[256], display[256], device_name[256];
        property(endpoint, kMIDIPropertyName, name, sizeof(name));
        property(endpoint, kMIDIPropertyDisplayName, display, sizeof(display));
        property(device, kMIDIPropertyName, device_name, sizeof(device_name));
        fprintf(stderr, "Destination %lu: device=\"%s\" name=\"%s\" display=\"%s\"\n",
                (unsigned long)i, device_name, name, display);
        if (is_fbv(device)) {
            *destination = endpoint;
            destination_device = device;
            ++destinations;
        }
    }
    if (sources != 1 || destinations != 1 || source_device != destination_device) {
        fprintf(stderr, "Need exactly one FBV Express Mk II Port 4 source and one destination "
                "on the same device; found %u source(s), %u destination(s). Nothing sent.\n",
                sources, destinations);
        return false;
    }
    return true;
}

static void hex(const Byte *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) printf("%02x", bytes[i]);
}

enum { SWITCH_COUNT = 4, CONTROL_COUNT = 6 };
static const char *const pedal_selectors[] = {"A", "B", "C", "D", "wah", "volume"};
static const char *const control_names[] = {"A", "B", "C", "D", "Wah", "Volume"};

static int parse_pedal(const char *text) {
    for (int i = 0; i < CONTROL_COUNT; ++i)
        if (strcmp(text, pedal_selectors[i]) == 0) return i;
    return -1;
}

static int control_index(Byte id) {
    if (id >= 2 && id <= 5) return id - 2;
    if (id == 13 || id == 14) return id - 9;
    return -1;
}

static const char *mode_name(Byte mode) {
    switch (mode) {
        case 1: return "Single";
        case 2: return "Toggle";
        case 3: return "Momentary";
        case 4: return "Continuous";
        default: return "unknown";
    }
}

static bool cc_record(const Byte *record) {
    if (record[1] < 1 || record[1] > 3 || record[2] >= 16 || record[3] != 1 ||
        record[5] != 0 || record[7] != 127 || record[9] != 0) return false;
    int control = control_index(record[0]);
    if (control < 0) return false;
    if (control >= SWITCH_COUNT) return record[4] == 4 && record[8] == 0;
    return record[4] >= 1 && record[4] <= 3 &&
           record[8] == (record[4] == 1 ? 0 : record[6]);
}

static void print_reply(const Byte *reply, bool json) {
    if (json) {
        printf("{\"raw_hex\":\"");
        hex(reply, DUMP_SIZE);
        printf("\",\"length\":81,\"checksum\":{\"received\":%u,\"calculated\":%u,\"valid\":true},"
               "\"header_raw_hex\":\"%02x%02x\",\"records\":[",
               reply[79], checksum(reply), reply[7], reply[8]);
    } else {
        printf("Raw: ");
        hex(reply, DUMP_SIZE);
        printf("\nChecksum: %02x (valid); header raw: %02x %02x\n",
               reply[79], reply[7], reply[8]);
    }
    for (size_t i = 0; i < 7; ++i) {
        const Byte *record = reply + 9 + i * 10;
        if (json) printf("%s{\"id\":%u,\"raw_hex\":\"", i ? "," : "", record[0]);
        else printf("Record %02x raw: ", record[0]);
        hex(record, 10);
        if (json) printf("\"");
        int control = control_index(record[0]);
        if (control >= 0) {
            const char *label = control_names[control];
            if (json) printf(",\"control\":\"%s\"", label);
            else printf(" [%s", label);
            if (cc_record(record)) {
                if (json) {
                    printf(",\"port_raw\":%u,\"channel_zero_based\":%u,\"port\":%u,"
                           "\"channel\":%u,\"mode_raw\":%u,\"mode\":\"%s\","
                           "\"confirmed_command\":\"CC\",\"controller\":%u",
                           record[1], record[2], record[1], record[2] + 1,
                           record[4], mode_name(record[4]), record[6]);
                } else {
                    printf(" port=%u channel=%u mode=%s CC%u", record[1], record[2] + 1,
                           mode_name(record[4]), record[6]);
                }
            }
            if (!json) putchar(']');
        }
        printf(json ? "}" : "\n");
    }
    if (json) printf("]}\n");
}

static int self_test(bool json) {
    unsigned checks = 0;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr, "Self-test failed at line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)
    CHECK(sizeof(captured) == DUMP_SIZE);
    CHECK(checksum(captured) == 0x73);
    CHECK(valid_reply(captured, sizeof(captured)));
    CHECK(memcmp(captured + 9, known_a, 10) == 0);
    for (size_t split = 0; split <= DUMP_SIZE; ++split) {
        Parser parser = {0};
        feed(&parser, captured, split);
        CHECK(parser.ready == (split == DUMP_SIZE));
        feed(&parser, captured + split, DUMP_SIZE - split);
        CHECK(parser.ready && memcmp(parser.reply, captured, DUMP_SIZE) == 0);
    }
    for (size_t size = 0; size < DUMP_SIZE; ++size) {
        Parser parser = {0};
        feed(&parser, captured, size);
        CHECK(!parser.ready);
    }
    Parser realtime = {0};
    for (size_t i = 0; i < DUMP_SIZE; ++i) {
        Byte clock = 0xf8;
        feed(&realtime, &clock, 1);
        feed(&realtime, captured + i, 1);
    }
    CHECK(realtime.ready);
    for (size_t offset = 0; offset < DUMP_SIZE; ++offset) {
        Byte corrupt[DUMP_SIZE];
        memcpy(corrupt, captured, DUMP_SIZE);
        corrupt[offset] ^= 1;
        Parser parser = {0};
        feed(&parser, corrupt, sizeof(corrupt));
        CHECK(!parser.ready);
        feed(&parser, captured, DUMP_SIZE);
        CHECK(parser.ready);
    }
    const Byte unrelated[] = {0xf0, 0x7e, 0x7f, 6, 2, 0, 1, 0x0c, 0x11, 0, 1, 0,
                              0x20, 0x31, 0x31, 0x30, 0xf7,
                              0xf0, 0, 1, 0x0c, 0x11, 1, 7, 0, 0xf7};
    Parser parser = {0};
    feed(&parser, unrelated, sizeof(unrelated));
    CHECK(!parser.ready);
    feed(&parser, captured, 20);
    Byte bad_status = 0x90;
    feed(&parser, &bad_status, 1);
    feed(&parser, captured + 20, DUMP_SIZE - 20);
    CHECK(!parser.ready);
    Byte oversized[256] = {0xf0};
    feed(&parser, oversized, sizeof(oversized));
    CHECK(!parser.ready && !parser.active);
    feed(&parser, captured, DUMP_SIZE);
    CHECK(parser.ready);
    Parser independent = {0};
    feed(&independent, captured + 20, DUMP_SIZE - 20);
    CHECK(!independent.ready);
    for (size_t offset = 1; offset <= 6; ++offset) {
        Byte other[DUMP_SIZE];
        memcpy(other, captured, DUMP_SIZE);
        other[offset] ^= 1;
        other[79] = (Byte)checksum(other);
        CHECK(!valid_reply(other, DUMP_SIZE));
    }
    Reader reader = {.mutex = PTHREAD_MUTEX_INITIALIZER, .armed = true};
    MIDIPacketList packets;
    MIDIPacket *packet = MIDIPacketListInit(&packets);
    packet = MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, 12, captured);
    CHECK(packet != NULL);
    receive(&packets, &reader, NULL);
    CHECK(!reader.parser.active);
    receive(&packets, &reader, &reader);
    CHECK(reader.parser.active && !reader.parser.ready);
    packet = MIDIPacketListInit(&packets);
    packet = MIDIPacketListAdd(&packets, sizeof(packets), packet, 0,
                               DUMP_SIZE - 12, captured + 12);
    CHECK(packet != NULL);
    receive(&packets, &reader, &reader);
    CHECK(reader.parser.ready);
    pthread_mutex_destroy(&reader.mutex);
    fprintf(json ? stderr : stdout, "Offline self-test: %u checks passed (captured dump, "
            "fragmentation, realtime, truncation, corruption, unrelated traffic, overflow, "
            "source isolation, recovery).\n", checks);
    if (json) print_reply(captured, true);
    return 0;
#undef CHECK
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

typedef struct {
    MIDIClientRef client;
    MIDIPortRef input, output;
    MIDIEndpointRef source, destination;
    bool connected;
    Reader reader;
} Channel;

static bool midi_ok(OSStatus status, const char *operation) {
    if (status == noErr) return true;
    fprintf(stderr, "%s failed: %d\n", operation, (int)status);
    return false;
}

static bool open_channel(Channel *channel) {
    if (!midi_ok(MIDIClientCreate(CFSTR("FBV configuration CLI"), NULL, NULL,
                                  &channel->client), "MIDIClientCreate")) return false;
    if (!resolve(&channel->source, &channel->destination)) return false;
    if (!midi_ok(MIDIInputPortCreate(channel->client, CFSTR("FBV reply"), receive,
                                     &channel->reader, &channel->input),
                 "MIDIInputPortCreate")) return false;
    if (!midi_ok(MIDIOutputPortCreate(channel->client, CFSTR("FBV requests"),
                                      &channel->output), "MIDIOutputPortCreate")) return false;
    if (!midi_ok(MIDIPortConnectSource(channel->input, channel->source, &channel->reader),
                 "MIDIPortConnectSource")) return false;
    channel->connected = true;
    return true;
}

static void close_channel(Channel *channel) {
    pthread_mutex_lock(&channel->reader.mutex);
    channel->reader.armed = false;
    pthread_mutex_unlock(&channel->reader.mutex);
    if (channel->connected) MIDIPortDisconnectSource(channel->input, channel->source);
    if (channel->input) MIDIPortDispose(channel->input);
    if (channel->output) MIDIPortDispose(channel->output);
    if (channel->client) MIDIClientDispose(channel->client);
    pthread_mutex_destroy(&channel->reader.mutex);
}

static void arm(Channel *channel, bool ack) {
    pthread_mutex_lock(&channel->reader.mutex);
    channel->reader.parser = (Parser){.expect_ack = ack};
    channel->reader.armed = true;
    pthread_mutex_unlock(&channel->reader.mutex);
}

static bool wait_reply(Channel *channel, bool ack, Byte *reply) {
    double deadline = monotonic_seconds() + TIMEOUT_SECONDS;
    while (monotonic_seconds() < deadline) {
        pthread_mutex_lock(&channel->reader.mutex);
        Parser *parser = &channel->reader.parser;
        bool ready = ack ? parser->ack_ready : parser->ready;
        bool failed = parser->ack_failed;
        if (ready && !ack) memcpy(reply, parser->reply, DUMP_SIZE);
        pthread_mutex_unlock(&channel->reader.mutex);
        if (ready) return true;
        if (failed) {
            fprintf(stderr, "Device returned a nonzero assignment acknowledgement.\n");
            return false;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    fprintf(stderr, "Timed out after %d seconds waiting for %s on Port 4; no retry.\n",
            TIMEOUT_SECONDS, ack ? "assignment acknowledgement" : "valid configuration reply");
    return false;
}

static bool send_packet(Channel *channel, const Byte *bytes, size_t size) {
    MIDIPacketList packets;
    MIDIPacket *packet = MIDIPacketListInit(&packets);
    if (!MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, size, bytes)) {
        fprintf(stderr, "Could not construct MIDI packet; nothing sent.\n");
        return false;
    }
    return midi_ok(MIDISend(channel->output, channel->destination, &packets), "MIDISend");
}

static bool read_dump(void *context, Byte *reply) {
    Channel *channel = context;
    arm(channel, false);
    return send_packet(channel, read_request, sizeof(read_request)) &&
           wait_reply(channel, false, reply);
}

static int query(bool json) {
    Channel channel = {.reader.mutex = PTHREAD_MUTEX_INITIALIZER};
    Byte reply[DUMP_SIZE];
    bool ok = open_channel(&channel) && read_dump(&channel, reply);
    close_channel(&channel);
    if (ok) print_reply(reply, json);
    return ok ? 0 : 1;
}

#define ASSIGN_SIZE 21
#define BACKUP_PATH_SIZE 128

typedef struct {
    int cc, mode, port, channel;
    int pedal;
    bool apply, json;
} Change;

typedef struct {
    bool (*read)(void *, Byte *);
    bool (*backup)(void *, const Byte *, char *);
    bool (*assign)(void *, const Byte *);
    bool (*ack)(void *);
    void *context;
} Operations;

typedef struct {
    Byte original[DUMP_SIZE], expected[DUMP_SIZE], assignment[ASSIGN_SIZE];
    char backup_path[BACKUP_PATH_SIZE];
    const char *status;
} Proposal;

static bool eligible(const Byte *dump, int pedal) {
    static const Byte ids[] = {2, 3, 4, 5, 13, 14, 16};
    if (pedal < 0 || pedal >= CONTROL_COUNT || !valid_reply(dump, DUMP_SIZE) ||
        dump[7] != 4 || dump[8] != 8) return false;
    for (size_t i = 0; i < sizeof(ids); ++i)
        if (dump[9 + i * 10] != ids[i]) return false;
    return cc_record(dump + 9 + 10 * pedal);
}

static bool build_proposal(const Byte *original, Change change, Proposal *proposal) {
    if (change.cc < -1 || change.cc > 127 || change.mode < -1 || change.mode > 3 ||
        change.mode == 0 || change.port < -1 || change.port == 0 || change.port > 3 ||
        change.channel < -1 || change.channel == 0 || change.channel > 16 ||
        (change.pedal >= SWITCH_COUNT && change.mode != -1) ||
        !eligible(original, change.pedal)) return false;
    memcpy(proposal->original, original, DUMP_SIZE);
    memcpy(proposal->expected, original, DUMP_SIZE);
    Byte *record = proposal->expected + 9 + 10 * change.pedal;
    if (change.cc != -1) record[6] = (Byte)change.cc;
    if (change.mode != -1) record[4] = (Byte)change.mode;
    if (change.port != -1) record[1] = (Byte)change.port;
    if (change.channel != -1) record[2] = (Byte)(change.channel - 1);
    if (change.pedal < SWITCH_COUNT) record[8] = record[4] == 1 ? 0 : record[6];
    proposal->expected[79] = (Byte)checksum(proposal->expected);
    const Byte header[] = {0xf0, 0, 1, 0x0c, 0x11, 1, 6, 0, 0x0c};
    memcpy(proposal->assignment, header, sizeof(header));
    /* The app's dump-to-internal and internal-to-write permutations cancel. */
    memcpy(proposal->assignment + 9, record, 10);
    unsigned sum = 0;
    for (size_t i = 7; i < ASSIGN_SIZE - 2; ++i) sum += proposal->assignment[i];
    proposal->assignment[19] = (Byte)(sum & 127);
    proposal->assignment[20] = 0xf7;
    return true;
}

static bool backup_to(const char *path, const Byte *dump) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "Cannot create backup %s: %s; no assignment sent.\n", path, strerror(errno));
        return false;
    }
    size_t written = 0;
    bool ok = true;
    while (written < DUMP_SIZE) {
        ssize_t count = write(fd, dump + written, DUMP_SIZE - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        written += (size_t)count;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) fprintf(stderr, "Backup %s write/fsync/close failed; retained file may be partial; "
                     "no assignment sent.\n", path);
    return ok;
}

static bool save_backup(void *context, const Byte *dump, char *path) {
    (void)context;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    snprintf(path, BACKUP_PATH_SIZE, "fbv-backup-%lld-%09ld-%d.syx",
             (long long)now.tv_sec, now.tv_nsec, getpid());
    bool ok = backup_to(path, dump);
    if (ok) fprintf(stderr, "Saved original raw configuration: %s\n", path);
    return ok;
}

static bool assign_pedal(void *context, const Byte *assignment) {
    Channel *channel = context;
    arm(channel, true);
    return send_packet(channel, assignment, ASSIGN_SIZE);
}

static bool assignment_ack(void *context) {
    return wait_reply(context, true, NULL);
}

static int transact(Change change, Operations operations, Proposal *proposal) {
    Byte original[DUMP_SIZE], current[DUMP_SIZE], after[DUMP_SIZE];
    proposal->status = "error";
    if (!operations.read(operations.context, original)) return 1;
    if (!build_proposal(original, change, proposal)) {
        fprintf(stderr, "Unsupported selected pedal CC shape, layout or requested values; "
                "no assignment sent.\n");
        return 1;
    }
    if (memcmp(original, proposal->expected, DUMP_SIZE) == 0) {
        proposal->status = "no-op";
        return 0;
    }
    if (!change.apply) {
        proposal->status = "preview";
        return 0;
    }
    if (!operations.backup(operations.context, original, proposal->backup_path)) return 1;
    if (!operations.read(operations.context, current)) return 1;
    if (memcmp(original, current, DUMP_SIZE) != 0) {
        fprintf(stderr, "Configuration changed since initial read; no assignment sent. Backup: %s\n",
                proposal->backup_path);
        return 1;
    }
    bool sent = operations.assign(operations.context, proposal->assignment);
    bool ack = sent && operations.ack(operations.context);
    bool readback = operations.read(operations.context, after);
    if (!sent || !ack || !readback || !valid_reply(after, DUMP_SIZE) ||
        memcmp(after, proposal->expected, DUMP_SIZE) != 0) {
        proposal->status = "uncertain";
        fprintf(stderr, "UNCERTAIN: assignment attempted but acknowledgement/full readback "
                "not verified. No retry or rollback. Backup: %s\n", proposal->backup_path);
        return 1;
    }
    proposal->status = "applied";
    return 0;
}

static void print_proposal(const Proposal *proposal, Change change) {
    const Byte *record = proposal->expected + 9 + 10 * change.pedal;
    const char *label = control_names[change.pedal];
    if (change.json) {
        printf("{\"status\":\"%s\",\"pedal\":\"%s\",\"record_id\":%u,"
               "\"apply_requested\":%s,\"original_raw_hex\":\"",
               proposal->status, pedal_selectors[change.pedal], record[0],
               change.apply ? "true" : "false");
        hex(proposal->original, DUMP_SIZE);
        printf("\",\"expected_raw_hex\":\"");
        hex(proposal->expected, DUMP_SIZE);
        printf("\",\"assignment_sysex_hex\":\"");
        hex(proposal->assignment, ASSIGN_SIZE);
        printf("\",\"backup\":");
        if (proposal->backup_path[0]) printf("\"%s\"", proposal->backup_path);
        else printf("null");
        printf("}\n");
    } else {
        printf("%s: %s CC%u %s, port %u/channel %u\n", proposal->status, label,
               record[6], mode_name(record[4]), record[1], record[2] + 1);
        printf("Proposed assignment (not sent for preview/no-op): ");
        hex(proposal->assignment, ASSIGN_SIZE);
        printf("\n");
        if (proposal->backup_path[0]) printf("Original raw backup: %s\n", proposal->backup_path);
    }
}

static int change_pedal(Change change) {
    Channel channel = {.reader.mutex = PTHREAD_MUTEX_INITIALIZER};
    Proposal proposal = {0};
    int result = 1;
    if (open_channel(&channel)) {
        Operations operations = {read_dump, save_backup, assign_pedal, assignment_ack, &channel};
        result = transact(change, operations, &proposal);
    }
    close_channel(&channel);
    if (proposal.status && strcmp(proposal.status, "error") != 0)
        print_proposal(&proposal, change);
    return result;
}

static bool parse_number(const char *text, unsigned minimum, unsigned maximum, int *number) {
    if (!text[0]) return false;
    unsigned value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + (unsigned)(*p - '0');
        if (value > maximum) return false;
    }
    if (value < minimum) return false;
    *number = (int)value;
    return true;
}

int main(int argc, char **argv) {
    bool json = false, help = false, test = false, pedal_seen = false;
    Change change = {.cc = -1, .mode = -1, .port = -1, .channel = -1, .pedal = 0};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) json = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) help = true;
        else if (strcmp(argv[i], "--self-test") == 0) test = true;
        else if (strcmp(argv[i], "--apply") == 0) change.apply = true;
        else if (strcmp(argv[i], "--pedal") == 0) {
            if (pedal_seen || ++i == argc || (change.pedal = parse_pedal(argv[i])) < 0) {
                fprintf(stderr, "--pedal requires A|B|C|D|wah|volume, specified once.\n");
                return 2;
            }
            pedal_seen = true;
        } else if (strcmp(argv[i], "--cc") == 0 || strcmp(argv[i], "--port") == 0 ||
                 strcmp(argv[i], "--channel") == 0) {
            const char *option = argv[i];
            bool cc = strcmp(option, "--cc") == 0;
            bool port = strcmp(option, "--port") == 0;
            int *value = cc ? &change.cc : port ? &change.port : &change.channel;
            unsigned minimum = cc ? 0 : 1;
            unsigned maximum = cc ? 127 : port ? 3 : 16;
            if (*value != -1 || ++i == argc ||
                !parse_number(argv[i], minimum, maximum, value)) {
                fprintf(stderr, "%s requires one decimal value %u..%u, specified once.\n",
                        option, minimum, maximum);
                return 2;
            }
        } else if (strcmp(argv[i], "--mode") == 0) {
            if (change.mode != -1 || ++i == argc) {
                fprintf(stderr, "--mode requires single|toggle|momentary, specified once.\n");
                return 2;
            }
            if (strcmp(argv[i], "single") == 0) change.mode = 1;
            else if (strcmp(argv[i], "toggle") == 0) change.mode = 2;
            else if (strcmp(argv[i], "momentary") == 0) change.mode = 3;
            else {
                fprintf(stderr, "Unknown mode: %s; use single|toggle|momentary.\n", argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\nUse %s --help for usage.\n",
                    argv[i], argv[0]);
            return 2;
        }
    }
    if (change.pedal >= SWITCH_COUNT && change.mode != -1) {
        fprintf(stderr, "--mode is only supported for A|B|C|D; wah/volume stay Continuous.\n");
        return 2;
    }
    if (help) {
        printf("Usage: %s [--pedal A|B|C|D|wah|volume] [--json]\n"
               "       [--port 1..3] [--channel 1..16] [--cc 0..127]\n"
               "       [--mode single|toggle|momentary (A-D only)] [--apply]\n"
               "Without changes: read FBV configuration. With changes: PREVIEW by default.\n"
               "--pedal      Select one control, default A. Alone, this only reads.\n"
               "--apply      Explicitly enable one backed-up, drift-checked selected assignment.\n"
               "--json       Structured read/proposal output.\n"
               "--self-test  Offline parser checks only; may combine with --json.\n"
               "--help, -h   Show help without accessing MIDI.\n"
               "A-D live-write verified; expression offline-tested only. No unattended changes.\n", argv[0]);
        return 0;
    }
    bool changing = change.cc != -1 || change.mode != -1 ||
                    change.port != -1 || change.channel != -1;
    if ((change.apply && !changing) || (test && (changing || change.apply || pedal_seen))) {
        fprintf(stderr, "--apply requires --cc, --mode, --port or --channel; "
                "--self-test cannot include changes or a pedal selector.\n");
        return 2;
    }
    if (test) return self_test(json);
    change.json = json;
    return changing ? change_pedal(change) : query(json);
}
