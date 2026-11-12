// Test stand-in for libopenvr_api.so: a scripted overlay event queue behind the same C entry points.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { kOverlayFunctions = 82, kPollNextOverlayEvent = 48, kEventBytes = 60 };

static unsigned char script[32][kEventBytes];
static int script_count;
static int script_next;

void fake_add_event(uint32_t type, const char* input, int input_bytes) {
    unsigned char* event = script[script_count++];
    memset(event, 0, kEventBytes);
    memcpy(event, &type, sizeof type);
    memcpy(event + 12, input, (size_t)input_bytes);
}

void fake_reset(void) {
    script_count = 0;
    script_next = 0;
}

static bool fake_poll(uint64_t overlay, void* event, uint32_t size) {
    (void)overlay;
    if (script_next >= script_count) {
        return false;
    }
    memcpy(event, script[script_next++], size < kEventBytes ? size : kEventBytes);
    return true;
}

void* fake_overlay_table[kOverlayFunctions];
void* fake_other_table[4];

intptr_t VR_GetGenericInterface(const char* version, int* error) {
    if (error) {
        *error = 0;
    }
    if (strcmp(version, "FnTable:IVROverlay_026") == 0) {
        for (int i = 0; i < kOverlayFunctions; ++i) {
            fake_overlay_table[i] = (void*)(uintptr_t)(0x1000 + i);
        }
        fake_overlay_table[kPollNextOverlayEvent] = (void*)fake_poll;
        return (intptr_t)fake_overlay_table;
    }
    return (intptr_t)fake_other_table;
}

intptr_t VR_InitInternal(int* error, int type) {
    (void)type;
    if (error) {
        *error = 0;
    }
    return 4242;
}

const char* VR_GetVRInitErrorAsEnglishDescription(int error) {
    (void)error;
    return "fake";
}
