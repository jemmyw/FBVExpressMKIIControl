struct FbvSession {
    bool demo;
    int scenario;
    unsigned reads;
    Byte demo_dump[DUMP_SIZE];
    char directory[FBV_PATH_SIZE];
    Channel channel;
};

static FbvResult failure(FbvStatus status) {
    FbvResult result = {.status = status};
    const char *message = "";
    switch (status) {
        case FBV_INVALID: message = "Invalid control or values. CC 0..127, port 1..3, channel 1..16; modes 1..3 on A-D only."; break;
        case FBV_UNSUPPORTED: message = "Unsupported configuration or selected CC shape. Nothing sent. Refresh after checking the device in its editor."; break;
        case FBV_DISCONNECTED: message = "Could not read the FBV Express Mk II. Check USB, close other editors and refresh. Nothing assigned."; break;
        case FBV_DRIFT: message = "Device configuration changed. Nothing assigned. Refresh and review your change again."; break;
        case FBV_BACKUP_FAILED: message = "Could not save the raw backup. Nothing assigned. Check the backup folder and free space; partial files are retained."; break;
        case FBV_UNCERTAIN: message = "Uncertain: an assignment was attempted, but acknowledgement and full readback were not both verified. No retry or rollback. Inspect the device, then deliberately refresh."; break;
        default: break;
    }
    snprintf(result.message, sizeof(result.message), "%s", message);
    return result;
}

static Change public_change(FbvEdit edit) {
    return (Change){.pedal = edit.pedal, .cc = edit.cc, .port = edit.port,
                    .channel = edit.channel, .mode = edit.mode, .apply = true};
}

static bool valid_edit(FbvEdit edit) {
    return edit.pedal >= 0 && edit.pedal < 6 && edit.cc >= -1 && edit.cc <= 127 &&
           edit.port >= -1 && edit.port != 0 && edit.port <= 3 &&
           edit.channel >= -1 && edit.channel != 0 && edit.channel <= 16 &&
           edit.mode >= -1 && edit.mode != 0 && edit.mode <= 3 &&
           (edit.pedal < 4 || edit.mode == -1);
}

static FbvResult public_proposal(Proposal *proposal, FbvStatus status) {
    FbvResult result = failure(status);
    memcpy(result.dump, proposal->original, DUMP_SIZE);
    memcpy(result.expected, proposal->expected, DUMP_SIZE);
    memcpy(result.assignment, proposal->assignment, ASSIGN_SIZE);
    snprintf(result.backup, sizeof(result.backup), "%s", proposal->backup_path);
    return result;
}

FbvResult fbv_preview(const uint8_t *dump, size_t count, FbvEdit edit) {
    if (!dump || count != DUMP_SIZE || !valid_edit(edit)) return failure(FBV_INVALID);
    Proposal proposal = {0};
    if (!build_proposal(dump, public_change(edit), &proposal)) return failure(FBV_UNSUPPORTED);
    return public_proposal(&proposal, memcmp(dump, proposal.expected, DUMP_SIZE) == 0 ? FBV_NOOP : FBV_PREVIEW);
}

bool fbv_assignment(const uint8_t *dump, size_t count, int pedal, FbvEdit *edit) {
    if (!dump || !edit || count != DUMP_SIZE || !eligible(dump, pedal)) return false;
    const Byte *record = dump + 9 + 10 * pedal;
    *edit = (FbvEdit){pedal, record[6], record[1], record[2] + 1, pedal < 4 ? record[4] : -1};
    return true;
}

bool fbv_demo_scenario(FbvSession *session, int scenario) {
    if (!session || !session->demo || scenario < 0 || scenario > 7) return false;
    session->scenario = scenario;
    session->reads = 0;
    memcpy(session->demo_dump, captured, DUMP_SIZE);
    for (int i = 0; i < 4; ++i) {
        Byte *record = session->demo_dump + 9 + 10 * i;
        record[1] = 1; record[2] = 0; record[4] = 3;
        record[6] = (Byte)(20 + i); record[8] = record[6];
    }
    if (scenario == 2) session->demo_dump[12] = 2;
    session->demo_dump[79] = (Byte)checksum(session->demo_dump);
    return true;
}

