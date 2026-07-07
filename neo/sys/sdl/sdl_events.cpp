/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012 dhewg (dhewm3)
Copyright (C) 2012 Robert Beckebans
Copyright (C) 2013 Daniel Gibson
Copyright (C) 2018 George Kalampokis

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.	If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#include "../../idlib/precompiled.h"

#undef strncmp
#undef strcasecmp
#undef vsnprintf

#include <SDL3/SDL.h>

#include "renderer/RenderCommon.h"
#include "sdl_local.h"
#include "../posix/posix_public.h"
#include "../common/localuser.h"
#include "../../framework/Common.h"

static const int MAX_JOYSTICKS = 4;

extern idCVar r_windowX;
extern idCVar r_windowY;
extern idCVar r_windowWidth;
extern idCVar r_windowHeight;
extern idCVar com_emergencyexit;

SDL_Thread* joyThread = nullptr;
void JoystickSamplingThread( void* data );

const char* kbdNames[] =
        {
                "english", "french", "german", "italian", "spanish", "turkish", "norwegian", NULL
        };

idCVar in_keyboard( "in_keyboard", "english", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT, "keyboard layout", kbdNames, idCmdSystem::ArgCompletion_String<kbdNames> );

struct kbd_poll_t
{
    int key;
    bool state;

    kbd_poll_t() {}
    kbd_poll_t( int k, bool s ) : key(k), state(s) {}
};

struct mouse_poll_t
{
    int action;
    int value;

    mouse_poll_t() {}
    mouse_poll_t( int a, int v ) : action(a), value(v) {}
};

static idList<kbd_poll_t> kbd_polls;
static idList<mouse_poll_t> mouse_polls;

struct joystick_poll_t
{
    int action;
    int value;
};

struct joyState {
    int buttons[15];
    int LTrigger;
    int RTrigger;
    int LXThumb;
    int LYThumb;
    int RXThumb;
    int RYThumb;
};

static joyState current[4];
static joyState old[4];
static joystick_poll_t joystick_polls[42];
static int numEvents = 0;
int joyAxis[MAX_JOYSTICKS][6];
SDL_Joystick* joy = NULL;
static SDL_Gamepad* gcontroller[MAX_JOYSTICKS] = {NULL};
static int registeredControllers = 0;
static bool joyThreadKill = false;
int SDL_joystick_has_hat = 0;
bool buttonStates[MAX_JOYSTICKS][K_LAST_KEY];
extern SDL_Window* window;

#ifndef ANDROID
#define MAX_QUED_EVENTS		256
#else
#define MAX_QUED_EVENTS		1024
#endif
#define MASK_QUED_EVENTS	( MAX_QUED_EVENTS - 1 )

sysEvent_t	eventQue[MAX_QUED_EVENTS];
int			eventHead = 0;
int			eventTail = 0;

#include "sdl2_scancode_mappings.h"
#include <map>

static int SDLScanCodeToKeyNum( SDL_Scancode sc )
{
    int idx = int( sc );
    assert( idx >= 0 && idx < SDL_SCANCODE_COUNT );
    return scanCodeToKeyNum[idx];
}

