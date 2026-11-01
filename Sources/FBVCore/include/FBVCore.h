#ifndef FBV_CORE_H
#define FBV_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "FBVMonitor.h"

#define FBV_DUMP_SIZE 81
#define FBV_ASSIGN_SIZE 21
#define FBV_PATH_SIZE 1024

typedef enum {
    FBV_READ, FBV_PREVIEW, FBV_NOOP, FBV_APPLIED,
    FBV_INVALID, FBV_UNSUPPORTED, FBV_DISCONNECTED, FBV_DRIFT,
    FBV_BACKUP_FAILED, FBV_UNCERTAIN
} FbvStatus;

typedef struct {
    int pedal, cc, port, channel, mode;
} FbvEdit;

typedef struct {
    FbvStatus status;
    uint8_t dump[FBV_DUMP_SIZE];
    uint8_t expected[FBV_DUMP_SIZE];
    uint8_t assignment[FBV_ASSIGN_SIZE];
    char backup[FBV_PATH_SIZE];
    char message[256];
} FbvResult;

typedef struct FbvSession FbvSession;
/* Demo never creates a MIDI client. A session must not be used concurrently. */
FbvSession *fbv_session_create(bool demo, const char *backup_directory);
void fbv_session_destroy(FbvSession *session);
FbvResult fbv_session_read(FbvSession *session);
FbvResult fbv_preview(const uint8_t *dump, size_t count, FbvEdit edit);
/* Baseline is the exact snapshot shown in the UI, checked before backup. */
FbvResult fbv_session_apply(FbvSession *session, const uint8_t *baseline,
                           size_t count, FbvEdit edit);
bool fbv_assignment(const uint8_t *dump, size_t count, int pedal, FbvEdit *edit);
/* Offline fault injection, refused for real sessions. 0 normal, 1 disconnected,
   2 unsupported A, 3 initial drift, 4 preflight drift, 5 acknowledgement failure,
   6 readback mismatch, 7 backup failure. Resets the demo to its fixture. */
bool fbv_demo_scenario(FbvSession *session, int scenario);
int fbv_cli_main(int argc, char **argv);
#endif
