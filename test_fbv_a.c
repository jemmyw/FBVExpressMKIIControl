#include "Sources/FBVCore/engine.c"
#include <sys/stat.h>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } \
} while (0)

typedef struct {
    Byte original[DUMP_SIZE], expected[DUMP_SIZE], sent[ASSIGN_SIZE];
    char events[16];
    unsigned event_count, reads, fail_read;
    bool backup_fail, send_fail, ack_fail, drift, mismatch;
} Mock;

static void event(Mock *mock, char name) {
    CHECK(mock->event_count + 1 < sizeof(mock->events));
    mock->events[mock->event_count++] = name;
}

static bool mock_read(void *context, Byte *reply) {
    Mock *mock = context;
    event(mock, 'R');
    if (++mock->reads == mock->fail_read) return false;
    memcpy(reply, mock->reads == 3 ? mock->expected : mock->original, DUMP_SIZE);
    if ((mock->reads == 2 && mock->drift) || (mock->reads == 3 && mock->mismatch)) {
        reply[30] ^= 1;
        reply[79] = (Byte)checksum(reply);
    }
    return true;
}

static bool mock_backup(void *context, const Byte *dump, char *path) {
    Mock *mock = context;
    event(mock, 'B');
    CHECK(memcmp(dump, mock->original, DUMP_SIZE) == 0);
    snprintf(path, BACKUP_PATH_SIZE, "offline-backup-not-created.syx");
    return !mock->backup_fail;
}

static bool mock_send(void *context, const Byte *assignment) {
    Mock *mock = context;
    event(mock, 'S');
    memcpy(mock->sent, assignment, ASSIGN_SIZE);
    return !mock->send_fail;
}

static bool mock_ack(void *context) {
    Mock *mock = context;
    event(mock, 'A');
    return !mock->ack_fail;
}

static Mock mock_for(Change change) {
    Mock mock = {0};
    Proposal proposal = {0};
    memcpy(mock.original, captured, DUMP_SIZE);
    CHECK(build_proposal(captured, change, &proposal));
    memcpy(mock.expected, proposal.expected, DUMP_SIZE);
    return mock;
}

static int run(Mock *mock, Change change, Proposal *proposal) {
    Operations operations = {mock_read, mock_backup, mock_send, mock_ack, mock};
    return transact_guarded(change, operations, proposal, NULL);
}

static void from_hex(const char *text, Byte *bytes, size_t count) {
    CHECK(strlen(text) == count * 2);
    for (size_t i = 0; i < count; ++i) {
        unsigned value;
        CHECK(sscanf(text + i * 2, "%2x", &value) == 1);
        bytes[i] = (Byte)value;
    }
}