static SDL_Scancode KeyNumToSDLScanCode( int keyNum )
{
    if( keyNum < K_JOY1 )
    {
        for( int i = 0; i < SDL_SCANCODE_COUNT; ++i )
        {
            if( scanCodeToKeyNum[i] == keyNum )
            {
                return SDL_Scancode( i );
            }
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

static void ConvertUTF8toUTF32( const char* utf8str, int32* utf32buf )
{
    static SDL_iconv_t cd = SDL_iconv_t( -1 );

    if( cd == SDL_iconv_t( -1 ) )
    {
        const char* toFormat = "UTF-32LE";
        cd = SDL_iconv_open( toFormat, "UTF-8" );
        if( cd == SDL_iconv_t( -1 ) )
        {
            common->Warning( "Couldn't initialize SDL_iconv for UTF-8 to UTF-32!" );
            return;
        }
    }

    size_t len = strlen( utf8str );
    size_t inbytesleft = len;
    size_t outbytesleft = 4 * len;
    char* outbuf = ( char* )utf32buf;
    size_t n = SDL_iconv( cd, &utf8str, &inbytesleft, &outbuf, &outbytesleft );

    if( n == size_t( -1 ) )
    {
        common->Warning( "Converting UTF-8 string \"%s\" from SDL_TEXTINPUT to UTF-32 failed!", utf8str );
        memset( utf32buf, 0, len * sizeof( int32 ) );
    }

    SDL_iconv( cd, NULL, &inbytesleft, NULL, &outbytesleft );
}

static void PushConsoleEvent( const char* s )
{
    char* b;
    size_t len;

    len = strlen( s ) + 1;
    b = ( char* )Mem_Alloc( len, TAG_EVENTS );
    strcpy( b, s );

    SDL_Event event;
    event.type = SDL_EVENT_USER;
    event.user.code = SE_CONSOLE;
    event.user.data1 = ( void* )len;
    event.user.data2 = b;

    SDL_PushEvent( &event );
}

/*
=================
Sys_InitInput
=================
*/
void Sys_InitInput()
{
    kbd_polls.SetGranularity( 256 );
    mouse_polls.SetGranularity( 256 );

    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        memset( &buttonStates[i], 0, sizeof( buttonStates[i] ) );
        memset( &joyAxis[i], 0, sizeof( joyAxis[i] ) );
    }
    memset( &current, 0, sizeof(joyState) );
    memset( &old, 0, sizeof(joyState) );
    memset( &joystick_polls, 0, sizeof(joystick_polls) );

    in_keyboard.SetModified();
#ifndef ANDROID
    joyThread = SDL_CreateThread((SDL_ThreadFunction)JoystickSamplingThread, "Joystic", NULL);
#endif
    Sys_ClearEvents();
}

/*
=================
Sys_ShutdownInput
=================
*/
void Sys_ShutdownInput()
{
    kbd_polls.Clear();
    mouse_polls.Clear();
    joyThreadKill = true;
#ifndef ANDROID
    SDL_WaitThread(joyThread, NULL);
#endif
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        memset( &buttonStates[i], 0, sizeof( buttonStates[i] ) );
        memset( &joyAxis[i], 0, sizeof( joyAxis[i] ) );
    }
    memset( &current, 0, sizeof(joyState) );
    memset( &old, 0, sizeof(joyState) );
    memset( &joystick_polls, 0, sizeof(joystick_polls) );
}

/*
===========
Sys_InitScanTable
===========
*/
#ifndef _WIN32
void Sys_InitScanTable()
{
}
#endif

/*
===============
Sys_GetConsoleKey
===============
*/
unsigned char Sys_GetConsoleKey( bool shifted )
{
    static unsigned char keys[2] = { '`', '~' };

    if( in_keyboard.IsModified() )
    {
        idStr lang = in_keyboard.GetString();

        if( lang.Length() )
        {
            if( !lang.Icmp( "french" ) )
            {
                keys[0] = '<';
                keys[1] = '>';
            }
            else if( !lang.Icmp( "german" ) )
            {
                keys[0] = '^';
                keys[1] = 176; // °
            }
            else if( !lang.Icmp( "italian" ) )
            {
                keys[0] = '\\';
                keys[1] = '|';
            }
            else if( !lang.Icmp( "spanish" ) )
            {
                keys[0] = 186; // º
                keys[1] = 170; // ª
            }
            else if( !lang.Icmp( "turkish" ) )
            {
                keys[0] = '"';
                keys[1] = 233; // é
            }
            else if( !lang.Icmp( "norwegian" ) )
            {
                keys[0] = 124; // |
                keys[1] = 167; // §
            }
        }

        in_keyboard.ClearModified();
    }

    return shifted ? keys[1] : keys[0];
}

/*
===============
Sys_MapCharForKey
===============
*/
unsigned char Sys_MapCharForKey( int key )
{
    return key & 0xff;
}

/*
===============
Sys_GrabMouseCursor
===============
*/
void Sys_GrabMouseCursor( bool grabIt )
{
    int flags;
    if( grabIt )
    {
        flags = GRAB_ENABLE | GRAB_SETSTATE;
    }
    else
    {
        flags = GRAB_SETSTATE;
    }
    GLimp_GrabInput( flags );
}

static std::map<SDL_JoystickID, int> reverseControllerMap;

/*
================
Sys_QueEvent
================
*/
void Sys_QueEvent( sysEventType_t type, int value, int value2, int ptrLength, void *ptr, int inputDeviceNum )
{
    sysEvent_t * ev = &eventQue[ eventHead & MASK_QUED_EVENTS ];

    if ( eventHead - eventTail >= MAX_QUED_EVENTS ) {
        common->Printf("Sys_QueEvent: overflow\n");
        if ( ev->evPtr ) {
            Mem_Free( ev->evPtr );
        }
        eventTail++;
    }

    eventHead++;

    ev->evType = type;
    ev->evValue = value;
    ev->evValue2 = value2;
    ev->evPtrLength = ptrLength;
    ev->evPtr = ptr;
    ev->inputDevice = inputDeviceNum;
}

int SDL_DisplayIDToIndex(SDL_DisplayID displayId) {
    int displayCount = -1;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
    if (displayCount > 0) {
        for (int i = 0; i < displayCount; i++) {
            if (displays[i] == displayId) {
                SDL_free(displays);
                return i + 1;
            }
        }
    }
    SDL_free(displays);
    return displayId;
}

static int32 uniStr[512] = {0};
static size_t uniStrPos = 0;
static int mwheelRel = 0;
static int32 uniChar = 0;

#ifdef ANDROID
extern void SetMute(bool mute);
#endif

static void ResumeGame()
{
    if (cvarSystem == nullptr || soundSystem == nullptr){
        return;
    }

    Sys_ClearEvents();
    SDL_Keymod currentmod = SDL_GetModState();
    int newmod = SDL_KMOD_NONE;
    if (currentmod & SDL_KMOD_CAPS)
        newmod |= SDL_KMOD_CAPS;
    SDL_SetModState((SDL_Keymod)newmod);

    kbd_polls.SetNum(0);
    mouse_polls.SetNum(0);
    memset( &joystick_polls, 0, sizeof(joystick_polls) );
    uniStr[0] = 0;
    uniStrPos = 0;
    uniChar = 0;
    mwheelRel = 0;

#if ANDROID
    SetMute(false);
#else
    soundSystem->SetMute(false);
#endif
    cvarSystem->SetCVarBool("com_pausePlatform", false);
    cvarSystem->SetCVarBool("com_pause", false);
}

static void PauseGame()
{
    if (cvarSystem == nullptr || soundSystem == nullptr)
        return;
#if ANDROID
    SetMute(true);
#else
    soundSystem->SetMute(true);
#endif
    cvarSystem->SetCVarBool("com_pausePlatform", true);
    cvarSystem->SetCVarBool("com_pause", true);
}

void SDL_Poll()
{
    sysEvent_t res = { };
    SDL_Event ev;
    int key;
    sys_jEvents joyEvent;
    static const sysEvent_t no_more_events = { SE_NONE, 0, 0, 0, NULL };
    static int previous_hat_state = SDL_HAT_CENTERED;

    const bool acceptInput = (cvarSystem != NULL && !cvarSystem->GetCVarBool("com_pausePlatform"));

    if( uniStr[0] != 0 )
    {
        if( acceptInput )
        {
            Sys_QueEvent(SE_CHAR, uniStr[uniStrPos], 1, 0, NULL, 0);
        }
        ++uniStrPos;
        if( !uniStr[uniStrPos] || uniStrPos == 512 )
        {
            memset( uniStr, 0, sizeof( uniStr ) );
            uniStrPos = 0;
        }
    }
    if( uniChar )
    {
        if( acceptInput )
        {
            Sys_QueEvent(SE_CHAR, uniChar, 1, 0, NULL, 0);
        }
        uniChar = 0;
    }

    if( mwheelRel )
    {
        if( acceptInput )
        {
            Sys_QueEvent(SE_KEY, mwheelRel, 0, 0, NULL, 0);
        }
        mwheelRel = 0;
    }

    if (console->Active()) {
        if (!SDL_TextInputActive(window)) {
            if (!SDL_StartTextInput(window)) {
                common->Warning("Error initializing text input: %s\n", SDL_GetError());
            }
        }
    }
    else {
        if (SDL_TextInputActive(window)) {
            SDL_StopTextInput(window);
        }
    }

    while ( SDL_PollEvent( &ev ) )
    {
        switch (ev.type)
        {
#ifndef ANDROID
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
				ResumeGame();
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				SDL_MinimizeWindow(window);
				PauseGame();
				break;
#endif
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                if( acceptInput )
                    Sys_QueEvent(SE_MOUSE_LEAVE, 0, 0, 0, NULL, 0);
                break;

#ifndef ANDROID
                case SDL_EVENT_WINDOW_RESIZED:
#else
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
#endif
            {
                int w = ev.window.data1;
                int h = ev.window.data2;
                r_windowWidth.SetInteger(w);
                r_windowHeight.SetInteger(h);
#if ANDROID
                r_customWidth.SetInteger(w);
                r_customHeight.SetInteger(h);
#endif
                glConfig.nativeScreenWidth = w;
                glConfig.nativeScreenHeight = h;
                cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "vid_restart\n");
                break;
            }
#ifndef ANDROID
                case SDL_EVENT_WINDOW_MOVED:
			{
				int x = ev.window.data1;
				int y = ev.window.data2;
				r_windowX.SetInteger(x);
				r_windowY.SetInteger(y);
				cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "vid_restart\n");
				break;
			}
#endif
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                com_emergencyexit.SetBool(true);
                soundSystem->SetMute(true);
                cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "quit\n");
                break;

            case SDL_EVENT_KEY_DOWN:
#ifndef ANDROID
                if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT) > 0)
				{
					int fullscreen = 0;
					if (!renderSystem->IsFullScreen())
					{
						SDL_Rect* windowRect = new SDL_Rect();
						windowRect->x = r_windowX.GetInteger();
						windowRect->y = r_windowY.GetInteger();
						windowRect->w = r_windowWidth.GetInteger();
						windowRect->h = r_windowHeight.GetInteger();
						fullscreen = SDL_GetDisplayForRect(windowRect);
						fullscreen = SDL_DisplayIDToIndex(fullscreen);
						delete(windowRect);
						if (fullscreen <= 0) {
							common->Printf("SDL3: Failed to detect Display index from Window with error message: %s\n", SDL_GetError());
						}
					}
					cvarSystem->SetCVarInteger("r_fullscreen", fullscreen);
					cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "vid_restart\n");
					continue;
				}
