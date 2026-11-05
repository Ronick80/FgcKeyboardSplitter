/*
 * FgcCore public C API.
 *
 * Conventions
 *  - Plain C ABI: no STL types, no exceptions, no C++ classes cross the boundary.
 *  - All strings are UTF-8, NUL-terminated, and live in caller-provided buffers or
 *    fixed-size struct fields. The library never hands out memory the caller must free.
 *  - Every struct passed in or out begins with `struct_size`. The caller sets it to
 *    sizeof(struct) before the call. The library rejects a struct smaller than it expects, except where a
 *    function says it accepts an older layout and fills only the fields that fit. Future versions append
 *    fields only after the existing ones.
 *  - Mapping threads never call back into the caller. Status and events are polled.
 *
 * Thread safety (labels used on each function)
 *  - [control]   Safe from any thread. Serialized internally by the control lock. Never
 *                taken by mapping threads, so it cannot stall input.
 *  - [lock-free] Safe from any thread, concurrently with everything else, never blocks.
 *  - [create]    Not safe to call concurrently with any other call on the same context.
 *
 * API stability: version 1 is stable from release 1.0.0. Later releases add functions and append struct fields
 * (guarded by struct_size) without changing what exists.
 */
#ifndef FGC_CORE_H
#define FGC_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(FGC_CORE_BUILD)
#define FGC_API __declspec(dllexport)
#else
#define FGC_API __declspec(dllimport)
#endif

#define FGC_API_VERSION 1u

#define FGC_MAX_XINPUT_CONTROLLERS 4u
#define FGC_HARDWARE_ID_MAX 256u
#define FGC_DISPLAY_NAME_MAX 128u
#define FGC_PROFILE_NAME_MAX 64u

typedef struct FgcContext FgcContext;

typedef enum FgcResult {
    FGC_OK = 0,
    FGC_ERR_INVALID_ARGUMENT = 1,
    FGC_ERR_NOT_IMPLEMENTED = 2,
    FGC_ERR_BUFFER_TOO_SMALL = 3,
    FGC_ERR_DRIVER_MISSING = 4,
    FGC_ERR_ALREADY_RUNNING = 5,
    FGC_ERR_NOT_RUNNING = 6,
    FGC_ERR_CONTROLLER_LIMIT = 7,
    FGC_ERR_NOT_FOUND = 8,
    FGC_ERR_IO = 9,
    FGC_ERR_INTERNAL = 10,
    FGC_ERR_ACCESS_DENIED = 11,
    FGC_ERR_DEVICE_BUSY = 12, /* another process already holds the keyboard */
    FGC_ERR_BAD_FORMAT = 13, /* a stored file could not be understood */
    FGC_ERR_ALREADY_EXISTS = 14,
    FGC_ERR_READ_ONLY = 15 /* a built-in profile cannot be saved over, renamed or deleted */
} FgcResult;

typedef enum FgcInterceptionState {
    FGC_INTERCEPTION_NOT_INSTALLED = 0,
    FGC_INTERCEPTION_REBOOT_REQUIRED = 1,
    FGC_INTERCEPTION_INSTALLED = 2
} FgcInterceptionState;

typedef struct FgcDriverStatus {
    uint32_t struct_size;
    FgcInterceptionState interception;
    int32_t scpvbus_installed; /* nonzero when the ScpVBus bus device is present */
} FgcDriverStatus;

/* Xbox inputs a key can be bound to. Analog directions are held at full deflection. */
typedef enum FgcXboxInput {
    FGC_INPUT_A = 0,
    FGC_INPUT_B,
    FGC_INPUT_X,
    FGC_INPUT_Y,
    FGC_INPUT_LEFT_BUMPER,
    FGC_INPUT_RIGHT_BUMPER,
    FGC_INPUT_LEFT_TRIGGER,
    FGC_INPUT_RIGHT_TRIGGER,
    FGC_INPUT_LEFT_STICK_CLICK,
    FGC_INPUT_RIGHT_STICK_CLICK,
    FGC_INPUT_START,
    FGC_INPUT_BACK,
    FGC_INPUT_GUIDE,
    FGC_INPUT_DPAD_UP,
    FGC_INPUT_DPAD_DOWN,
    FGC_INPUT_DPAD_LEFT,
    FGC_INPUT_DPAD_RIGHT,
    FGC_INPUT_LEFT_STICK_UP,
    FGC_INPUT_LEFT_STICK_DOWN,
    FGC_INPUT_LEFT_STICK_LEFT,
    FGC_INPUT_LEFT_STICK_RIGHT,
    FGC_INPUT_RIGHT_STICK_UP,
    FGC_INPUT_RIGHT_STICK_DOWN,
    FGC_INPUT_RIGHT_STICK_LEFT,
    FGC_INPUT_RIGHT_STICK_RIGHT,
    FGC_INPUT_COUNT
} FgcXboxInput;

