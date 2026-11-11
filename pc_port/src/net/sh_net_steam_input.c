/* Controllers through Steam Input.
 *
 * The online build initialises Steam under an app id Steam treats as a native
 * Steam Input title. Steam then hides every Steam-Input-enabled controller
 * from XInput, raw input and HID -- so from SDL -- and expects the game to
 * read ISteamInput. This does that: the game hands Steam its own action
 * manifest (one action per pad button, stick and trigger; gamedata/
 * steam_input, written by pc_port/tools/gen_steam_input.py), reads those
 * actions each frame and feeds them to PsyCross as a stand-in game controller
 * (PsyX_pad_steam.cpp), so the game's own bind screens stay in charge of what
 * each button does and every controller type Steam supports works, rumble
 * included.
 *
 * Which layout Steam applies is the player's choice, kept per app id. One who
 * once picked a layout for this app id (any game that borrowed it) keeps that
 * layout, none of our actions are bound, and the controller reads as idle.
 * That state is detected and said once; PsyCross's direct XInput read covers
 * Xbox-type pads meanwhile.
 *
 * Nothing here runs unless Steam came up: without it SDL owns the pads.
 *
 * Threads. Every Steam call in the game is made on the network worker, which
 * owns the callback pipe (manual dispatch); a Steam call from any other thread
 * while callbacks are queued makes Steam assert and mis-deliver them, and the
 * first version of this file, whose rumble went out from the game thread every
 * frame, took lobbies and peer connections down with it. SDL's joysticks
 * belong to the game thread. So the worker reads Steam into s_shared and sends
 * the rumble it finds there; the game thread copies s_shared into the stand-in
 * controllers and leaves its rumble requests there. Neither side calls into
 * the other's library. */
#include <stdio.h>
#include <string.h>
#include <SDL.h>

#include "sh_log.h"
#include "pc_config.h"

void ShSteamInput_Start(void);
void ShSteamInput_Tick(void);
void ShSteamInput_Stop(void);
int  ShSteamInput_NeedsLayout(void);
void ShSteamInput_ShowLayoutScreen(void);

#if defined(_WIN32)

#include <stdlib.h>

extern void* ShSteam_Symbol(const char* name);

extern void PsyX_Pad_SteamBypassHold(int hold);
extern void PsyX_Pad_StandInFeed(int slot, int present, unsigned buttons, const short* axes, const char* name, unsigned vid, unsigned pid);
extern void PsyX_Pad_StandInSetRumble(int (*fn)(int slot, unsigned short low, unsigned short high));
extern void PsyX_Pad_StandInSetSource(void (*fn)(void));

void ShSteamInput_ApplyToPads(void);

typedef unsigned long long u64h;

#pragma pack(push, 1)
typedef struct { unsigned char state, active; } SiDigital;
typedef struct { int mode; float x, y; unsigned char active; } SiAnalog;
#pragma pack(pop)

enum { SI_MAX = 16, SI_SLOTS = 4, SI_BUTTONS = 14 };

/* Action names as gen_steam_input.py writes them, with the SDL controller
 * button each one stands for (the stand-in's raw button index). */