#endif
                if (ev.key.key == SDLK_G && (ev.key.mod & SDL_KMOD_CTRL) > 0)
                {
                    bool grab = cvarSystem->GetCVarBool("in_nograb");
                    grab = !grab;
                    cvarSystem->SetCVarBool("in_nograb", grab);
                    continue;
                }
                // fall through
            case SDL_EVENT_KEY_UP:
            {
                bool isChar;
                if (ev.key.scancode == SDL_SCANCODE_GRAVE)
                {
                    key = K_GRAVE;
                    uniChar = K_BACKSPACE;
                }
                else
                {
                    key = SDLScanCodeToKeyNum(ev.key.scancode);
                    if (key == 0)
                    {
                        if (ev.type == SDL_EVENT_KEY_DOWN)
                            common->Warning("unmapped SDL key %d scancode %d", ev.key.key, ev.key.scancode);
                        continue;
                    }
                }

                kbd_polls.Append(kbd_poll_t(key, ev.key.down == true));

                if (key == K_BACKSPACE && ev.key.down == true)
                    uniChar = key;

                if( acceptInput )
                    Sys_QueEvent(SE_KEY, key, (ev.key.down == true ? 1 : 0), 0, NULL, 0);
            }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (ev.text.text[0] != '\0')
                {
                    ConvertUTF8toUTF32(ev.text.text, uniStr);

                    if( acceptInput )
                    {
                        if (Sys_GetConsoleKey(false) == uniStr[0] || Sys_GetConsoleKey(true) == uniStr[0])
                            Sys_QueEvent(SE_KEY, K_GRAVE, 1, 0, NULL, 0);
                        else
                            Sys_QueEvent(SE_CHAR, uniStr[0], 1, 0, NULL, 0);
                    }
                    uniStrPos = 1;
                    if (uniStr[1] == 0)
                    {
                        uniStr[0] = 0;
                        uniStrPos = 0;
                    }
                    break;
                }
                continue;

            case SDL_EVENT_MOUSE_MOTION:
                if( acceptInput )
                {
#ifndef ANDROID
                    if (game && game->Shell_IsActive())
#else
                    if (game && (game->Shell_IsActive() || game->IsPDAOpen()))
#endif
                    {
                        Sys_QueEvent(SE_MOUSE_ABSOLUTE, ev.motion.x, ev.motion.y, 0, NULL, 0);
                    }
                    else
                    {
                        Sys_QueEvent(SE_MOUSE, ev.motion.xrel, ev.motion.yrel, 0, NULL, 0);
                    }
                    mouse_polls.Append(mouse_poll_t(M_DELTAX, ev.motion.xrel));
                    mouse_polls.Append(mouse_poll_t(M_DELTAY, ev.motion.yrel));
                }
                break;

            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
                continue;

            case SDL_EVENT_MOUSE_WHEEL:
                if( acceptInput )
                {
                    mouse_polls.Append(mouse_poll_t(M_DELTAZ, ev.wheel.y));
                    Sys_QueEvent(SE_KEY, (ev.wheel.y > 0) ? K_MWHEELUP : K_MWHEELDOWN, 1, 0, NULL, 0);
                    mwheelRel = (ev.wheel.y > 0) ? K_MWHEELUP : K_MWHEELDOWN;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                res.evType = SE_KEY;
                switch (ev.button.button)
                {
                    case SDL_BUTTON_LEFT:   res.evValue = K_MOUSE1; mouse_polls.Append(mouse_poll_t(M_ACTION1, ev.button.down == true ? 1 : 0)); break;
                    case SDL_BUTTON_MIDDLE: res.evValue = K_MOUSE3; mouse_polls.Append(mouse_poll_t(M_ACTION3, ev.button.down == true ? 1 : 0)); break;
                    case SDL_BUTTON_RIGHT:  res.evValue = K_MOUSE2; mouse_polls.Append(mouse_poll_t(M_ACTION2, ev.button.down == true ? 1 : 0)); break;
                    default:
                        if (ev.button.button <= 16)
                        {
                            int buttonIndex = ev.button.button - SDL_BUTTON_LEFT;
                            res.evValue = K_MOUSE1 + buttonIndex;
                            mouse_polls.Append(mouse_poll_t(M_ACTION1 + buttonIndex, ev.button.down == true ? 1 : 0));
                            break;
                        }
                        else continue;
                }
                res.evValue2 = ev.button.down == true ? 1 : 0;
                if( acceptInput )
                    Sys_QueEvent(res.evType, res.evValue, res.evValue2, 0, NULL, 0);
                break;

#if ANDROID
            case SDL_EVENT_JOYSTICK_ADDED:
            case SDL_EVENT_JOYSTICK_REMOVED:
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMAPPED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                ReconnectGamepads();
                break;
#endif
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
#ifndef ANDROID
                case SDL_EVENT_JOYSTICK_ADDED:
		case SDL_EVENT_JOYSTICK_REMOVED:
#endif
            case SDL_EVENT_JOYSTICK_UPDATE_COMPLETE:
            case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
                continue;

            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                joyEvent = (sys_jEvents)(J_AXIS_LEFT_X + ev.gaxis.axis);
                switch(joyEvent) {
                    case J_AXIS_LEFT_X:  current[reverseControllerMap[ev.gaxis.which]].LXThumb = ev.gaxis.value; break;
                    case J_AXIS_LEFT_Y:  current[reverseControllerMap[ev.gaxis.which]].LYThumb = ev.gaxis.value; break;
                    case J_AXIS_RIGHT_X: current[reverseControllerMap[ev.gaxis.which]].RXThumb = ev.gaxis.value; break;
                    case J_AXIS_RIGHT_Y: current[reverseControllerMap[ev.gaxis.which]].RYThumb = ev.gaxis.value; break;
                    case J_AXIS_LEFT_TRIG:  current[reverseControllerMap[ev.gaxis.which]].LTrigger = ev.gaxis.value; break;
                    case J_AXIS_RIGHT_TRIG: current[reverseControllerMap[ev.gaxis.which]].RTrigger = ev.gaxis.value; break;
                }
                break;

            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                current[reverseControllerMap[ev.gbutton.which]].buttons[ev.gbutton.button] = (ev.gbutton.down == true ? 1 : 0);
                break;

            case SDL_EVENT_QUIT:
                PushConsoleEvent( "quit" );
                Sys_QueEvent(SE_NONE, 0, 0, 0, NULL, 0);
                return;

            case SDL_EVENT_USER:
                switch( ev.user.code )
                {
                    case SE_CONSOLE:
                        Sys_QueEvent(SE_CONSOLE, 0, 0, ( intptr_t )ev.user.data1, ev.user.data2, 0);
                        break;
                    default:
                        common->Warning( "unknown user event %u", ev.user.code );
                }
                continue;

            default:
                continue;
        }
    }
    Sys_QueEvent(SE_NONE, 0, 0, 0, NULL, 0);
}