/* Key code = set-1 scan code, with bit 8 (0x100) set for E0-prefixed keys. 0 = unbound. */
#define FGC_KEY_UNBOUND 0u
#define FGC_KEY_EXTENDED_FLAG 0x100u

typedef struct FgcMapping {
    uint32_t struct_size;
    uint16_t key_for_input[FGC_INPUT_COUNT];
} FgcMapping;

typedef struct FgcKeyboardInfo {
    uint32_t struct_size;
    char hardware_id[FGC_HARDWARE_ID_MAX]; /* stable across reboots and ports where possible */
    char display_name[FGC_DISPLAY_NAME_MAX];
    /* Each entry is one keyboard interface; a physical device can expose several (a keyboard's media keys, a
     * mouse's macro buttons). The fields below are filled only when the caller's struct_size covers them. */
    char product_name[FGC_DISPLAY_NAME_MAX]; /* name the device reports, e.g. "GMMK PRO"; "" if unknown */
    /* Same for every interface of one physical device. "" when it cannot be told apart from an identical
     * device that is also plugged in. */
    char device_id[FGC_HARDWARE_ID_MAX];
} FgcKeyboardInfo;

typedef struct FgcControllerLimit {
    uint32_t struct_size;
    uint32_t physical_connected; /* XInput controllers not owned by this library's running sessions */
    uint32_t virtual_active; /* virtual pads held by this library's sessions */
    uint32_t virtual_requested;
    int32_t allowed; /* nonzero when physical + active + requested <= 4 */
} FgcControllerLimit;

typedef struct FgcStatus {
    uint32_t struct_size;
    uint32_t running_sessions;
} FgcStatus;

/* Both events mean the keyboard's session already ended: the virtual pad was neutralized and released and
 * the keyboard handed back to Windows. Calling fgc_start again resumes it. */
typedef enum FgcEventType {
    FGC_EVENT_NONE = 0,
    FGC_EVENT_KEYBOARD_DISCONNECTED = 1, /* the keyboard was unplugged */
    FGC_EVENT_SESSION_FAULTED = 2        /* contact with the keyboard driver was lost */
} FgcEventType;

typedef struct FgcEvent {
    uint32_t struct_size;
    FgcEventType type;
    char hardware_id[FGC_HARDWARE_ID_MAX];
} FgcEvent;

/* Application latency of one mapping session, in nanoseconds: from the mapping thread waking on a key
 * event to the virtual controller accepting the resulting report. It excludes keyboard and USB delay
 * before the event and the game's own controller polling after it. Only events that change the
 * controller state are counted. */
typedef struct FgcLatencyStats {
    uint32_t struct_size;
    uint64_t sample_count;
    uint64_t last_ns;
    uint64_t average_ns;
    uint64_t max_ns;
    /* Where the time goes: the keyboard driver read after the thread wakes, and the virtual controller call.
     * Present only when the caller's struct_size covers them. */
    uint64_t read_average_ns;
    uint64_t read_max_ns;
    uint64_t report_average_ns;
    uint64_t report_max_ns;
} FgcLatencyStats;

/* Lifetime */

/* [lock-free] Product version, for example "1.0.0". Static UTF-8 string owned by the library. */
FGC_API const char* fgc_get_version(void);
/* [lock-free] */
FGC_API uint32_t fgc_get_api_version(void);
/* [create] Creates a context. Destroy it with fgc_destroy. */
FGC_API FgcResult fgc_create(FgcContext** out_context);
/* [create] Stops all sessions, restores keyboards, and releases the context. */
FGC_API void fgc_destroy(FgcContext* context);

/* Drivers */

/* [control] Queries the system on every call, so call it on demand rather than per frame. */
FGC_API FgcResult fgc_get_driver_status(FgcContext* context, FgcDriverStatus* out_status);

/* Keyboards */