static void port_channel_tests(void) {
    Byte current[DUMP_SIZE];
    memcpy(current, captured, DUMP_SIZE);
    current[13] = 3; current[15] = 19; current[17] = 19;
    current[79] = (Byte)checksum(current);
    Change change = {.cc = -1, .mode = -1, .port = -1, .channel = -1};
    Proposal proposal = {0};
    CHECK(build_proposal(current, change, &proposal));
    CHECK(memcmp(proposal.expected, current, DUMP_SIZE) == 0);
    for (int port = 1; port <= 3; ++port) {
        for (int channel = 1; channel <= 16; ++channel) {
            change.port = port; change.channel = channel;
            CHECK(build_proposal(current, change, &proposal));
            CHECK(proposal.assignment[10] == port && proposal.assignment[11] == channel - 1);
            CHECK(eligible(proposal.expected, 0));
            for (size_t i = 0; i < DUMP_SIZE; ++i)
                if (i != 10 && i != 11 && i != 79)
                    CHECK(proposal.expected[i] == current[i]);
        }
    }
    change.port = 1; change.channel = -1;
    CHECK(build_proposal(current, change, &proposal));
    CHECK(proposal.expected[10] == 1 && proposal.expected[11] == 1);
    for (size_t i = 0; i < DUMP_SIZE; ++i)
        if (i != 10 && i != 79) CHECK(proposal.expected[i] == current[i]);
    change.port = -1; change.channel = 1;
    CHECK(build_proposal(current, change, &proposal));
    CHECK(proposal.expected[10] == 3 && proposal.expected[11] == 0);
    for (size_t i = 0; i < DUMP_SIZE; ++i)
        if (i != 11 && i != 79) CHECK(proposal.expected[i] == current[i]);
    static const int invalid_ports[] = {-2, 0, 4};
    for (size_t i = 0; i < sizeof(invalid_ports) / sizeof(invalid_ports[0]); ++i) {
        change.port = invalid_ports[i];
        CHECK(!build_proposal(current, change, &proposal));
    }
    change.port = -1;
    static const int invalid_channels[] = {-2, 0, 17};
    for (size_t i = 0; i < sizeof(invalid_channels) / sizeof(invalid_channels[0]); ++i) {
        change.channel = invalid_channels[i];
        CHECK(!build_proposal(current, change, &proposal));
    }
    change = (Change){.cc = 20, .mode = 3, .port = 1, .channel = 1};
    CHECK(build_proposal(current, change, &proposal));
    Byte expected[ASSIGN_SIZE];
    from_hex("f000010c110106000c020100010300147f14003af7", expected, ASSIGN_SIZE);
    CHECK(memcmp(proposal.assignment, expected, ASSIGN_SIZE) == 0);
    for (size_t i = 0; i < DUMP_SIZE; ++i)
        if (i != 10 && i != 11 && i != 15 && i != 17 && i != 79)
            CHECK(proposal.expected[i] == current[i]);
    Mock mock = {0};
    memcpy(mock.original, current, DUMP_SIZE);
    memcpy(mock.expected, proposal.expected, DUMP_SIZE);
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "preview") == 0);
    change.apply = true;
    mock.event_count = mock.reads = 0;
    memset(mock.events, 0, sizeof(mock.events));
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "RBRSAR") == 0 && strcmp(proposal.status, "applied") == 0);
    CHECK(memcmp(mock.sent, expected, ASSIGN_SIZE) == 0);
    change = (Change){.cc = -1, .mode = -1, .port = 3, .channel = 2, .apply = true};
    mock = mock_for(change);
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "no-op") == 0);
}

static const char *current_hex =
    "f000010c1101010408020100010300147f1400030101010100157f0000"
    "040102010100167f0000050103010100177f00000d01000104005f7f0000"
    "0e01000104005f7f00001001000000000000000008f7";