/*
================
Sys_GetEvent
================
*/
sysEvent_t Sys_GetEvent() {
    sysEvent_t	ev;
    if ( eventHead > eventTail ) {
        eventTail++;
        return eventQue[ ( eventTail - 1 ) & MASK_QUED_EVENTS ];
    }
    memset( &ev, 0, sizeof( ev ) );
    return ev;
}

/*
================
Sys_ClearEvents
================
*/
void Sys_ClearEvents()
{
    eventHead = eventTail = 0;
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    kbd_polls.SetNum(0);
    mouse_polls.SetNum(0);
    memset( &joystick_polls, 0, sizeof(joystick_polls) );
    memset( &current, 0, sizeof(current) );
    uniStr[0] = 0;
    uniStrPos = 0;
    uniChar = 0;
    mwheelRel = 0;
}

/*
================
Sys_GenerateEvents
================
*/
void Sys_GenerateEvents()
{
    char* s = Posix_ConsoleInput();
    if( s )
        PushConsoleEvent( s );
    SDL_Poll();
}

/*
================
Sys_PollKeyboardInputEvents
================
*/
int Sys_PollKeyboardInputEvents()
{
    return kbd_polls.Num();
}

int Sys_ReturnKeyboardInputEvent( const int n, int& key, bool& state )
{
    if( n >= kbd_polls.Num() )
        return 0;
    key = kbd_polls[n].key;
    state = kbd_polls[n].state;
    return 1;
}

