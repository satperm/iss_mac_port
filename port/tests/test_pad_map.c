/* TLG_PAD_MAP / TLG_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"
// runtime_mouse_aim.c (the native aim hook) needs the guest; pad tests run without it.
int runtime_mouse_aim_installed(void) { return 0; }
int runtime_mouse_aim_motion(float dx, float dy, float sensitivity, int invert_y, int active) { (void)dx; (void)dy; (void)sensitivity; (void)invert_y; (void)active; return 0; }

/* The GPU library (not linked here): the mouse and the input settings. */
void tlggpu_mouse_take(TlgGpuMouse *mouse) { memset(mouse,0,sizeof(*mouse)); }
void tlggpu_mouse_pad_hud(int a, float x, float y, int t, int c) { (void)a; (void)x; (void)y; (void)t; (void)c; }
void tlggpu_input_settings(TlgGpuInputSettings *settings) { memset(settings,0,sizeof(*settings)); }
uint64_t tlggpu_total_draws(void) { return 0; }
void tlggpu_frame_counters(TlgGpuFrameCounters *out) { memset(out,0,sizeof(*out)); }
int tlggpu_overlay_captures_input(void) { return 0; }
const bool *tlggpu_split_keyboard(void) { return NULL; } /* single process: the SDL keyboard */
void tlggpu_screenshot(void) {}
void tlggpu_overlay_notice(const char *text, float seconds) { (void)text; (void)seconds; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"touchpad_right=guide,touchpad_left=paddle1",false)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */
    /* Keyboard: each side has a key of its own. */
    assert(pressed_by(&key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=key_map;
    assert(apply_button_map(&sides,"touchpad_right=Delete",true)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);

    /* Actions: the defaults are the old hard-coded keys. */
    static bool kb[SDL_SCANCODE_COUNT];
    static const struct { int key; int action; } old_keys[]={
        {SDL_SCANCODE_W,ACT_MOVE_UP}, {SDL_SCANCODE_S,ACT_MOVE_DOWN}, {SDL_SCANCODE_A,ACT_MOVE_LEFT},
        {SDL_SCANCODE_D,ACT_MOVE_RIGHT}, {SDL_SCANCODE_UP,ACT_LOOK_UP}, {SDL_SCANCODE_DOWN,ACT_LOOK_DOWN},
        {SDL_SCANCODE_LEFT,ACT_LOOK_LEFT}, {SDL_SCANCODE_RIGHT,ACT_LOOK_RIGHT}, {SDL_SCANCODE_T,ACT_CAN_LEFT},
        {SDL_SCANCODE_G,ACT_CAN_RIGHT}, {SDL_SCANCODE_Y,ACT_CAN_SHAKE}, {SDL_SCANCODE_TAB,ACT_SWIPE},
    };
    assert(action_bits(&key_map,kb,0)==0);
    for (size_t i=0;i<sizeof(old_keys)/sizeof(*old_keys);++i) {
        kb[old_keys[i].key]=true;
        assert(action_bits(&key_map,kb,0)==1u<<old_keys[i].action);
        kb[old_keys[i].key]=false;
    }
    assert(action_bits(&key_map,kb,1u<<(MOUSE_LEFT-MOUSE_SOURCE))==0); /* mouse left is R2 only */
    assert(key_map.count==31 && !key_map.swipe_set);

    /* Each action can be remapped; it loses its default; buttons are untouched. */
    for (size_t i=0;i<sizeof(action_names)/sizeof(*action_names);++i) {
        ButtonMap m=key_map;
        char spec[64];
        snprintf(spec,sizeof(spec),"%s=F5",action_names[i].name);
        assert(apply_button_map(&m,spec,true)==1);
        kb[SDL_SCANCODE_F5]=true;
        assert(action_bits(&m,kb,0)==1u<<(action_names[i].ps & ~(uint32_t)ACTION_FLAG));
        kb[SDL_SCANCODE_F5]=false;
        assert(pressed_by(&m,SDL_SCANCODE_SPACE)==BTN_CROSS);
    }
    /* Several sources, a mouse button and the wheel, none. */
    ButtonMap act=key_map;
    assert(apply_button_map(&act,"move_up=Up|Mouse X1,can_shake=Wheel Up,can_left=none",true)==3);
    kb[SDL_SCANCODE_W]=true;
    assert(action_bits(&act,kb,0)==0);                       /* W lost move_up */
    kb[SDL_SCANCODE_W]=false;
    assert(action_bits(&act,kb,1u<<(MOUSE_X1-MOUSE_SOURCE))==1u<<ACT_MOVE_UP);
    assert(action_bits(&act,kb,1u<<(WHEEL_UP-MOUSE_SOURCE))==1u<<ACT_CAN_SHAKE);
    kb[SDL_SCANCODE_T]=true;
    assert(action_bits(&act,kb,0)==0);                       /* T no longer holds the can */
    kb[SDL_SCANCODE_T]=false;
    /* Actions are keyboard-only: not accepted in the gamepad map. */
    ButtonMap padact=pad_map;
    assert(apply_button_map(&padact,"move_up=a",false)==0);
    /* Unknown action or key names are rejected (a log line each) and change nothing. */
    ButtonMap unk=key_map;
    assert(apply_button_map(&unk,"move_forward=W,look_up=NotAKey,swipe=,can_shake=Bogus|Nope",true)==0);
    assert(unk.count==key_map.count && !unk.swipe_set);

    /* Swipe follows the left touchpad click unless it is set. */
    ButtonMap sw=key_map;
    assert(apply_button_map(&sw,"touchpad_left=Delete",true)==1);
    kb[SDL_SCANCODE_TAB]=true;
    assert(action_bits(&sw,kb,0)==0);
    kb[SDL_SCANCODE_TAB]=false; kb[SDL_SCANCODE_DELETE]=true;
    assert(action_bits(&sw,kb,0)==1u<<ACT_SWIPE);
    kb[SDL_SCANCODE_DELETE]=false;
    assert(apply_button_map(&sw,"swipe=Left Alt",true)==1 && sw.swipe_set);
    kb[SDL_SCANCODE_DELETE]=true;
    assert(action_bits(&sw,kb,0)==0);
    kb[SDL_SCANCODE_DELETE]=false; kb[SDL_SCANCODE_LALT]=true;
    assert(action_bits(&sw,kb,0)==1u<<ACT_SWIPE);
    kb[SDL_SCANCODE_LALT]=false;
    assert(apply_button_map(&sw,"swipe=none",true)==1);
    kb[SDL_SCANCODE_TAB]=true; kb[SDL_SCANCODE_DELETE]=true;
    assert(action_bits(&sw,kb,0)==0);
    kb[SDL_SCANCODE_TAB]=false; kb[SDL_SCANCODE_DELETE]=false;

    /* A long map (every button and action with three sources, well over the old 1024 bytes) parses. */
    char big[4096]="";
    int entries=0;
    for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) {
        if (!strcmp(ps_names[i].name,"touchpad")) continue;
        size_t n=strlen(big);
        snprintf(big+n,sizeof(big)-n,"%s%s=Keypad 1|Keypad 2|Mouse Left",entries++ ? "," : "",ps_names[i].name);
    }
    for (size_t i=0;i<sizeof(action_names)/sizeof(*action_names);++i) {
        size_t n=strlen(big);
        snprintf(big+n,sizeof(big)-n,",%s=Keypad 3|Keypad 4|Wheel Down",action_names[i].name);
        ++entries;
    }
    assert(strlen(big)>1024 && strlen(big)<4096);
    ButtonMap lng=key_map;
    assert(apply_button_map(&lng,big,true)==entries);
    kb[SDL_SCANCODE_KP_4]=true;
    assert(action_bits(&lng,kb,0)==((1u<<ACT_COUNT)-1));
    kb[SDL_SCANCODE_KP_4]=false;
    puts("PASS: pad and key maps");
}
