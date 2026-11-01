/* Compatibility entry point. The implementation is the same library used by the app. */
#include "Sources/FBVCore/engine.c"

int main(int argc, char **argv) {
    return fbv_cli_main(argc, argv);
}