void Sys_EndKeyboardInputEvents()
{
    kbd_polls.SetNum( 0 );
}

int Sys_PollMouseInputEvents( int mouseEvents[MAX_MOUSE_EVENTS][2] )
{
    int numEvents = mouse_polls.Num();
    if( numEvents > MAX_MOUSE_EVENTS )
        numEvents = MAX_MOUSE_EVENTS;
    for( int i = 0; i < numEvents; i++ )
    {
        const mouse_poll_t& mp = mouse_polls[i];
        mouseEvents[i][0] = mp.action;
        mouseEvents[i][1] = mp.value;
    }
    mouse_polls.SetNum( 0 );
    return numEvents;
}

const char* Sys_GetKeyName( keyNum_t keynum )
{
    SDL_Scancode scancode = KeyNumToSDLScanCode( ( int )keynum );
    SDL_Keycode keycode = SDL_GetKeyFromScancode( scancode, SDL_KMOD_NONE, true );
    const char* ret = SDL_GetKeyName( keycode );
    if( ret != NULL && ret[0] != '\0' )
        return ret;
    return NULL;
}

char* Sys_GetClipboardData()
{
    char* txt = SDL_GetClipboardText();
    if( txt == NULL || txt[0] == '\0' )
    {
        if( txt ) SDL_free( txt );
        return NULL;
    }
    char* ret = Mem_CopyString( txt );
    SDL_free( txt );
    return ret;
}