static const struct { const char* name; int button; } s_buttonDef[SI_BUTTONS] = {
    { "pad_a", SDL_CONTROLLER_BUTTON_A },           { "pad_b", SDL_CONTROLLER_BUTTON_B },
    { "pad_x", SDL_CONTROLLER_BUTTON_X },           { "pad_y", SDL_CONTROLLER_BUTTON_Y },
    { "pad_lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER }, { "pad_rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { "pad_back", SDL_CONTROLLER_BUTTON_BACK },     { "pad_start", SDL_CONTROLLER_BUTTON_START },
    { "pad_ls", SDL_CONTROLLER_BUTTON_LEFTSTICK },  { "pad_rs", SDL_CONTROLLER_BUTTON_RIGHTSTICK },
    { "pad_up", SDL_CONTROLLER_BUTTON_DPAD_UP },    { "pad_down", SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { "pad_left", SDL_CONTROLLER_BUTTON_DPAD_LEFT }, { "pad_right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
};

/* What crosses between the two threads, under s_sharedLock. */
typedef struct
{
    int            present;
    unsigned       buttons;
    short          axes[SDL_CONTROLLER_AXIS_MAX];
    int            type;
    unsigned short rumbleLow, rumbleHigh;
    int            rumbleDirty;
} SiShared;

static SDL_SpinLock s_sharedLock;
static SiShared     s_shared[4];
static int          s_sharedAnyBound;

static void* s_input;
static int   s_started;
static u64h  s_set, s_button[SI_BUTTONS], s_leftStick, s_rightStick, s_leftTrigger, s_rightTrigger;
static u64h  s_slotHandle[SI_SLOTS];
static int   s_slotBound[SI_SLOTS];
static Uint32 s_nextCheck;
static int   s_needsLayout, s_saidUnbound, s_saidBound;
static Uint32 s_unboundSince;

static unsigned char (*s_SetManifest)(void*, const char*);
static unsigned char (*s_Init)(void*, unsigned char);
static unsigned char (*s_Shutdown)(void*);
static void          (*s_RunFrame)(void*, unsigned char);
static int           (*s_GetControllers)(void*, u64h*);
static int           (*s_GetType)(void*, u64h);
static u64h          (*s_GetSet)(void*, const char*);
static u64h          (*s_GetDigitalHandle)(void*, const char*);
static u64h          (*s_GetAnalogHandle)(void*, const char*);
static void          (*s_ActivateSet)(void*, u64h, u64h);
static SiDigital     (*s_GetDigital)(void*, u64h, u64h);
static SiAnalog      (*s_GetAnalog)(void*, u64h, u64h);
static int           (*s_GetDigitalOrigins)(void*, u64h, u64h, u64h, int*);
static void          (*s_Vibrate)(void*, u64h, unsigned short, unsigned short);
static unsigned char (*s_ShowBindingPanel)(void*, u64h);

/* What the stand-in calls itself, so the glyphs and the controls panel show
 * the pad the player is holding. Index is Steam's ESteamInputType. */
static void SteamInput_Identity(int type, const char** name, unsigned* vid, unsigned* pid)
{
    switch (type)
    {
        case 5:  *name = "PS4 Controller";        *vid = 0x054c; *pid = 0x09cc; break;
        case 12: *name = "PS3 Controller";        *vid = 0x054c; *pid = 0x0268; break;
        case 13: *name = "PS5 Controller";        *vid = 0x054c; *pid = 0x0ce6; break;
        case 8:
        case 9:
        case 10: *name = "Nintendo Switch Pro Controller"; *vid = 0x057e; *pid = 0x2009; break;
        case 1:  *name = "Steam Controller";      *vid = 0x28de; *pid = 0x1142; break;
        case 14: *name = "Steam Deck";            *vid = 0x28de; *pid = 0x1205; break;
        case 3:  *name = "Xbox One Controller";   *vid = 0x045e; *pid = 0x02ea; break;
        default: *name = "Xbox 360 Controller";   *vid = 0x045e; *pid = 0x028e; break;
    }
}

/* Game thread: only notes the request; the worker sends it. */
static int SteamInput_Rumble(int slot, unsigned short low, unsigned short high)
{
    if (slot < 0 || slot >= SI_SLOTS)
        return 0;
    SDL_AtomicLock(&s_sharedLock);
    if (s_shared[slot].rumbleLow != low || s_shared[slot].rumbleHigh != high)
    {
        s_shared[slot].rumbleLow   = low;
        s_shared[slot].rumbleHigh  = high;
        s_shared[slot].rumbleDirty = 1;
    }
    SDL_AtomicUnlock(&s_sharedLock);
    return 1;
}

/* Game thread, once per pad update (PsyCross calls it): the worker's latest
 * reading becomes the stand-in controllers. */
void ShSteamInput_ApplyToPads(void)
{
    SiShared snap[SI_SLOTS];
    int      i, anyBound;

    SDL_AtomicLock(&s_sharedLock);
    memcpy(snap, s_shared, sizeof(snap));
    anyBound = s_sharedAnyBound;
    SDL_AtomicUnlock(&s_sharedLock);

    for (i = 0; i < SI_SLOTS; i++)
    {
        const char* name = NULL;
        unsigned    vid = 0, pid = 0;

        if (snap[i].present)
            SteamInput_Identity(snap[i].type, &name, &vid, &pid);
        PsyX_Pad_StandInFeed(i, snap[i].present, snap[i].buttons, snap[i].axes, name, vid, pid);
    }
    /* The XInput fallback stands down once Steam Input is delivering. */
    PsyX_Pad_SteamBypassHold(anyBound);
}

static void SteamInput_Share(int slot, int present, unsigned buttons, const short* axes, int type)
{
    SDL_AtomicLock(&s_sharedLock);
    s_shared[slot].present = present;
    s_shared[slot].buttons = present ? buttons : 0;
    s_shared[slot].type    = type;
    if (present && axes != NULL)
        memcpy(s_shared[slot].axes, axes, sizeof(s_shared[slot].axes));
    SDL_AtomicUnlock(&s_sharedLock);
}

static short SteamInput_Axis(float v)
{
    int i = (int)(v * 32767.0f);
    return (short)(i > 32767 ? 32767 : (i < -32767 ? -32767 : i));
}

void ShSteamInput_Start(void)
{
    static const char* const getters[] = { "SteamAPI_SteamInput_v006", "SteamAPI_SteamInput_v005", "SteamAPI_SteamInput_v002",
                                           "SteamAPI_SteamInput_v001" };
    char  path[1024];
    char* full;
    int   i;

    if (s_started)
        return;
    if (!g_PcConfig.onlineSteamInput)
    {
        SH_DBG("[STEAMINPUT] off (online_steam_input = 0); controllers Steam hides stay on the XInput fallback");
        return;
    }
    for (i = 0; i < 4 && s_input == NULL; i++)
    {
        void* (*get)(void) = (void* (*)(void))ShSteam_Symbol(getters[i]);
        if (get != NULL)
            s_input = get();
    }
    s_SetManifest       = (unsigned char (*)(void*, const char*))ShSteam_Symbol("SteamAPI_ISteamInput_SetInputActionManifestFilePath");
    s_Init              = (unsigned char (*)(void*, unsigned char))ShSteam_Symbol("SteamAPI_ISteamInput_Init");
    s_Shutdown          = (unsigned char (*)(void*))ShSteam_Symbol("SteamAPI_ISteamInput_Shutdown");
    s_RunFrame          = (void (*)(void*, unsigned char))ShSteam_Symbol("SteamAPI_ISteamInput_RunFrame");
    s_GetControllers    = (int (*)(void*, u64h*))ShSteam_Symbol("SteamAPI_ISteamInput_GetConnectedControllers");
    s_GetType           = (int (*)(void*, u64h))ShSteam_Symbol("SteamAPI_ISteamInput_GetInputTypeForHandle");
    s_GetSet            = (u64h (*)(void*, const char*))ShSteam_Symbol("SteamAPI_ISteamInput_GetActionSetHandle");
    s_GetDigitalHandle  = (u64h (*)(void*, const char*))ShSteam_Symbol("SteamAPI_ISteamInput_GetDigitalActionHandle");
    s_GetAnalogHandle   = (u64h (*)(void*, const char*))ShSteam_Symbol("SteamAPI_ISteamInput_GetAnalogActionHandle");
    s_ActivateSet       = (void (*)(void*, u64h, u64h))ShSteam_Symbol("SteamAPI_ISteamInput_ActivateActionSet");
    s_GetDigital        = (SiDigital (*)(void*, u64h, u64h))ShSteam_Symbol("SteamAPI_ISteamInput_GetDigitalActionData");
    s_GetAnalog         = (SiAnalog (*)(void*, u64h, u64h))ShSteam_Symbol("SteamAPI_ISteamInput_GetAnalogActionData");
    s_GetDigitalOrigins = (int (*)(void*, u64h, u64h, u64h, int*))ShSteam_Symbol("SteamAPI_ISteamInput_GetDigitalActionOrigins");
    s_Vibrate           = (void (*)(void*, u64h, unsigned short, unsigned short))ShSteam_Symbol("SteamAPI_ISteamInput_TriggerVibration");
    s_ShowBindingPanel  = (unsigned char (*)(void*, u64h))ShSteam_Symbol("SteamAPI_ISteamInput_ShowBindingPanel");

    if (s_input == NULL || !s_SetManifest || !s_Init || !s_RunFrame || !s_GetControllers || !s_GetSet || !s_GetDigitalHandle ||
        !s_GetAnalogHandle || !s_ActivateSet || !s_GetDigital || !s_GetAnalog)
    {
        SH_DBG("[STEAMINPUT] this steam_api64.dll has no usable ISteamInput; controllers Steam hides stay on the XInput fallback");
        return;
    }

    /* Steam wants an absolute path; the layouts it names sit beside it. */
    full = _fullpath(path, "gamedata/steam_input/steam_input_manifest.vdf", sizeof(path));
    if (full == NULL || !s_SetManifest(s_input, path))
    {
        SH_DBG("[STEAMINPUT] action manifest not accepted (%s); controllers Steam hides stay on the XInput fallback",
               full ? path : "no path");
        return;
    }
    if (!s_Init(s_input, 1))
    {
        SH_DBG("[STEAMINPUT] ISteamInput Init failed");
        return;
    }
    PsyX_Pad_StandInSetRumble(SteamInput_Rumble);
    PsyX_Pad_StandInSetSource(ShSteamInput_ApplyToPads);
    s_started = 1;
    SH_DBG("[STEAMINPUT] started with %s", path);
}

void ShSteamInput_Stop(void)
{
    int i;

    if (!s_started)
        return;
    for (i = 0; i < SI_SLOTS; i++)
    {
        SteamInput_Share(i, 0, 0, NULL, 0);
        s_slotHandle[i] = 0;
        s_slotBound[i]  = 0;
    }
    SDL_AtomicLock(&s_sharedLock);
    s_sharedAnyBound = 0;
    SDL_AtomicUnlock(&s_sharedLock);
    if (s_Shutdown != NULL)
        s_Shutdown(s_input);
    s_started = 0;
}

int ShSteamInput_NeedsLayout(void)
{
    return s_needsLayout;
}

static volatile int s_wantLayoutScreen;

/* Any thread: the worker opens it on its next tick. */
void ShSteamInput_ShowLayoutScreen(void)
{
    s_wantLayoutScreen = 1;
}

void ShSteamInput_Tick(void)
{
    u64h   handles[SI_MAX];
    int    n, i, k, anyBound = 0, anyUnbound = 0;
    Uint32 now;
    int    check;

    if (!s_started)
        return;
    s_RunFrame(s_input, 0);
    if (s_wantLayoutScreen)
    {
        s_wantLayoutScreen = 0;
        for (i = 0; i < SI_SLOTS && s_ShowBindingPanel != NULL; i++)
        {
            if (s_slotHandle[i] != 0)
            {
                s_ShowBindingPanel(s_input, s_slotHandle[i]);
                break;
            }
        }
    }

    /* Handles resolve once Steam has parsed the manifest, a frame or two in. */
    if (s_set == 0)
    {
        s_set = s_GetSet(s_input, "sh_pad");
        if (s_set == 0)
            return;
        for (i = 0; i < SI_BUTTONS; i++)
            s_button[i] = s_GetDigitalHandle(s_input, s_buttonDef[i].name);
        s_leftStick    = s_GetAnalogHandle(s_input, "left_stick");
        s_rightStick   = s_GetAnalogHandle(s_input, "right_stick");
        s_leftTrigger  = s_GetAnalogHandle(s_input, "left_trigger");
        s_rightTrigger = s_GetAnalogHandle(s_input, "right_trigger");
    }

    memset(handles, 0, sizeof(handles));
    n = s_GetControllers(s_input, handles);
    if (n < 0)
        n = 0;

    /* Keep each controller on the slot it first took; free slots whose
     * controller is gone. */
    for (i = 0; i < SI_SLOTS; i++)
    {
        int alive = 0;
        for (k = 0; k < n && !alive; k++)
            alive = (s_slotHandle[i] != 0 && handles[k] == s_slotHandle[i]);
        if (s_slotHandle[i] != 0 && !alive)
        {
            SteamInput_Share(i, 0, 0, NULL, 0);
            s_slotHandle[i] = 0;
            s_slotBound[i]  = 0;
        }
    }
    for (k = 0; k < n; k++)
    {
        int have = 0, free = -1;
        for (i = 0; i < SI_SLOTS; i++)
        {
            if (s_slotHandle[i] == handles[k])
                have = 1;
            else if (s_slotHandle[i] == 0 && free < 0)
                free = i;
        }
        if (!have && free >= 0)
        {
            s_slotHandle[free] = handles[k];
            s_slotBound[free]  = 0;
            s_nextCheck        = 0;
        }
    }

    now   = SDL_GetTicks();
    check = (s_nextCheck == 0 || (Sint32)(now - s_nextCheck) >= 0);
    if (check)
        s_nextCheck = now + 1000;

    for (i = 0; i < SI_SLOTS; i++)
    {
        unsigned buttons = 0;
        short    axes[SDL_CONTROLLER_AXIS_MAX];
        SiAnalog a;

        if (s_slotHandle[i] == 0)
            continue;
        /* Rumble the game thread asked for since the last tick. */
        if (s_Vibrate != NULL)
        {
            unsigned short low, high;
            int            dirty;

            SDL_AtomicLock(&s_sharedLock);
            dirty = s_shared[i].rumbleDirty;
            low   = s_shared[i].rumbleLow;
            high  = s_shared[i].rumbleHigh;
            s_shared[i].rumbleDirty = 0;
            SDL_AtomicUnlock(&s_sharedLock);
            if (dirty)
                s_Vibrate(s_input, s_slotHandle[i], low, high);
        }
        s_ActivateSet(s_input, s_slotHandle[i], s_set);

        /* Bound = Steam applied a layout that knows our actions. The player's
         * own earlier pick for this app id wins over our defaults, and then
         * nothing is bound. */
        if (check)
        {
            int origins[SI_MAX];
            memset(origins, 0, sizeof(origins));
            s_slotBound[i] = (s_GetDigitalOrigins != NULL &&
                              s_GetDigitalOrigins(s_input, s_slotHandle[i], s_set, s_button[0], origins) > 0);
        }
        if (!s_slotBound[i])
        {
            anyUnbound = 1;
            SteamInput_Share(i, 0, 0, NULL, 0);
            continue;
        }
        anyBound = 1;

        for (k = 0; k < SI_BUTTONS; k++)
            if (s_GetDigital(s_input, s_slotHandle[i], s_button[k]).state)
                buttons |= 1u << s_buttonDef[k].button;
        /* Steam's sticks point up for +y; SDL's point down. */
        a = s_GetAnalog(s_input, s_slotHandle[i], s_leftStick);
        axes[SDL_CONTROLLER_AXIS_LEFTX] = SteamInput_Axis(a.x);
        axes[SDL_CONTROLLER_AXIS_LEFTY] = SteamInput_Axis(-a.y);
        a = s_GetAnalog(s_input, s_slotHandle[i], s_rightStick);
        axes[SDL_CONTROLLER_AXIS_RIGHTX] = SteamInput_Axis(a.x);
        axes[SDL_CONTROLLER_AXIS_RIGHTY] = SteamInput_Axis(-a.y);
        /* The stand-in's trigger axes run -32768 (released) to 32767. */
        a = s_GetAnalog(s_input, s_slotHandle[i], s_leftTrigger);
        axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = (short)((int)(a.x * 65535.0f) - 32768);
        a = s_GetAnalog(s_input, s_slotHandle[i], s_rightTrigger);
        axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = (short)((int)(a.x * 65535.0f) - 32768);

        SteamInput_Share(i, 1, buttons, axes, s_GetType ? s_GetType(s_input, s_slotHandle[i]) : 0);
    }

    SDL_AtomicLock(&s_sharedLock);
    s_sharedAnyBound = anyBound;
    SDL_AtomicUnlock(&s_sharedLock);
    /* Steam applies a layout a moment after the controller appears, so "none
     * bound" only means something once it has lasted. */
    if (anyUnbound && !anyBound)
    {
        if (s_unboundSince == 0)
            s_unboundSince = now | 1;
        s_needsLayout = ((Sint32)(now - s_unboundSince) >= 20000);
    }
    else
    {
        s_unboundSince = 0;
        s_needsLayout  = 0;
    }

    if (check)
    {
        if (anyBound && !s_saidBound)
        {
            s_saidBound = 1;
            SH_DBG("[STEAMINPUT] reading controllers through Steam Input");
        }
        if (s_needsLayout && !s_saidUnbound)
        {
            s_saidUnbound = 1;
            SH_DBG("[STEAMINPUT] Steam has a controller but none of the game's actions are bound: another layout is "
                   "selected for this app id in Steam. Choose the \"Silent Hill PC\" layout in Steam's controller "
                   "settings for full support; Xbox-type pads work meanwhile through the direct read.");
        }
    }
}

#else

void ShSteamInput_Start(void) {}
void ShSteamInput_Tick(void) {}
void ShSteamInput_Stop(void) {}
int  ShSteamInput_NeedsLayout(void) { return 0; }
void ShSteamInput_ShowLayoutScreen(void) {}

#endif
