/*
 * protocol.h - wire protocol between the TSF front-end (HaninTip, x64/x86)
 * and the 32-bit engine host (hanin_host.exe). Fixed-size structs over a
 * message-mode named pipe; layout identical for 32 and 64-bit builds.
 */
#pragma once
#include <stdint.h>

#define HANIN_PIPE_FMT    L"\\\\.\\pipe\\HaninRevival.%lu"   /* %lu = session id */
#define HANIN_MAGIC       0x4E4E4148u                        /* 'HANN' */
#define HANIN_PROTO_VER   1

enum {
    HCMD_PING  = 1,  /* no-op, returns state                                   */
    HCMD_TEST  = 2,  /* ImeProcessKey only: does the engine want this key?     */
    HCMD_KEY   = 3,  /* full key processing (ImeProcessKey + ImeToAsciiEx)      */
    HCMD_RESET = 4,  /* cancel composition / close candidate window            */
    HCMD_COMPLETE = 5,/* force-commit current composition                      */
    HCMD_TOGGLE = 6, /* toggle Chinese/English (same as a Shift tap)             */
    HCMD_LAUNCH = 7  /* start a companion program, vk = HLAUNCH_*                */
};

#define HLAUNCH_TOOLBOX 0
#define HLAUNCH_QUERY   1

#define HMETHOD_CHINESE 0
#define HMETHOD_ENGLISH 1

#define HREQ_UP 0x1  /* flags: key-up event */

#define HANIN_MAX_RESULT 256
#define HANIN_MAX_COMP   128
#define HANIN_MAX_CAND   2048

#pragma pack(push, 4)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t cmd;
    uint32_t flags;
    uint32_t vk;
    uint32_t lparam;          /* lKeyData as in WM_KEYDOWN/WM_KEYUP           */
    uint32_t clientPid;
    uint32_t clientTid;
    uint8_t  keystate[256];   /* GetKeyboardState() of the client thread       */
} HANIN_REQ;

typedef struct {
    uint32_t magic;
    uint32_t status;          /* 0 = ok, otherwise error code                 */
    uint32_t eaten;           /* key consumed by the engine                   */
    uint32_t open;            /* IME open (Chinese mode)                      */
    uint32_t convMode;        /* IME_CMODE_* from the input context           */
    uint32_t compCursor;      /* caret position inside comp, in UTF-16 units  */
    uint32_t candOpen;        /* a candidate/selection window is showing      */
    int32_t  method;          /* HMETHOD_*, -1 unknown                         */
    wchar_t  result[HANIN_MAX_RESULT];  /* text committed by this request     */
    wchar_t  comp[HANIN_MAX_COMP];      /* current composition string         */
    /* Candidate window snapshot, '\n' separated lines, '\t' separated fields:
     *   R <label> <active 0/1> <item> <item> ...   one grid row
     *   F <footer text>                             e.g. "(31 / 155)"
     *   T <title>                                   window title           */
    wchar_t  cand[HANIN_MAX_CAND];
} HANIN_RESP;
#pragma pack(pop)