static void pedal_tests(void) {
    static const char *messages[] = {
        "f000010c110106000c020204010300287f280067f7",
        "f000010c110106000c030204010300287f280068f7",
        "f000010c110106000c040204010300287f280069f7",
        "f000010c110106000c050204010300287f28006af7"
    };
    Byte current[DUMP_SIZE];
    from_hex(current_hex, current, DUMP_SIZE);
    CHECK(valid_reply(current, DUMP_SIZE));
    for (int pedal = 0; pedal < 4; ++pedal) {
        size_t offset = 9 + 10 * (size_t)pedal;
        Change change = {.cc = 40, .mode = 3, .port = 2, .channel = 5, .pedal = pedal};
        Proposal proposal = {0};
        CHECK(build_proposal(current, change, &proposal));
        Byte expected[ASSIGN_SIZE];
        from_hex(messages[pedal], expected, ASSIGN_SIZE);
        CHECK(memcmp(proposal.assignment, expected, ASSIGN_SIZE) == 0);
        CHECK(eligible(proposal.expected, pedal));
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 1 && i != offset + 2 && i != offset + 4 &&
                i != offset + 6 && i != offset + 8 && i != 79)
                CHECK(proposal.expected[i] == current[i]);
        Mock mock = {0};
        memcpy(mock.original, current, DUMP_SIZE);
        memcpy(mock.expected, proposal.expected, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "preview") == 0);
        change.apply = true;
        memset(mock.events, 0, sizeof(mock.events)); mock.event_count = mock.reads = 0;
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "RBRSAR") == 0 && strcmp(proposal.status, "applied") == 0);
        CHECK(memcmp(mock.sent, expected, ASSIGN_SIZE) == 0);
        for (int failure = 0; failure < 4; ++failure) {
            Mock failing = mock_for(change);
            if (failure == 0) failing.backup_fail = true;
            if (failure == 1) failing.drift = true;
            if (failure == 2) failing.ack_fail = true;
            if (failure == 3) failing.mismatch = true;
            CHECK(run(&failing, change, &proposal) == 1);
            const char *events[] = {"RB", "RBR", "RBRSAR", "RBRSAR"};
            CHECK(strcmp(failing.events, events[failure]) == 0);
            CHECK(strcmp(proposal.status, failure < 2 ? "error" : "uncertain") == 0);
        }
        change = (Change){.cc = -1, .mode = -1, .port = -1, .channel = -1,
                          .pedal = pedal, .apply = true};
        CHECK(build_proposal(current, change, &proposal));
        CHECK(memcmp(proposal.expected, current, DUMP_SIZE) == 0);
        mock = (Mock){0};
        memcpy(mock.original, current, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "no-op") == 0);
        change.mode = 3;
        CHECK(build_proposal(current, change, &proposal));
        CHECK(proposal.expected[offset + 6] == current[offset + 6]);
        CHECK(proposal.expected[offset + 8] == current[offset + 6]);
        CHECK(proposal.expected[offset + 1] == current[offset + 1]);
        CHECK(proposal.expected[offset + 2] == current[offset + 2]);
        Byte unsupported[DUMP_SIZE];
        memcpy(unsupported, current, DUMP_SIZE);
        unsupported[offset + 3] = 2;
        unsupported[79] = (Byte)checksum(unsupported);
        CHECK(!build_proposal(unsupported, change, &proposal));
        CHECK(!cc_record(unsupported + offset));
        mock = (Mock){0};
        memcpy(mock.original, unsupported, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 1 && strcmp(mock.events, "R") == 0);
        memcpy(unsupported, current, DUMP_SIZE);
        size_t other = 9 + 10 * (size_t)((pedal + 1) % 4);
        unsupported[other + 3] = 2;
        unsupported[79] = (Byte)checksum(unsupported);
        change.cc = 40;
        CHECK(build_proposal(unsupported, change, &proposal));
        CHECK(memcmp(unsupported + other, proposal.expected + other, 10) == 0);
        Mock non_cc = {0};
        memcpy(non_cc.original, unsupported, DUMP_SIZE);
        memcpy(non_cc.expected, proposal.expected, DUMP_SIZE);
        CHECK(run(&non_cc, change, &proposal) == 0);
        CHECK(strcmp(non_cc.events, "RBRSAR") == 0 && strcmp(proposal.status, "applied") == 0);
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 4 && i != offset + 6 && i != offset + 8 && i != 79)
                CHECK(proposal.expected[i] == unsupported[i]);
        Byte duplicate[DUMP_SIZE];
        memcpy(duplicate, current, DUMP_SIZE);
        duplicate[other] = current[offset];
        duplicate[79] = (Byte)checksum(duplicate);
        CHECK(!build_proposal(duplicate, change, &proposal));
    }
    Change invalid = {.cc = 40, .mode = -1, .port = -1, .channel = -1, .pedal = -1};
    Proposal proposal = {0};
    CHECK(!build_proposal(current, invalid, &proposal));
    invalid.pedal = CONTROL_COUNT;
    CHECK(!build_proposal(current, invalid, &proposal));
}

static const char *expression_hex =
    "f000010c1101010408020100010300147f1400030100010300157f1500"
    "040100010300167f1600050100010300177f17000d01000104005f7f0000"
    "0e01000104005f7f0000100100000000000000004af7";