void Sys_SetClipboardData( const char* string )
{
    SDL_SetClipboardText( string );
}

void Sys_SetRumble( int device, int low, int hi )
{
    if (gcontroller[device] != nullptr) {
        SDL_RumbleGamepad(gcontroller[device], idMath::ClampInt( 0, 65535, low ), idMath::ClampInt( 0, 65535, hi ), 1000);
    }
}

void PushButton( int inputDeviceNum, int key, bool value )
{
    if( buttonStates[inputDeviceNum][key] != value )
    {
        buttonStates[inputDeviceNum][key] = value;
        Sys_QueEvent( SE_KEY, key, value, 0, NULL, inputDeviceNum );
    }
}

void PostInputEvent( int inputDeviceNum, int event, int value, int range = 16384 )
{
    if( ( event >= J_ACTION1 ) && ( event <= J_ACTION_MAX ) )
    {
        PushButton( inputDeviceNum, K_JOY1 + ( event - J_ACTION1 ), value != 0 );
    }
    else if( event == J_AXIS_LEFT_X )
    {
        PushButton( inputDeviceNum, K_JOY_STICK1_LEFT, ( value < -range ) );
        PushButton( inputDeviceNum, K_JOY_STICK1_RIGHT, ( value > range ) );
    }
    else if( event == J_AXIS_LEFT_Y )
    {
        PushButton( inputDeviceNum, K_JOY_STICK1_UP, ( value < -range ) );
        PushButton( inputDeviceNum, K_JOY_STICK1_DOWN, ( value > range ) );
    }
    else if( event == J_AXIS_RIGHT_X )
    {
        PushButton( inputDeviceNum, K_JOY_STICK2_LEFT, ( value < -range ) );
        PushButton( inputDeviceNum, K_JOY_STICK2_RIGHT, ( value > range ) );
    }
    else if( event == J_AXIS_RIGHT_Y )
    {
        PushButton( inputDeviceNum, K_JOY_STICK2_UP, ( value < -range ) );
        PushButton( inputDeviceNum, K_JOY_STICK2_DOWN, ( value > range ) );
    }
    else if( ( event >= J_DPAD_UP ) && ( event <= J_DPAD_RIGHT ) )
    {
        PushButton( inputDeviceNum, K_JOY_DPAD_UP + ( event - J_DPAD_UP ), value != 0 );
    }
    else if( event == J_AXIS_LEFT_TRIG )
    {
        PushButton( inputDeviceNum, K_JOY_TRIGGER1, ( value > range ) );
    }
    else if( event == J_AXIS_RIGHT_TRIG )
    {
        PushButton( inputDeviceNum, K_JOY_TRIGGER2, ( value > range ) );
    }
    if( event >= J_AXIS_MIN && event <= J_AXIS_MAX )
    {
        int axis = event - J_AXIS_MIN;
        int percent = ( value * 16 ) / range;
        if( joyAxis[inputDeviceNum][axis] != percent )
        {
            joyAxis[inputDeviceNum][axis] = percent;
            Sys_QueEvent( SE_JOYSTICK, axis, percent, 0, NULL, inputDeviceNum );
        }
    }

    joystick_polls[numEvents].action = event;
    joystick_polls[numEvents].value = value;
    numEvents++;
}