/* [control] Rescans devices and replaces the cached list. */
FGC_API FgcResult fgc_refresh_keyboards(FgcContext* context);
/* [control] Reads the cached list from the last refresh. */
FGC_API FgcResult fgc_get_keyboard_count(FgcContext* context, uint32_t* out_count);
/* [control] Reads the cached list from the last refresh. */
FGC_API FgcResult fgc_get_keyboard(FgcContext* context, uint32_t index, FgcKeyboardInfo* out_info);
/* [control] Starts watching all idle keyboards; their keys still reach Windows normally. */
FGC_API FgcResult fgc_identify_begin(FgcContext* context);
/* [control] Stops identify mode. */
FGC_API FgcResult fgc_identify_cancel(FgcContext* context);
/* [control] Sets *out_index to the identified keyboard's list index, or -1 if no key was pressed yet. */
FGC_API FgcResult fgc_identify_poll(FgcContext* context, int32_t* out_index);
/* [control] Starts reporting every key press on idle keyboards, for a "press any key to join" screen. Their keys
 * still reach Windows normally. Watching continues until fgc_watch_end: keyboards that start drop out of it,
 * keyboards that stop rejoin it, and newly plugged keyboards are picked up. Replaces identify mode. */
FGC_API FgcResult fgc_watch_begin(FgcContext* context);
/* [control] Stops watching. */
FGC_API FgcResult fgc_watch_end(FgcContext* context);
/* [control] Takes the oldest key press not yet returned. *out_pressed is nonzero when out_info holds the keyboard
 * interface it came from (key repeats from holding a key are not reported). Call it regularly while watching; it
 * also rescans for plugged-in keyboards about once a second. */
FGC_API FgcResult fgc_watch_poll(FgcContext* context, FgcKeyboardInfo* out_info, int32_t* out_pressed);

/* Mapping (live: takes effect on a running session without stalling input) */

/* [lock-free] Fills the mapping used for keyboards that have none of their own. */
FGC_API FgcResult fgc_get_default_mapping(FgcMapping* out_mapping);
/* [control] Returns the keyboard's current mapping. A keyboard that has none yet starts from its last-used
   profile, or the default mapping when it has no usable one. */
FGC_API FgcResult fgc_get_mapping(FgcContext* context, const char* hardware_id, FgcMapping* out_mapping);
/* [control] Sets the keyboard's current mapping, live if it is running. Not saved to any profile. */
FGC_API FgcResult fgc_set_mapping(FgcContext* context, const char* hardware_id, const FgcMapping* mapping);

/* Profiles (stored per keyboard hardware ID)
 *
 * A profile is a named mapping. Files live in %APPDATA%\FgcKeyboardSplitter:
 *   profiles\<hardware id>\<name>.json   one file per profile; <hardware id> has \ / : * ? " < > | replaced by _
 *   last_used.json                       {"format":"fgc-last-used","version":1,"profiles":{"<hardware id>":"<name>"}}
 * The profile name is the file name. Names are 1-63 UTF-8 bytes and cannot contain \ / : * ? " < > |
 * or control characters, start or end with a space, end with a dot, or be a reserved Windows device name.
 * Names are compared case-insensitively, as on the file system.
 *
 * Profile file (UTF-8 JSON):
 *   {"format":"fgc-profile","version":1,"keyboard":"<hardware id>",
 *    "bindings":{"a":36,"dpad_up":17,"left_stick_left":331, ...}}
 * Each binding is a key code: set-1 scan code, plus 256 for E0-prefixed keys, 0 for unbound. Binding names
 * follow FgcXboxInput: a b x y left_bumper right_bumper left_trigger right_trigger left_stick_click
 * right_stick_click start back guide dpad_up dpad_down dpad_left dpad_right left_stick_up left_stick_down
 * left_stick_left left_stick_right right_stick_up right_stick_down right_stick_left right_stick_right.
 * A missing binding is unbound; unknown names are ignored. A "version" above 1 is rejected.
 *
 * Every keyboard also has built-in profiles that are not stored as files: "Default" (the mapping
 * fgc_get_default_mapping returns) and "2XKO Default" (2XKO's default keyboard layout on the buttons of 2XKO's
 * default arcade stick layout). A keyboard with no last-used profile starts from "Default". They can be loaded and are remembered
 * as last used, but saving over, renaming or deleting one fails with FGC_ERR_READ_ONLY, and renaming a profile
 * to a built-in name fails with FGC_ERR_ALREADY_EXISTS. A profile file with a built-in name is ignored.
 */