FbvSession *fbv_session_create(bool demo, const char *backup_directory) {
    if (!backup_directory || strlen(backup_directory) >= FBV_PATH_SIZE - 90) return NULL;
    FbvSession *session = calloc(1, sizeof(*session));
    if (!session) return NULL;
    session->demo = demo;
    snprintf(session->directory, sizeof(session->directory), "%s", backup_directory);
    if (demo) fbv_demo_scenario(session, 0);
    return session;
}

void fbv_session_destroy(FbvSession *session) { free(session); }

static bool session_read(void *context, Byte *dump) {
    FbvSession *session = context;
    if (!session->demo) return read_dump(&session->channel, dump);
    if (session->scenario == 1) return false;
    ++session->reads;
    if ((session->scenario == 3 && session->reads == 1) ||
        (session->scenario == 4 && session->reads == 2)) {
        session->demo_dump[70] ^= 1;
        session->demo_dump[79] = (Byte)checksum(session->demo_dump);
    }
    memcpy(dump, session->demo_dump, DUMP_SIZE);
    return true;
}

static bool session_backup(void *context, const Byte *dump, char *path) {
    FbvSession *session = context;
    if (session->demo) return session->scenario != 7;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    snprintf(path, BACKUP_PATH_SIZE, "%s/fbv-backup-%lld-%09ld-%d.syx",
             session->directory, (long long)now.tv_sec, now.tv_nsec, getpid());
    return backup_to(path, dump);
}

static bool session_assign(void *context, const Byte *assignment) {
    FbvSession *session = context;
    if (!session->demo) return assign_pedal(&session->channel, assignment);
    int pedal = control_index(assignment[9]);
    if (pedal < 0) return false;
    memcpy(session->demo_dump + 9 + 10 * pedal, assignment + 9, 10);
    if (session->scenario == 6) session->demo_dump[70] ^= 1;
    session->demo_dump[79] = (Byte)checksum(session->demo_dump);
    return true;
}

static bool session_ack(void *context) {
    FbvSession *session = context;
    return session->demo ? session->scenario != 5 : assignment_ack(&session->channel);
}

static bool session_open(FbvSession *session) {
    session->reads = 0;
    if (session->demo) return true;
    session->channel = (Channel){0};
    if (open_channel(&session->channel)) return true;
    close_channel(&session->channel);
    return false;
}

FbvResult fbv_session_read(FbvSession *session) {
    if (!session) return failure(FBV_INVALID);
    if (!session_open(session)) return failure(FBV_DISCONNECTED);
    FbvResult result = {.status = FBV_READ};
    if (!session_read(session, result.dump)) result = failure(FBV_DISCONNECTED);
    else if (!valid_reply(result.dump, DUMP_SIZE)) result = failure(FBV_UNSUPPORTED);
    if (!session->demo) close_channel(&session->channel);
    return result;
}

FbvResult fbv_session_apply(FbvSession *session, const uint8_t *baseline,
                           size_t count, FbvEdit edit) {
    if (!session || !baseline || count != DUMP_SIZE || !valid_edit(edit)) return failure(FBV_INVALID);
    FbvResult preview = fbv_preview(baseline, count, edit);
    if (preview.status != FBV_PREVIEW && preview.status != FBV_NOOP) return preview;
    if (!session_open(session)) return failure(FBV_DISCONNECTED);
    Operations operations = {session_read, session_backup, session_assign, session_ack, session};
    Proposal proposal = {0};
    int code = transact_guarded(public_change(edit), operations, &proposal, baseline);
    if (!session->demo) close_channel(&session->channel);
    FbvStatus status = code ? proposal.error : strcmp(proposal.status, "no-op") == 0 ? FBV_NOOP : FBV_APPLIED;
    return public_proposal(&proposal, status);
}