int Sys_PollJoystickInputEvents( int deviceNum )
{
    numEvents = 0;
    int controllerButtonRemap[15] =
            {
                    J_ACTION1, J_ACTION2, J_ACTION3, J_ACTION4,
                    J_ACTION10, J_ACTION11, J_ACTION9, J_ACTION7, J_ACTION8,
                    J_ACTION5, J_ACTION6,
                    J_DPAD_UP, J_DPAD_DOWN, J_DPAD_LEFT, J_DPAD_RIGHT
            };

    for (int i = 0; i < 15; i++) {
        if (current[deviceNum].buttons[i] != old[deviceNum].buttons[i]) {
            PostInputEvent(deviceNum, controllerButtonRemap[i], current[deviceNum].buttons[i]);
        }
    }
    if (current[deviceNum].LXThumb != old[deviceNum].LXThumb)
        PostInputEvent(deviceNum, J_AXIS_LEFT_X, current[deviceNum].LXThumb);
    if (current[deviceNum].LYThumb != old[deviceNum].LYThumb)
        PostInputEvent(deviceNum, J_AXIS_LEFT_Y, current[deviceNum].LYThumb);
    if (current[deviceNum].RXThumb != old[deviceNum].RXThumb)
        PostInputEvent(deviceNum, J_AXIS_RIGHT_X, current[deviceNum].RXThumb);
    if (current[deviceNum].RYThumb != old[deviceNum].RYThumb)
        PostInputEvent(deviceNum, J_AXIS_RIGHT_Y, current[deviceNum].RYThumb);
    if (current[deviceNum].LTrigger != old[deviceNum].LTrigger)
        PostInputEvent(deviceNum, J_AXIS_LEFT_TRIG, current[deviceNum].LTrigger);
    if (current[deviceNum].RTrigger != old[deviceNum].RTrigger)
        PostInputEvent(deviceNum, J_AXIS_RIGHT_TRIG, current[deviceNum].RTrigger);

    old[deviceNum] = current[deviceNum];
    return numEvents;
}

int Sys_ReturnJoystickInputEvent( const int n, int& action, int& value )
{
    if( ( n < 0 ) || ( n >= MAX_JOY_EVENT ) )
        return 0;
    const joystick_poll_t& mp = joystick_polls[n];
    action = mp.action;
    value = mp.value;
    return 1;
}

void Sys_EndJoystickInputEvents()
{
}

bool Sys_hasConnectedController() {
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (gcontroller[i])
            return true;
    }
    return false;
}

//GK: This is the controller state detection thread. It can check whenever a controller is connected or not.
//Most of the console outputs have been disabled since this thread runs from the begining of the game and never stops.
static int	threadTimeDeltas[256];
static int	threadPacket[256];
static int	threadCount;
static int	defaultAvailable;

#if ANDROID
static int virtualControllerIndex = -1;

static void CloseGamepads(){
	for (uint32 i = 0; i < MAX_JOYSTICKS; i++) {
		const auto controller = gcontroller[i];
		if (controller == nullptr){
			continue;
		}
		const auto controllerId = SDL_GetGamepadID (controller);
		if (!SDL_IsJoystickVirtual(controllerId)){
			SDL_CloseGamepad( controller );
			gcontroller[i] = nullptr;
		}
	}
}

void ReconnectGamepads() {
    SDL_UpdateGamepads();
    reverseControllerMap.clear();
    if (virtualControllerIndex != -1) {
		CloseGamepads();
        return;
    }
    int count = 0;
    SDL_JoystickID *controllers = SDL_GetGamepads(&count);
    for (uint32 i = 0; i < count; i++) {
        if (SDL_IsJoystickVirtual(controllers[i])) {
            virtualControllerIndex = i;
            break;
        }
    }

	CloseGamepads();

    if (virtualControllerIndex != -1) {
        gcontroller[0] = SDL_OpenGamepad(controllers[virtualControllerIndex]);
        return;
    }

	int emptyControllerId = -1;

	for (uint32 i = 0; i < MAX_JOYSTICKS; i++) {
		if (gcontroller[i] == nullptr){
			emptyControllerId = i;
			break;
		}
	}

	if (emptyControllerId!=-1){
		for (uint32 i = 0; i < count; i++) {
            const auto controllerId = controllers[i];
            const auto controller = SDL_OpenGamepad(controllerId);
			if (controller != nullptr){
				gcontroller[emptyControllerId] = controller;
                reverseControllerMap.insert(std::make_pair(controllerId, i));
                if (session->GetSignInManager().GetMasterLocalUser() != NULL) {
                    idLocalUserWin *user = dynamic_cast<idLocalUserWin *>(session->GetSignInManager().GetMasterLocalUser());
                    user->SetInputDevice(controllerId);
                }
				break;
			}
		}
	}
}
#endif