static void expression_tests(void) {
    static const char *messages[] = {
        "f000010c110106000c0d02020104004a7f00006bf7",
        "f000010c110106000c0e02020104004a7f00006cf7"
    };
    Byte current[DUMP_SIZE];
    from_hex(expression_hex, current, DUMP_SIZE);
    CHECK(valid_reply(current, DUMP_SIZE));
    CHECK(parse_pedal("wah") == 4 && parse_pedal("volume") == 5);
    CHECK(parse_pedal("Wah") == -1 && parse_pedal("both") == -1);
    CHECK(strcmp(mode_name(4), "Continuous") == 0);
    for (int pedal = SWITCH_COUNT; pedal < CONTROL_COUNT; ++pedal) {
        size_t offset = 9 + 10 * (size_t)pedal;
        CHECK(control_index(current[offset]) == pedal && cc_record(current + offset));
        Change change = {.cc = 74, .mode = -1, .port = 2, .channel = 3, .pedal = pedal};
        Proposal proposal = {0};
        CHECK(build_proposal(current, change, &proposal));
        Byte expected[ASSIGN_SIZE];
        from_hex(messages[pedal - SWITCH_COUNT], expected, ASSIGN_SIZE);
        CHECK(memcmp(proposal.assignment, expected, ASSIGN_SIZE) == 0);
        CHECK(eligible(proposal.expected, pedal));
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 1 && i != offset + 2 && i != offset + 6 && i != 79)
                CHECK(proposal.expected[i] == current[i]);
        Mock mock = {0};
        memcpy(mock.original, current, DUMP_SIZE);
        memcpy(mock.expected, proposal.expected, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "preview") == 0);
        change.apply = true;
        memset(mock.events, 0, sizeof(mock.events)); mock.event_count = mock.reads = 0;
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "RBRSAR") == 0 && strcmp(proposal.status, "applied") == 0);
        CHECK(memcmp(mock.sent, expected, ASSIGN_SIZE) == 0);
        for (int failure = 0; failure < 4; ++failure) {
            Mock failing = mock_for(change);
            if (failure == 0) failing.backup_fail = true;
            if (failure == 1) failing.drift = true;
            if (failure == 2) failing.ack_fail = true;
            if (failure == 3) failing.mismatch = true;
            CHECK(run(&failing, change, &proposal) == 1);
            const char *events[] = {"RB", "RBR", "RBRSAR", "RBRSAR"};
            CHECK(strcmp(failing.events, events[failure]) == 0);
            CHECK(strcmp(proposal.status, failure < 2 ? "error" : "uncertain") == 0);
        }
        change = (Change){.cc = -1, .mode = -1, .port = -1, .channel = -1,
                          .pedal = pedal, .apply = true};
        mock = (Mock){0};
        memcpy(mock.original, current, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "no-op") == 0);
        CHECK(memcmp(proposal.expected, current, DUMP_SIZE) == 0);
        change.cc = 95;
        mock = (Mock){0};
        memcpy(mock.original, current, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0);
        CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "no-op") == 0);
        change.cc = -1;
        change.port = 3;
        CHECK(build_proposal(current, change, &proposal));
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 1 && i != 79) CHECK(proposal.expected[i] == current[i]);
        change.port = -1; change.channel = 16;
        CHECK(build_proposal(current, change, &proposal));
        CHECK(proposal.expected[offset + 2] == 15);
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 2 && i != 79) CHECK(proposal.expected[i] == current[i]);
        change.channel = -1;
        for (int cc = 0; cc <= 127; cc += 127) {
            change.cc = cc;
            CHECK(build_proposal(current, change, &proposal));
            CHECK(proposal.expected[offset + 6] == cc);
            for (size_t i = 0; i < DUMP_SIZE; ++i)
                if (i != offset + 6 && i != 79) CHECK(proposal.expected[i] == current[i]);
        }
        for (int mode = 1; mode <= 4; ++mode) {
            change.mode = mode;
            CHECK(!build_proposal(current, change, &proposal));
        }
        change.mode = -1;
        static const struct { size_t byte; Byte value; } unsupported[] = {
            {1, 0}, {1, 4}, {2, 16}, {3, 0}, {3, 2}, {4, 0}, {4, 3}, {4, 5},
            {5, 1}, {7, 126}, {8, 95}, {9, 1}
        };
        for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
            Byte invalid[DUMP_SIZE];
            memcpy(invalid, current, DUMP_SIZE);
            invalid[offset + unsupported[i].byte] = unsupported[i].value;
            invalid[79] = (Byte)checksum(invalid);
            CHECK(!build_proposal(invalid, change, &proposal));
            CHECK(!cc_record(invalid + offset));
        }
        Byte unrelated[DUMP_SIZE];
        memcpy(unrelated, current, DUMP_SIZE);
        size_t other = 9 + 10 * (size_t)(pedal == 4 ? 5 : 4);
        unrelated[12] = 2;
        unrelated[other + 7] = 100;
        unrelated[79] = (Byte)checksum(unrelated);
        CHECK(build_proposal(unrelated, change, &proposal));
        for (size_t i = 0; i < DUMP_SIZE; ++i)
            if (i != offset + 6 && i != 79) CHECK(proposal.expected[i] == unrelated[i]);
        mock = (Mock){0};
        memcpy(mock.original, unrelated, DUMP_SIZE);
        memcpy(mock.expected, proposal.expected, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 0 && strcmp(mock.events, "RBRSAR") == 0);
        unrelated[offset + 8] = 1;
        unrelated[79] = (Byte)checksum(unrelated);
        mock = (Mock){0};
        memcpy(mock.original, unrelated, DUMP_SIZE);
        CHECK(run(&mock, change, &proposal) == 1 && strcmp(mock.events, "R") == 0);
    }
    Change toe = {.cc = 74, .mode = -1, .port = -1, .channel = -1, .pedal = CONTROL_COUNT};
    Proposal proposal = {0};
    CHECK(!build_proposal(current, toe, &proposal));
}