/* [lock-free] Sets *out_read_only to 1 for a built-in profile name (case-insensitive), else 0. */
FGC_API FgcResult fgc_profile_is_read_only(const char* name, int32_t* out_read_only);
/* [control] */
FGC_API FgcResult fgc_profile_count(FgcContext* context, const char* hardware_id, uint32_t* out_count);
/* [control] Built-in profiles come first in a fixed order, then the keyboard's own profiles sorted
   case-insensitively. FGC_ERR_NOT_FOUND when index is out of range. */
FGC_API FgcResult fgc_profile_get_name(FgcContext* context, const char* hardware_id, uint32_t index,
                                       char* out_name, uint32_t out_name_size);
/* [control] Makes the profile the keyboard's current mapping (live if running) and marks it as last used.
   out_mapping may be NULL. */
FGC_API FgcResult fgc_profile_load(FgcContext* context, const char* hardware_id, const char* name,
                                   FgcMapping* out_mapping);
/* [control] Creates or replaces a profile from the given mapping and marks it as last used. */
FGC_API FgcResult fgc_profile_save(FgcContext* context, const char* hardware_id, const char* name,
                                   const FgcMapping* mapping);
/* [control] FGC_ERR_ALREADY_EXISTS if new_name belongs to a different profile. */
FGC_API FgcResult fgc_profile_rename(FgcContext* context, const char* hardware_id, const char* old_name,
                                     const char* new_name);
/* [control] */
FGC_API FgcResult fgc_profile_delete(FgcContext* context, const char* hardware_id, const char* name);
/* [control] Returns "Default" when the keyboard has no last-used profile, or its profile file is gone. */
FGC_API FgcResult fgc_profile_get_last_used(FgcContext* context, const char* hardware_id, char* out_name,
                                            uint32_t out_name_size);

/* Controller limit */

/* [control] Counts XInput controllers and this library's virtual pads, plus the pads about to be added.
   A pad started less than 3 seconds ago may not be visible to XInput yet, so it is counted conservatively. */
FGC_API FgcResult fgc_check_controller_limit(FgcContext* context, uint32_t virtual_requested,
                                             FgcControllerLimit* out_limit);

/* Sessions */

/* [control] Fails with FGC_ERR_CONTROLLER_LIMIT if the 4-controller cap would be exceeded. Keyboards run
   independently, each on its own thread and virtual pad. */
FGC_API FgcResult fgc_start(FgcContext* context, const char* hardware_id);
/* [control] Resets the virtual pad to neutral and releases the keyboard. */
FGC_API FgcResult fgc_stop(FgcContext* context, const char* hardware_id);
/* [control] */
FGC_API FgcResult fgc_stop_all(FgcContext* context);
/* [control] Sets *out_running to nonzero while the keyboard's mapping thread is alive. */
FGC_API FgcResult fgc_is_running(FgcContext* context, const char* hardware_id, int32_t* out_running);
/* [control] The Xbox inputs a running keyboard holds right now: bit (1u << FgcXboxInput) is set while a key bound to
   that input is down, so a UI can show its mapping working. The mapping thread publishes this after each controller
   report, so reading it never delays a report; polling about once per frame is fine. FGC_ERR_NOT_RUNNING (and 0)
   when the keyboard is not running. */
FGC_API FgcResult fgc_get_held_inputs(FgcContext* context, const char* hardware_id, uint32_t* out_inputs);
/* [control] How many keys a running keyboard has pressed since it started (key repeats excluded; wraps around), so
   a UI can notice which running keyboard is being used. Published like held inputs, so it never delays a report.
   FGC_ERR_NOT_RUNNING (and 0) when the keyboard is not running. */
FGC_API FgcResult fgc_get_key_press_count(FgcContext* context, const char* hardware_id, uint32_t* out_count);
/* [lock-free] */
FGC_API FgcResult fgc_get_status(FgcContext* context, FgcStatus* out_status);
/* [lock-free] Pops one event; out_event->type is FGC_EVENT_NONE when the queue is empty. */
FGC_API FgcResult fgc_poll_event(FgcContext* context, FgcEvent* out_event);
/* [control] FGC_ERR_NOT_RUNNING when the keyboard has no session. */
FGC_API FgcResult fgc_get_latency_stats(FgcContext* context, const char* hardware_id,
                                        FgcLatencyStats* out_stats);
/* [control] Clears the statistics, including the maximum. */
FGC_API FgcResult fgc_reset_latency_stats(FgcContext* context, const char* hardware_id);

#ifdef __cplusplus
}
#endif

#endif /* FGC_CORE_H */