void JoystickSamplingThread(void* data){

	static int prevTime = 0;
#if ANDROID
	static int virtualControllerIndex = -1;
#endif
	static uint64 nextCheck[MAX_JOYSTICKS] = { 0 };
	const uint64 waitTime = 5000;// 000; // poll every 5 seconds to see if a controller was connected
	while(1){
		if (joyThreadKill) {
			for(int i = 0; i < MAX_JOYSTICKS; i++) {
				if(gcontroller[i]){
					SDL_CloseGamepad(gcontroller[i]);
				}
				gcontroller[i]=NULL;
			}
			break;
		}
		int	now = Sys_Microseconds();
		int	delta;
		if( prevTime == 0 )
		{
			delta = 4000;
		}
		else
		{
			delta = now - prevTime;
		}
		prevTime = now;
		threadTimeDeltas[threadCount & 255] = delta;
		threadCount++;
		if(now>=nextCheck[0]){ //GK: Similar to the windows thread
	SDL_Gamepad* controller = NULL;
	int inactive = 0;
	int available = -1;
	bool alreadyConnected = false;
	int count = 0;
	SDL_JoystickID* controllers = SDL_GetGamepads(&count);
	SDL_free(controllers);
	controllers = SDL_GetGamepads(&count); //SDL 3 Weird quirk. It needs to query joystick twice in order to work
	if (count == 0) {
		reverseControllerMap.clear();
		count = 4; //GK: Clean time
	}

#if ANDROID
	if (virtualControllerIndex == -1) {
		for (uint32 i = 0; i < count; i++) {
			if (SDL_IsJoystickVirtual(controllers[i])) {
				virtualControllerIndex = i;
				break;
			}
		}

		if (virtualControllerIndex != -1) {
			for (uint32 i = 0; i < count; i++) {
				if (gcontroller[i]!= nullptr){
					SDL_CloseGamepad( gcontroller[i] );
					gcontroller[i] = nullptr;
				}
			}
			gcontroller[0] = SDL_OpenGamepad(controllers[virtualControllerIndex]);
		}

        if (virtualControllerIndex != -1){
            SDL_free(controllers);
            return;
        }
	}
#endif

	for( uint32 i = 0; i < count; i++ )
	{
		if( SDL_IsGamepad( controllers[i] ) )
		{
			if (!gcontroller[i]) {
				controller = SDL_OpenGamepad( controllers[i] );
				if( controller )
				{
					if (available < 0) {
						available = i;
					}
					reverseControllerMap.insert(std::make_pair(controllers[i], i));
					nextCheck[0]=0; //GK: Like the Windows thread constantly checking for the controller state once it's connected
//					idLib::Printf("Controller Connected: %s\n", SDL_GetGamepadName(controller));
					gcontroller[i]=controller;					
				} else {
//					common->Warning("Error Initializing controller: %s\n", SDL_GetError());
				}
			} else {
				alreadyConnected = true;
				continue;
				//common->Printf( "GameController %i name: %s\n", i, SDL_GameControllerName( controller ) );
				//common->Printf( "GameController %i is mapped as \"%s\".\n", i, SDL_GameControllerMapping( controller ) );
			}
		}else{
			inactive++;
					if(gcontroller[i]){
						SDL_CloseGamepad(gcontroller[i]);
					}
					gcontroller[i]=NULL;
					nextCheck[0] = now + waitTime;
					continue;
		}
	}
	SDL_free(controllers);
	if (!alreadyConnected) {
		//GK: Enable controller layout if there is one controller connected
		registeredControllers = 4-inactive;
		if (registeredControllers > 0) {
			if (session->GetSignInManager().GetMasterLocalUser() != NULL) {
				idLocalUserWin* user = dynamic_cast<idLocalUserWin*>(session->GetSignInManager().GetMasterLocalUser());
				user->SetInputDevice(available);
			}
			else {
				defaultAvailable = available;
			}
		}
	}
	/*idLib::joystick = inactive >= 4 ? false : true;*/
		}else{
			continue;
		}
		SDL_Delay(10);
	}
}

#if ANDROID
extern "C"{
__attribute__((used)) __attribute__((visibility("default")))
void onNativeResume() {
	ResumeGame();
}

__attribute__((used)) __attribute__((visibility("default")))
void onNativePause() {
	PauseGame();
}

__attribute__((used)) __attribute__((visibility("default")))
bool needToInvokeMouseButtonsEvents(){
	return game!= nullptr && (game->Shell_IsActive() || game->IsPDAOpen());
}

__attribute__((used)) __attribute__((visibility("default")))
bool needToShowScreenControls() {
    return !needToInvokeMouseButtonsEvents();
}
}
#endif