static void public_api_tests(void) {
    FbvEdit edit = {0, 42, 1, 1, 3};
    CHECK(fbv_preview(NULL, DUMP_SIZE, edit).status == FBV_INVALID);
    CHECK(fbv_preview(captured, DUMP_SIZE - 1, edit).status == FBV_INVALID);
    CHECK(fbv_preview(captured, DUMP_SIZE + 1, edit).status == FBV_INVALID);
    CHECK(!fbv_assignment(NULL, DUMP_SIZE, 0, &edit));
    CHECK(!fbv_assignment(captured, DUMP_SIZE, 0, NULL));
    CHECK(!fbv_assignment(captured, DUMP_SIZE, -1, &edit));
    CHECK(!fbv_assignment(captured, DUMP_SIZE, 6, &edit));
    CHECK(fbv_session_read(NULL).status == FBV_INVALID);
    CHECK(fbv_session_apply(NULL, NULL, 0, edit).status == FBV_INVALID);
    CHECK(fbv_session_create(true, NULL) == NULL);
    char long_path[FBV_PATH_SIZE + 1];
    memset(long_path, 'a', sizeof(long_path) - 1); long_path[sizeof(long_path) - 1] = 0;
    CHECK(fbv_session_create(true, long_path) == NULL);
    FbvSession *session = fbv_session_create(true, "/not-created-by-demo");
    CHECK(session != NULL);
    CHECK(!fbv_demo_scenario(session, -1) && !fbv_demo_scenario(session, 8));
    FbvResult read = fbv_session_read(session);
    CHECK(read.status == FBV_READ);
    CHECK(fbv_assignment(read.dump, DUMP_SIZE, 0, &edit));
    CHECK(edit.cc == 20 && edit.mode == 3 && edit.port == 1 && edit.channel == 1);
    CHECK(fbv_demo_scenario(session, 7));
    CHECK(fbv_session_apply(session, read.dump, DUMP_SIZE, edit).status == FBV_NOOP);
    edit.cc = 42;
    static const FbvStatus errors[] = {FBV_APPLIED, FBV_DISCONNECTED, FBV_DRIFT, FBV_DRIFT,
        FBV_DRIFT, FBV_UNCERTAIN, FBV_UNCERTAIN, FBV_BACKUP_FAILED};
    for (int scenario = 0; scenario <= 7; ++scenario) {
        CHECK(fbv_demo_scenario(session, scenario));
        FbvResult result = fbv_session_apply(session, read.dump, DUMP_SIZE, edit);
        CHECK(result.status == errors[scenario]);
        CHECK(result.backup[0] == 0);
        if (scenario == 0) {
            FbvResult after = fbv_session_read(session);
            CHECK(memcmp(after.dump, result.expected, DUMP_SIZE) == 0);
        } else CHECK(result.message[0] != 0);
    }
    CHECK(fbv_demo_scenario(session, 2));
    FbvResult unsupported = fbv_session_read(session);
    CHECK(unsupported.status == FBV_READ);
    CHECK(fbv_preview(unsupported.dump, DUMP_SIZE, edit).status == FBV_UNSUPPORTED);
    fbv_session_destroy(session);
    fbv_session_destroy(NULL);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "--expression-read-json") == 0 ||
                      strcmp(argv[1], "--expression-read") == 0)) {
        Byte current[DUMP_SIZE];
        from_hex(expression_hex, current, DUMP_SIZE);
        CHECK(valid_reply(current, DUMP_SIZE));
        if (argc == 3 && strcmp(argv[2], "--unknown-wah") == 0) {
            current[52] = 2;
            current[79] = (Byte)checksum(current);
        } else if (argc != 2) return 2;
        print_reply(current, strcmp(argv[1], "--expression-read-json") == 0);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "--expression-preview-json") == 0 ||
                      strcmp(argv[1], "--expression-preview") == 0)) {
        int pedal = parse_pedal(argv[2]);
        if (pedal < SWITCH_COUNT) return 2;
        Change change = {.cc = 74, .mode = -1, .port = 2, .channel = 3,
                         .pedal = pedal, .json = strcmp(argv[1], "--expression-preview-json") == 0};
        Mock mock = {0};
        from_hex(expression_hex, mock.original, DUMP_SIZE);
        Proposal proposal = {0};
        CHECK(run(&mock, change, &proposal) == 0 && strcmp(mock.events, "R") == 0);
        print_proposal(&proposal, change);
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "--current-read-json") == 0 ||
                      strcmp(argv[1], "--current-read") == 0)) {
        Byte current[DUMP_SIZE];
        from_hex(current_hex, current, DUMP_SIZE);
        CHECK(valid_reply(current, DUMP_SIZE));
        if (argc == 3 && strcmp(argv[2], "--unknown-b") == 0) {
            current[22] = 2;
            current[79] = (Byte)checksum(current);
        } else if (argc != 2) return 2;
        print_reply(current, strcmp(argv[1], "--current-read-json") == 0);
        return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "--pedal-preview-json") == 0 ||
                      strcmp(argv[1], "--pedal-preview") == 0)) {
        if (strlen(argv[2]) != 1 || argv[2][0] < 'A' || argv[2][0] > 'D') return 2;
        Change change = {.cc = 40, .mode = 3, .port = 2, .channel = 5,
                         .pedal = argv[2][0] - 'A',
                         .json = strcmp(argv[1], "--pedal-preview-json") == 0};
        Mock mock = mock_for(change);
        Proposal proposal = {0};
        CHECK(run(&mock, change, &proposal) == 0 && strcmp(mock.events, "R") == 0);
        print_proposal(&proposal, change);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--preview-json") == 0) {
        Change change = {.cc = 19, .mode = 3, .json = true, .port = -1, .channel = -1};
        Mock mock = mock_for(change);
        Proposal proposal = {0};
        CHECK(run(&mock, change, &proposal) == 0 && strcmp(mock.events, "R") == 0);
        print_proposal(&proposal, change);
        return 0;
    }
    if (argc != 1) return 2;
    CHECK(self_test(false) == 0);
    port_channel_tests();
    pedal_tests();
    expression_tests();
    public_api_tests();
    static const struct { int cc, mode; const char *hex; } writes[] = {
        {20, 3, "f000010c110106000c020100010300147f14003af7"},
        {20, 1, "f000010c110106000c020100010100147f000024f7"},
        {19, 2, "f000010c110106000c020100010200137f130037f7"},
        {19, 3, "f000010c110106000c020100010300137f130038f7"}
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        Byte original[DUMP_SIZE], expected[ASSIGN_SIZE];
        memcpy(original, captured, DUMP_SIZE);
        original[10] = 1;
        original[11] = 0;
        original[79] = (Byte)checksum(original);
        Proposal proposal = {0};
        CHECK(build_proposal(original, (Change){.cc = writes[i].cc, .mode = writes[i].mode, .port = -1, .channel = -1}, &proposal));
        from_hex(writes[i].hex, expected, ASSIGN_SIZE);
        CHECK(memcmp(proposal.assignment, expected, ASSIGN_SIZE) == 0);
    }
    for (int port = 1; port <= 3; ++port) {
        for (int channel = 0; channel < 16; ++channel) {
            Byte dump[DUMP_SIZE];
            memcpy(dump, captured, DUMP_SIZE);
            dump[10] = (Byte)port;
            dump[11] = (Byte)channel;
            dump[79] = (Byte)checksum(dump);
            Proposal proposal = {0};
            CHECK(build_proposal(dump, (Change){.cc = 19, .mode = 3, .port = -1, .channel = -1}, &proposal));
            CHECK(proposal.assignment[10] == port && proposal.assignment[11] == channel);
            const Byte *record = proposal.expected + 9;
            Byte internal[] = {record[0], record[3], record[5], record[4], record[1],
                               record[2], record[6], record[7], record[8], record[9]};
            Byte serialised[] = {internal[0], internal[4], internal[5], internal[1], internal[3],
                                 internal[2], internal[6], internal[7], internal[8], internal[9]};
            CHECK(memcmp(proposal.assignment + 9, serialised, 10) == 0);
        }
    }
    for (int cc = 0; cc <= 127; ++cc) {
        for (int mode = 1; mode <= 3; ++mode) {
            Proposal proposal = {0};
            CHECK(build_proposal(captured, (Change){.cc = cc, .mode = mode, .port = -1, .channel = -1}, &proposal));
            CHECK(eligible(proposal.expected, 0));
            CHECK(proposal.expected[15] == cc && proposal.expected[13] == mode);
            CHECK(proposal.expected[17] == (mode == 1 ? 0 : cc));
            for (size_t i = 0; i < DUMP_SIZE; ++i)
                if (i != 13 && i != 15 && i != 17 && i != 79)
                    CHECK(proposal.expected[i] == captured[i]);
            CHECK(memcmp(proposal.assignment + 9, proposal.expected + 9, 10) == 0);
        }
    }
    static const struct { size_t offset; Byte value; } invalid[] = {
        {5, 2}, {6, 6}, {7, 5}, {8, 9}, {9, 3}, {10, 0}, {10, 4}, {11, 16},
        {12, 2}, {13, 0}, {13, 4}, {14, 1}, {16, 126}, {17, 18}, {18, 1}, {19, 2}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        Byte dump[DUMP_SIZE];
        memcpy(dump, captured, DUMP_SIZE);
        dump[invalid[i].offset] = invalid[i].value;
        dump[79] = (Byte)checksum(dump);
        CHECK(!eligible(dump, 0));
    }
    Proposal proposal = {0};
    CHECK(build_proposal(captured, (Change){.cc = -1, .mode = 3, .port = -1, .channel = -1}, &proposal));
    CHECK(proposal.expected[15] == 18 && proposal.expected[17] == 18);
    CHECK(build_proposal(captured, (Change){.cc = 19, .mode = -1, .port = -1, .channel = -1}, &proposal));
    CHECK(proposal.expected[13] == 1 && proposal.expected[17] == 0);
    Byte toggle[DUMP_SIZE];
    memcpy(toggle, captured, DUMP_SIZE);
    toggle[13] = 2; toggle[17] = 18;
    toggle[79] = (Byte)checksum(toggle);
    CHECK(build_proposal(toggle, (Change){.cc = 20, .mode = -1, .port = -1, .channel = -1}, &proposal));
    CHECK(proposal.expected[13] == 2 && proposal.expected[17] == 20);
    CHECK(!build_proposal(captured, (Change){.cc = 128, .mode = 1, .port = -1, .channel = -1}, &proposal));
    CHECK(!build_proposal(captured, (Change){.cc = -2, .mode = 1, .port = -1, .channel = -1}, &proposal));
    CHECK(!build_proposal(captured, (Change){.cc = 1, .mode = 4, .port = -1, .channel = -1}, &proposal));
    CHECK(!build_proposal(captured, (Change){.cc = 1, .mode = 0, .port = -1, .channel = -1}, &proposal));
    Byte corrupt[DUMP_SIZE];
    memcpy(corrupt, captured, DUMP_SIZE);
    corrupt[15] ^= 1;
    CHECK(!eligible(corrupt, 0));
    int cc;
    CHECK(parse_number("0", 0, 127, &cc) && cc == 0);
    CHECK(parse_number("127", 0, 127, &cc) && cc == 127);
    CHECK(!parse_number("128", 0, 127, &cc) && !parse_number("-1", 0, 127, &cc));
    CHECK(!parse_number("", 0, 127, &cc) && !parse_number("1x", 0, 127, &cc) &&
          !parse_number("+1", 0, 127, &cc));

    Change change = {.cc = 19, .mode = 3, .port = -1, .channel = -1};
    Mock mock = mock_for(change);
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "preview") == 0);
    change.apply = true;
    change.cc = 18; change.mode = 1;
    mock = mock_for(change);
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "R") == 0 && strcmp(proposal.status, "no-op") == 0);
    change.cc = 19; change.mode = 3;
    mock = mock_for(change);
    CHECK(run(&mock, change, &proposal) == 0);
    CHECK(strcmp(mock.events, "RBRSAR") == 0 && strcmp(proposal.status, "applied") == 0);
    CHECK(memcmp(mock.sent, proposal.assignment, ASSIGN_SIZE) == 0);
    for (int failure = 0; failure < 8; ++failure) {
        mock = mock_for(change);
        if (failure == 0) mock.backup_fail = true;
        if (failure == 1) mock.drift = true;
        if (failure == 2) mock.send_fail = true;
        if (failure == 3) mock.ack_fail = true;
        if (failure == 4) mock.mismatch = true;
        if (failure == 5) mock.fail_read = 1;
        if (failure == 6) mock.fail_read = 2;
        if (failure == 7) mock.fail_read = 3;
        CHECK(run(&mock, change, &proposal) == 1);
        const char *events[] = {"RB", "RBR", "RBRSR", "RBRSAR", "RBRSAR", "R", "RBR", "RBRSAR"};
        CHECK(strcmp(mock.events, events[failure]) == 0);
        CHECK(strcmp(proposal.status, (failure == 2 || failure == 3 || failure == 4 || failure == 7)
                     ? "uncertain" : "error") == 0);
    }
    mock = mock_for(change);
    mock.original[12] = 2;
    mock.original[79] = (Byte)checksum(mock.original);
    CHECK(run(&mock, change, &proposal) == 1 && strcmp(mock.events, "R") == 0);
    static const Byte success[] = {0xf0, 0, 1, 0x0c, 0x11, 1, 7, 0, 0xf7};
    for (size_t split = 0; split <= sizeof(success); ++split) {
        Parser parser = {.expect_ack = true};
        feed(&parser, captured, DUMP_SIZE);
        CHECK(!parser.ready && !parser.ack_ready);
        feed(&parser, success, split);
        feed(&parser, success + split, sizeof(success) - split);
        CHECK(parser.ack_ready && !parser.ready);
    }
    Byte negative[sizeof(success)];
    memcpy(negative, success, sizeof(success));
    negative[7] = 1;
    Parser parser = {.expect_ack = true};
    feed(&parser, negative, sizeof(negative));
    CHECK(parser.ack_failed && !parser.ack_ready);
    parser = (Parser){0};
    feed(&parser, success, sizeof(success));
    CHECK(!parser.ready && !parser.ack_ready);

    char directory[] = "/tmp/mkii-backup-test-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    snprintf(path, sizeof(path), "%s/original.syx", directory);
    CHECK(backup_to(path, captured));
    struct stat info;
    CHECK(stat(path, &info) == 0 && info.st_size == DUMP_SIZE && (info.st_mode & 0777) == 0600);
    CHECK(!backup_to(path, corrupt));
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    Byte restored[DUMP_SIZE];
    CHECK(fread(restored, 1, DUMP_SIZE, file) == DUMP_SIZE && fgetc(file) == EOF);
    CHECK(fclose(file) == 0 && memcmp(restored, captured, DUMP_SIZE) == 0);
    CHECK(unlink(path) == 0);
    CHECK(symlink("missing-target", path) == 0);
    CHECK(!backup_to(path, captured));
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
    CHECK(!backup_to("/nonexistent-mkii-test-directory/original.syx", captured));
    printf("Writer offline tests: %u checks passed; no MIDI client created, no live sends.\n", checks);
    return 0;
}
