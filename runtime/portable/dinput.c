/* DirectInput 8 for 64-bit hosts (R3.1), over input.c (SDL3).
 *
 * FFXiMain reads the keyboard, the mouse and gamepads through DirectInput, not window messages.
 * The devices here are the three kinds it can create: the system keyboard (DIK-indexed state and
 * buffered key events), the system mouse (relative motion, buttons) and one joystick per SDL
 * gamepad. A device's data format (SetDataFormat) is honoured object by object, so the game's own
 * c_dfDIKeyboard / c_dfDIMouse / c_dfDIJoystick(2) - compiled into it from dinput8.lib - lay out
 * the state exactly as they would on Windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dinput.h"
#include "gthread.h"
#include "gwin.h"
#include "input.h"
#include "kobj.h"
#include "thunk.h"

#define DI_OK 0u
#define DI_NOEFFECT 1u
#define DI_BUFFEROVERFLOW 1u
#define S_FALSE 1u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_NOTBUFFERED 0x80040207u
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_NOAGGREGATION 0x80040110u
#define DIERR_NOINTERFACE 0x80004002u
#define DIERR_OBJECTNOTFOUND 0x80070002u

enum
{
    D_KEYBOARD,
    D_MOUSE,
    D_PAD,
};

/* GUIDs, as bytes in memory */
#define GUIDB(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                                               \
    { (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24), (uint8_t)(b), (uint8_t)((b) >> 8), \
      (uint8_t)(c), (uint8_t)((c) >> 8), d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t GUID_SysMouse[16] = GUIDB(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_SysKeyboard[16] = GUIDB(0x6F1D2B61, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_XAxis[16] = GUIDB(0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_YAxis[16] = GUIDB(0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_ZAxis[16] = GUIDB(0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_RxAxis[16] = GUIDB(0xA36D02F4, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_RyAxis[16] = GUIDB(0xA36D02F5, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_Button[16] = GUIDB(0xA36D02F0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_POV[16] = GUIDB(0xA36D02F2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
/* our gamepads' instance GUIDs: "SIGN" and the index */
static void pad_guid(int i, uint8_t out[16])
{
    const uint8_t g[16] = GUIDB(0x4E474953, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)i);
    memcpy(out, g, 16);
}

/* the gamepad's objects: 5 axes, 1 POV, 10 buttons (the Xbox controller's DirectInput view) */
#define PAD_AXES 5
#define PAD_BUTTONS 10
static const uint8_t* const AXIS_GUIDS[PAD_AXES] = { GUID_XAxis, GUID_YAxis, GUID_ZAxis, GUID_RxAxis, GUID_RyAxis };
static const char* const AXIS_NAMES[PAD_AXES] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation" };

enum
{
    SRC_NONE,
    SRC_AXIS,
    SRC_POV,
    SRC_BUTTON,
};

typedef struct Map
{
    uint32_t ofs;
    uint8_t src, index;
} Map;

typedef struct Dev
{
    int used, kind, pad;
    int32_t refs;
    uint32_t guest;
    uint32_t data_size, nmap;
    Map map[256];
    int acquired;
    uint32_t buffer_size, cursor;
    uint32_t event;
    int32_t min[PAD_AXES], max[PAD_AXES];
    uint32_t deadzone[PAD_AXES];
    uint32_t last[256 / 4 + 64]; /* the gamepad's last formatted state, for buffered data */
    uint32_t pad_seq;
} Dev;

#define MAX_DEVS 16
static Dev g_devs[MAX_DEVS];
static uint32_t g_di, g_vtbl_di, g_vtbl_dev, g_vtbl_eff, g_effect;
static int32_t g_di_refs;

static Dev* dev(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < MAX_DEVS && g_devs[i].used && g_devs[i].guest == p ? &g_devs[i] : NULL;
}

static void notify(int kind)
{
    for (int i = 0; i < MAX_DEVS; ++i)
        if (g_devs[i].used && g_devs[i].event && g_devs[i].acquired &&
            ((kind == INPUT_KEYBOARD && g_devs[i].kind == D_KEYBOARD) || (kind == INPUT_MOUSE && g_devs[i].kind == D_MOUSE)))
            k_event_set(g_devs[i].event, 1);
}

/* --- IDirectInput8A ---------------------------------------------------------------------------------- */
static void DI_QueryInterface(Guest* g)
{
    wr32(ARG(2), 0);
    RET(DIERR_NOINTERFACE, 3);
}
static void DI_AddRef(Guest* g) { RET((uint32_t)++g_di_refs, 1); }
static void DI_Release(Guest* g) { RET((uint32_t)(g_di_refs > 0 ? --g_di_refs : 0), 1); }

/* DIDEVICEINSTANCEA (580 bytes) */
static void write_instance(uint32_t p, int kind, int pad)
{
    memset(GUEST_PTR(p + 4), 0, 576);
    uint8_t guid[16];
    const char* name;
    uint32_t type;
    if (kind == D_KEYBOARD)
        memcpy(guid, GUID_SysKeyboard, 16), name = "Keyboard", type = 0x0413; /* DI8DEVTYPE_KEYBOARD, PCENH */
    else if (kind == D_MOUSE)
        memcpy(guid, GUID_SysMouse, 16), name = "Mouse", type = 0x0112;       /* DI8DEVTYPE_MOUSE */
    else
    {
        pad_guid(pad, guid);
        name = input_pad_name(pad);
        name = name ? name : "Gamepad";
        type = 0x00010215; /* DI8DEVTYPE_GAMEPAD, standard, HID */
    }
    memcpy(GUEST_PTR(p + 4), guid, 16); /* guidInstance */
    if (kind == D_PAD)
    {
        /* an Xbox 360 controller's product GUID: Data1 is MAKELONG(VID 045E, PID 028E), "PIDVID" after.
         * With XInput on, the game's IsXInputDevice matches it to the gamepad ole.c's WMI lists, and
         * reads the pad through XInput instead. */
        static const uint8_t XBOX360[16] = GUIDB(0x028E045E, 0, 0, 0, 0, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44);
        memcpy(guid, XBOX360, 16);
    }
    memcpy(GUEST_PTR(p + 20), guid, 16); /* guidProduct */
    wr32(p + 36, type);
    snprintf((char*)GUEST_PTR(p + 40), 260, "%s", name);
    snprintf((char*)GUEST_PTR(p + 300), 260, "%s", name);
}

/* EnumDevices(dwDevType, lpCallback, pvRef, dwFlags): DI8DEVCLASS_ALL 0, DEVICE 1, POINTER 2,
 * KEYBOARD 3, GAMECTRL 4, or a DI8DEVTYPE_* */
static void DI_EnumDevices(Guest* g)
{
    uint32_t cls = ARG(1), cb = ARG(2), ref = ARG(3);
    int want[3] = { cls == 0 || cls == 3 || cls == 0x13, cls == 0 || cls == 2 || cls == 0x12,
                    cls == 0 || cls == 4 || (cls >= 0x14 && cls <= 0x18) };
    uint32_t inst = gheap_alloc(580, 1);
    wr32(inst, 580);
    int stop = 0;
    for (int k = 0; k < 2 && !stop; ++k)
        if (want[k])
        {
            write_instance(inst, k, 0);
            uint32_t a[2] = { inst, ref };
            stop = !guest_call(cb, 2, a);
        }
    for (int i = 0; want[2] && !stop && i < input_pad_count(); ++i)
    {
        write_instance(inst, D_PAD, i);
        uint32_t a[2] = { inst, ref };
        stop = !guest_call(cb, 2, a);
    }
    gheap_free(inst);
    RET(DI_OK, 5);
}

static uint32_t dev_new(int kind, int pad)
{
    for (int i = 0; i < MAX_DEVS; ++i)
        if (!g_devs[i].used)
        {
            Dev* d = &g_devs[i];
            memset(d, 0, sizeof *d);
            d->used = 1, d->kind = kind, d->pad = pad, d->refs = 1;
            for (int a = 0; a < PAD_AXES; ++a)
                d->min[a] = 0, d->max[a] = 65535;
            d->guest = gheap_alloc(8, 1);
            wr32(d->guest, g_vtbl_dev);
            wr32(d->guest + 4, (uint32_t)i);
            return d->guest;
        }
    return 0;
}

/* CreateDevice(rguid, lplpDirectInputDevice, pUnkOuter) */
static void DI_CreateDevice(Guest* g)
{
    wr32(ARG(2), 0);
    if (ARG(3))
        RET(DIERR_NOAGGREGATION, 4);
    const uint8_t* guid = (const uint8_t*)GUEST_PTR(ARG(1));
    int kind = -1, pad = 0;
    if (!memcmp(guid, GUID_SysKeyboard, 16))
        kind = D_KEYBOARD;
    else if (!memcmp(guid, GUID_SysMouse, 16))
        kind = D_MOUSE;
    else
        for (int i = 0; i < input_pad_count(); ++i)
        {
            uint8_t pg[16];
            pad_guid(i, pg);
            if (!memcmp(guid, pg, 16))
                kind = D_PAD, pad = i;
        }
    if (kind < 0)
        RET(DIERR_DEVICENOTREG, 4);
    uint32_t p = dev_new(kind, pad);
    wr32(ARG(2), p);
    RET(p ? DI_OK : DIERR_INVALIDPARAM, 4);
}

static void DI_GetDeviceStatus(Guest* g) { RET(DI_OK, 2); }
static void DI_RunControlPanel(Guest* g) { RET(DI_OK, 3); }
static void DI_Initialize(Guest* g) { RET(DI_OK, 3); }

/* --- IDirectInputDevice8A ------------------------------------------------------------------------------ */
static void DD_QueryInterface(Guest* g)
{
    Dev* d = dev(ARG(0));
    /* IDirectInputDevice8A {54D41080-DC15-4833-A41B-748F73A38179} and the older device interfaces: itself */
    wr32(ARG(2), d ? d->guest : 0);
    if (!d)
        RET(DIERR_NOINTERFACE, 3);
    d->refs++;
    RET(DI_OK, 3);
}

static void DD_AddRef(Guest* g)
{
    Dev* d = dev(ARG(0));
    RET(d ? (uint32_t)++d->refs : 0, 1);
}

static void DD_Release(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(0, 1);
    uint32_t n = (uint32_t)--d->refs;
    if (!n)
    {
        gheap_free(d->guest);
        d->used = 0;
    }
    RET(n, 1);
}

/* DIDEVCAPS {dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, ...} */
static void DD_GetCapabilities(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 2);
    uint32_t p = ARG(1), size = rd32(p);
    memset(GUEST_PTR(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 1); /* DIDC_ATTACHED */
    if (d->kind == D_KEYBOARD)
        wr32(p + 8, 0x0413), wr32(p + 16, 128);
    else if (d->kind == D_MOUSE)
        wr32(p + 8, 0x0112), wr32(p + 12, 3), wr32(p + 16, 5);
    else
        wr32(p + 8, 0x00010215), wr32(p + 12, PAD_AXES), wr32(p + 16, PAD_BUTTONS), wr32(p + 20, 1);
    RET(DI_OK, 2);
}

/* DIDEVICEOBJECTINSTANCEA (316 bytes) for a gamepad object */
static void write_object(uint32_t p, int src, int index, uint32_t ofs)
{
    memset(GUEST_PTR(p + 4), 0, 312);
    if (src == SRC_AXIS)
    {
        memcpy(GUEST_PTR(p + 4), AXIS_GUIDS[index], 16);
        wr32(p + 24, 0x2u | (uint32_t)index << 8); /* DIDFT_ABSAXIS | instance */
        wr32(p + 28, 0x100);                       /* DIDOI_ASPECTPOSITION */
        snprintf((char*)GUEST_PTR(p + 32), 260, "%s", AXIS_NAMES[index]);
    }
    else if (src == SRC_POV)
    {
        memcpy(GUEST_PTR(p + 4), GUID_POV, 16);
        wr32(p + 24, 0x10u);
        snprintf((char*)GUEST_PTR(p + 32), 260, "Hat Switch");
    }
    else
    {
        memcpy(GUEST_PTR(p + 4), GUID_Button, 16);
        wr32(p + 24, 0x4u | (uint32_t)index << 8); /* DIDFT_PSHBUTTON | instance */
        snprintf((char*)GUEST_PTR(p + 32), 260, "Button %d", index);
    }
    wr32(p + 20, ofs);
}

/* where an object is in the device's data format (DIJOYSTATE's layout until one is set) */
static uint32_t object_ofs(const Dev* d, int src, int index)
{
    for (uint32_t i = 0; i < d->nmap; ++i)
        if (d->map[i].src == src && d->map[i].index == index)
            return d->map[i].ofs;
    return src == SRC_AXIS ? 4u * (uint32_t)index : src == SRC_POV ? 32u : 48u + (uint32_t)index;
}

/* EnumObjects(lpCallback, pvRef, dwFlags): DIDFT_ALL 0, AXIS 3, BUTTON 0xC, POV 0x10 */
static void DD_EnumObjects(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 4);
    if (d->kind != D_PAD)
        RET(DI_OK, 4); /* keyboard and mouse objects: not needed so far */
    uint32_t flags = ARG(3), o = gheap_alloc(316, 1);
    wr32(o, 316);
    int stop = 0;
    for (int a = 0; a < PAD_AXES && !stop && (!flags || (flags & 3)); ++a)
    {
        write_object(o, SRC_AXIS, a, object_ofs(d, SRC_AXIS, a));
        uint32_t args[2] = { o, ARG(2) };
        stop = !guest_call(ARG(1), 2, args);
    }
    if (!stop && (!flags || (flags & 0x10)))
    {
        write_object(o, SRC_POV, 0, object_ofs(d, SRC_POV, 0));
        uint32_t args[2] = { o, ARG(2) };
        stop = !guest_call(ARG(1), 2, args);
    }
    for (int b = 0; b < PAD_BUTTONS && !stop && (!flags || (flags & 0xC)); ++b)
    {
        write_object(o, SRC_BUTTON, b, object_ofs(d, SRC_BUTTON, b));
        uint32_t args[2] = { o, ARG(2) };
        stop = !guest_call(ARG(1), 2, args);
    }
    gheap_free(o);
    RET(DI_OK, 4);
}

/* the axis a DIPROPHEADER addresses: dwHow DIPH_DEVICE (all, -1) or DIPH_BYOFFSET */
static int prop_axis(const Dev* d, uint32_t ph)
{
    uint32_t obj = rd32(ph + 8), how = rd32(ph + 12);
    if (how == 0)
        return -1;
    if (how == 1)
        for (int a = 0; a < PAD_AXES; ++a)
            if (object_ofs(d, SRC_AXIS, a) == obj)
                return a;
    if (how == 2 && (obj & 3))
        return (int)((obj >> 8) & 0xFFFF) < PAD_AXES ? (int)((obj >> 8) & 0xFFFF) : -2;
    return -2;
}

/* GetProperty/SetProperty(rguidProp, pdiph): DIPROP_* are small integers in place of a GUID pointer */
static void DD_SetProperty(Guest* g)
{
    Dev* d = dev(ARG(0));
    uint32_t prop = ARG(1), ph = ARG(2);
    if (!d)
        RET(DIERR_INVALIDPARAM, 3);
    switch (prop)
    {
    case 1: /* DIPROP_BUFFERSIZE */
        d->buffer_size = rd32(ph + 16);
        RET(DI_OK, 3);
    case 2: /* DIPROP_AXISMODE: absolute (0) is all there is */
        RET(DI_OK, 3);
    case 4: /* DIPROP_RANGE */
    case 5: /* DIPROP_DEADZONE */
    {
        int a = prop_axis(d, ph);
        if (a == -2)
            RET(DIERR_OBJECTNOTFOUND, 3);
        for (int i = a < 0 ? 0 : a; i < (a < 0 ? PAD_AXES : a + 1); ++i)
            if (prop == 4)
                d->min[i] = (int32_t)rd32(ph + 16), d->max[i] = (int32_t)rd32(ph + 20);
            else
                d->deadzone[i] = rd32(ph + 16);
        RET(DI_OK, 3);
    }
    default: RET(DI_OK, 3); /* saturation, auto-centre, gain: accepted */
    }
}

static void DD_GetProperty(Guest* g)
{
    Dev* d = dev(ARG(0));
    uint32_t prop = ARG(1), ph = ARG(2);
    if (!d)
        RET(DIERR_INVALIDPARAM, 3);
    int a = prop_axis(d, ph);
    switch (prop)
    {
    case 1: wr32(ph + 16, d->buffer_size); RET(DI_OK, 3);
    case 2: wr32(ph + 16, 0); RET(DI_OK, 3);
    case 4:
        a = a < 0 ? 0 : a;
        wr32(ph + 16, (uint32_t)d->min[a]);
        wr32(ph + 20, (uint32_t)d->max[a]);
        RET(DI_OK, 3);
    case 5: wr32(ph + 16, d->deadzone[a < 0 ? 0 : a]); RET(DI_OK, 3);
    case 6: wr32(ph + 16, 10000); RET(DI_OK, 3); /* DIPROP_SATURATION */
    default: RET(DIERR_UNSUPPORTED, 3);
    }
}

static void DD_Acquire(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 1);
    if (d->acquired)
        RET(S_FALSE, 1);
    d->acquired = 1;
    if (d->kind != D_PAD)
        d->cursor = input_cursor_now(d->kind == D_KEYBOARD ? INPUT_KEYBOARD : INPUT_MOUSE);
    if (d->kind == D_MOUSE)
    {
        int32_t x, y, z;
        uint8_t b[8];
        input_mouse(&x, &y, &z, b, 1); /* motion from before the acquire does not count */
    }
    RET(DI_OK, 1);
}

static void DD_Unacquire(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 1);
    int was = d->acquired;
    d->acquired = 0;
    RET(was ? DI_OK : DI_NOEFFECT, 1);
}

/* the gamepad's state, laid out by the data format; also used to diff for buffered data */
static void pad_format(Dev* d, uint8_t* out)
{
    PadState s;
    memset(out, 0, d->data_size);
    input_pad_state(d->pad, &s);
    int32_t axes[PAD_AXES] = { s.x, s.y, s.z, s.rx, s.ry };
    for (uint32_t i = 0; i < d->nmap; ++i)
    {
        Map* m = &d->map[i];
        if (m->ofs + (m->src == SRC_BUTTON ? 1 : 4) > d->data_size)
            continue;
        if (m->src == SRC_AXIS)
        {
            int32_t v = axes[m->index];
            int64_t dz = (int64_t)d->deadzone[m->index] * 32768 / 10000;
            if (v > -dz && v < dz)
                v = 0;
            int64_t lo = d->min[m->index], hi = d->max[m->index];
            int32_t r = (int32_t)(lo + ((int64_t)v + 32768) * (hi - lo) / 65535);
            memcpy(out + m->ofs, &r, 4);
        }
        else if (m->src == SRC_POV)
            memcpy(out + m->ofs, m->index == 0 ? &s.pov : &(uint32_t){ 0xFFFFFFFFu }, 4);
        else
            out[m->ofs] = m->index < 11 ? s.buttons[m->index] : 0;
    }
}

/* GetDeviceState(cbData, lpvData) */
static void DD_GetDeviceState(Guest* g)
{
    Dev* d = dev(ARG(0));
    uint32_t n = ARG(1);
    uint8_t* out = (uint8_t*)ARGP(2);
    if (!d)
        RET(DIERR_INVALIDPARAM, 3);
    if (!d->acquired)
        RET(DIERR_NOTACQUIRED, 3);
    if (d->kind == D_KEYBOARD)
    {
        uint8_t k[256];
        input_keyboard(k);
        memset(out, 0, n);
        memcpy(out, k, n < 256 ? n : 256);
    }
    else if (d->kind == D_MOUSE)
    {
        int32_t v[3];
        uint8_t b[8];
        input_mouse(&v[0], &v[1], &v[2], b, 1);
        memset(out, 0, n);
        memcpy(out, v, n < 12 ? n : 12);
        if (n > 12)
            memcpy(out + 12, b, n - 12 < 8 ? n - 12 : 8); /* DIMOUSESTATE: 4 buttons, DIMOUSESTATE2: 8 */
    }
    else
    {
        uint8_t buf[512];
        if (!d->data_size || d->data_size > sizeof buf)
            RET(DIERR_INVALIDPARAM, 3);
        pad_format(d, buf);
        memset(out, 0, n);
        memcpy(out, buf, n < d->data_size ? n : d->data_size);
    }
    RET(DI_OK, 3);
}

/* GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags): DIDEVICEOBJECTDATA {ofs, data, time, seq, appdata} */
static void DD_GetDeviceData(Guest* g)
{
    Dev* d = dev(ARG(0));
    uint32_t cb = ARG(1), out = ARG(2), inout = ARG(3), peek = ARG(4) & 1;
    if (!d)
        RET(DIERR_INVALIDPARAM, 5);
    if (!d->acquired)
        RET(DIERR_NOTACQUIRED, 5);
    if (!d->buffer_size)
        RET(DIERR_NOTBUFFERED, 5);
    uint32_t max = rd32(inout), got = 0;
    int lost = 0;
    if (d->kind == D_PAD)
    {
        /* gamepads are polled: the changes since the last call become events */
        uint8_t now[512];
        if (d->data_size && d->data_size <= sizeof now)
        {
            pad_format(d, now);
            uint8_t* last = (uint8_t*)d->last;
            for (uint32_t i = 0; i < d->nmap && got < max; ++i)
            {
                Map* m = &d->map[i];
                uint32_t w = m->src == SRC_BUTTON ? 1 : 4;
                if (m->ofs + w > d->data_size || !memcmp(now + m->ofs, last + m->ofs, w))
                    continue;
                uint32_t v = 0;
                memcpy(&v, now + m->ofs, w);
                if (out)
                {
                    uint32_t e = out + got * cb;
                    memset(GUEST_PTR(e), 0, cb);
                    wr32(e, m->ofs), wr32(e + 4, v), wr32(e + 8, (uint32_t)(rt_monotonic_ns() / 1000000u)), wr32(e + 12, ++d->pad_seq);
                }
                got++;
                if (!peek)
                    memcpy(last + m->ofs, now + m->ofs, w);
            }
        }
        wr32(inout, got);
        RET(DI_OK, 5);
    }
    int kind = d->kind == D_KEYBOARD ? INPUT_KEYBOARD : INPUT_MOUSE;
    uint32_t cursor = d->cursor;
    InputEvent ev[64];
    while (got < max)
    {
        unsigned want = max - got < 64 ? max - got : 64;
        unsigned n = input_events(kind, &cursor, ev, want, &lost);
        if (!n)
            break;
        for (unsigned i = 0; i < n && out; ++i)
        {
            uint32_t e = out + (got + i) * cb;
            memset(GUEST_PTR(e), 0, cb);
            wr32(e, ev[i].ofs), wr32(e + 4, ev[i].data), wr32(e + 8, ev[i].time), wr32(e + 12, ev[i].seq);
        }
        got += n;
    }
    if (!peek)
        d->cursor = cursor;
    wr32(inout, got);
    RET(lost ? DI_BUFFEROVERFLOW : DI_OK, 5);
}

/* SetDataFormat(lpdf): DIDATAFORMAT {dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf},
 * DIOBJECTDATAFORMAT {pguid, dwOfs, dwType, dwFlags} */
static void DD_SetDataFormat(Guest* g)
{
    Dev* d = dev(ARG(0));
    uint32_t f = ARG(1);
    if (!d || !f)
        RET(DIERR_INVALIDPARAM, 2);
    if (d->acquired)
        RET(0x80070005u, 2); /* DIERR_ACQUIRED */
    d->data_size = rd32(f + 12);
    uint32_t n = rd32(f + 16), objs = rd32(f + 20), osize = rd32(f + 4);
    d->nmap = 0;
    if (d->kind == D_PAD)
    {
        int next_axis = 0, next_button = 0, next_pov = 0;
        for (uint32_t i = 0; i < n && d->nmap < 256; ++i)
        {
            uint32_t o = objs + i * osize, pg = rd32(o), ofs = rd32(o + 4), type = rd32(o + 8);
            int any = (type & 0x00FFFF00u) == 0x00FFFF00u;
            uint32_t inst = (type >> 8) & 0xFFFF;
            Map m = { ofs, SRC_NONE, 0 };
            const uint8_t* guid = pg ? (const uint8_t*)GUEST_PTR(pg) : NULL;
            int axis_by_guid = -1;
            for (int a = 0; guid && a < PAD_AXES; ++a)
                if (!memcmp(guid, AXIS_GUIDS[a], 16))
                    axis_by_guid = a;
            if (axis_by_guid >= 0)
                m.src = SRC_AXIS, m.index = (uint8_t)axis_by_guid;
            else if ((guid && !memcmp(guid, GUID_POV, 16)) || (!guid && (type & 0x10)))
            {
                int idx = any ? next_pov++ : (int)inst;
                if (idx < 1)
                    m.src = SRC_POV, m.index = (uint8_t)idx;
            }
            else if ((guid && !memcmp(guid, GUID_Button, 16)) || (!guid && (type & 0xC)))
            {
                int idx = any ? next_button++ : (int)inst;
                if (idx < PAD_BUTTONS)
                    m.src = SRC_BUTTON, m.index = (uint8_t)idx;
            }
            else if (!guid && (type & 3))
            {
                int idx = any ? next_axis++ : (int)inst;
                if (idx < PAD_AXES)
                    m.src = SRC_AXIS, m.index = (uint8_t)idx;
            }
            if (m.src != SRC_NONE)
                d->map[d->nmap++] = m;
        }
        memset(d->last, 0, sizeof d->last);
    }
    RET(DI_OK, 2);
}

static void DD_SetEventNotification(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 2);
    d->event = ARG(1);
    RET(DI_OK, 2);
}

static void DD_SetCooperativeLevel(Guest* g) { RET(dev(ARG(0)) ? DI_OK : DIERR_INVALIDPARAM, 3); }

static void DD_GetDeviceInfo(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 2);
    write_instance(ARG(1), d->kind, d->pad);
    RET(DI_OK, 2);
}

static void DD_Poll(Guest* g)
{
    Dev* d = dev(ARG(0));
    if (!d)
        RET(DIERR_INVALIDPARAM, 1);
    RET(!d->acquired ? DIERR_NOTACQUIRED : d->kind == D_PAD ? DI_OK : DI_NOEFFECT, 1);
}

/* --- entry point and registration ----------------------------------------------------------------------- */
/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
static void sh_DirectInput8Create(Guest* g)
{
    wr32(ARG(3), 0);
    if (ARG(4))
        RET(DIERR_NOAGGREGATION, 5);
    if (!g_di)
    {
        g_di = gheap_alloc(8, 1);
        wr32(g_di, g_vtbl_di);
        input_set_notify(INPUT_KEYBOARD, notify);
        input_set_notify(INPUT_MOUSE, notify);
    }
    g_di_refs++;
    wr32(ARG(3), g_di);
    RET(DI_OK, 5);
}

/* --- force feedback --------------------------------------------------------------------------------------
 * FFXiMain creates one effect on the gamepad (and ignores whether that worked, keeping whatever
 * pointer comes back). Effects here are accepted and do nothing: the pad's rumble goes through
 * XInput (XI_SetState). One shared effect object serves every CreateEffect. */
/* CreateEffect(rguid, lpeff, ppdeff, punkOuter) */
static void DD_CreateEffect(Guest* g)
{
    if (!g_effect)
    {
        g_effect = gheap_alloc(8, 1);
        wr32(g_effect, g_vtbl_eff);
    }
    if (ARG(3))
        wr32(ARG(3), g_effect);
    RET(DI_OK, 5);
}

/* EnumEffects(lpCallback, pvRef, dwEffType): no effects to list */
static void DD_EnumEffects(Guest* g) { RET(DI_OK, 4); }
static void DD_GetEffectInfo(Guest* g) { RET(DIERR_DEVICENOTREG, 3); }
/* GetForceFeedbackState(pdwOut): nothing playing */
static void DD_GetForceFeedbackState(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0);
    RET(DI_OK, 2);
}
static void DD_SendForceFeedbackCommand(Guest* g) { RET(DI_OK, 2); }
static void DD_EnumCreatedEffectObjects(Guest* g) { RET(DI_OK, 4); }

static void EF_QueryInterface(Guest* g)
{
    if (ARG(2))
        wr32(ARG(2), 0);
    RET(DIERR_NOINTERFACE, 3);
}
static void EF_AddRef(Guest* g) { RET(1, 1); }
static void EF_Release(Guest* g) { RET(0, 1); }
static void EF_Ok1(Guest* g) { RET(DI_OK, 1); }  /* Stop, Download, Unload */
static void EF_Ok2(Guest* g) { RET(DI_OK, 2); }  /* GetEffectGuid, Escape */
static void EF_Ok3(Guest* g) { RET(DI_OK, 3); }  /* GetParameters, SetParameters, Start */
static void EF_Ok4(Guest* g) { RET(DI_OK, 4); }  /* Initialize */
/* GetEffectStatus(pdwFlags): not playing */
static void EF_GetEffectStatus(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0);
    RET(DI_OK, 2);
}

#define S(i, m, f) { "dinput8.dll", i "::" m, f }
static const ShimDef DINPUT[] = {
    { "dinput8.dll", "DirectInput8Create", sh_DirectInput8Create },
    S("IDirectInput8A", "QueryInterface", DI_QueryInterface),
    S("IDirectInput8A", "AddRef", DI_AddRef),
    S("IDirectInput8A", "Release", DI_Release),
    S("IDirectInput8A", "CreateDevice", DI_CreateDevice),
    S("IDirectInput8A", "EnumDevices", DI_EnumDevices),
    S("IDirectInput8A", "GetDeviceStatus", DI_GetDeviceStatus),
    S("IDirectInput8A", "RunControlPanel", DI_RunControlPanel),
    S("IDirectInput8A", "Initialize", DI_Initialize),
    S("IDirectInputDevice8A", "QueryInterface", DD_QueryInterface),
    S("IDirectInputDevice8A", "AddRef", DD_AddRef),
    S("IDirectInputDevice8A", "Release", DD_Release),
    S("IDirectInputDevice8A", "GetCapabilities", DD_GetCapabilities),
    S("IDirectInputDevice8A", "EnumObjects", DD_EnumObjects),
    S("IDirectInputDevice8A", "GetProperty", DD_GetProperty),
    S("IDirectInputDevice8A", "SetProperty", DD_SetProperty),
    S("IDirectInputDevice8A", "Acquire", DD_Acquire),
    S("IDirectInputDevice8A", "Unacquire", DD_Unacquire),
    S("IDirectInputDevice8A", "GetDeviceState", DD_GetDeviceState),
    S("IDirectInputDevice8A", "GetDeviceData", DD_GetDeviceData),
    S("IDirectInputDevice8A", "SetDataFormat", DD_SetDataFormat),
    S("IDirectInputDevice8A", "SetEventNotification", DD_SetEventNotification),
    S("IDirectInputDevice8A", "SetCooperativeLevel", DD_SetCooperativeLevel),
    S("IDirectInputDevice8A", "GetDeviceInfo", DD_GetDeviceInfo),
    S("IDirectInputDevice8A", "Poll", DD_Poll),
    S("IDirectInputDevice8A", "CreateEffect", DD_CreateEffect),
    S("IDirectInputDevice8A", "EnumEffects", DD_EnumEffects),
    S("IDirectInputDevice8A", "GetEffectInfo", DD_GetEffectInfo),
    S("IDirectInputDevice8A", "GetForceFeedbackState", DD_GetForceFeedbackState),
    S("IDirectInputDevice8A", "SendForceFeedbackCommand", DD_SendForceFeedbackCommand),
    S("IDirectInputDevice8A", "EnumCreatedEffectObjects", DD_EnumCreatedEffectObjects),
    S("IDirectInputEffect", "QueryInterface", EF_QueryInterface),
    S("IDirectInputEffect", "AddRef", EF_AddRef),
    S("IDirectInputEffect", "Release", EF_Release),
    S("IDirectInputEffect", "Initialize", EF_Ok4),
    S("IDirectInputEffect", "GetEffectGuid", EF_Ok2),
    S("IDirectInputEffect", "GetParameters", EF_Ok3),
    S("IDirectInputEffect", "SetParameters", EF_Ok3),
    S("IDirectInputEffect", "Start", EF_Ok3),
    S("IDirectInputEffect", "Stop", EF_Ok1),
    S("IDirectInputEffect", "GetEffectStatus", EF_GetEffectStatus),
    S("IDirectInputEffect", "Download", EF_Ok1),
    S("IDirectInputEffect", "Unload", EF_Ok1),
    S("IDirectInputEffect", "Escape", EF_Ok2),
    { NULL, NULL, NULL },
};

static const char* const kDI8[] = { "QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
    "GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "EnumDevicesBySemantics", "ConfigureDevices",
    NULL };
static const char* const kDev8[] = { "QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
    "GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState", "GetDeviceData", "SetDataFormat",
    "SetEventNotification", "SetCooperativeLevel", "GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize",
    "CreateEffect", "EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
    "EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile", "WriteEffectToFile",
    "BuildActionMap", "SetActionMap", "GetImageInfo", NULL };
static const char* const kEff[] = { "QueryInterface", "AddRef", "Release", "Initialize", "GetEffectGuid", "GetParameters",
    "SetParameters", "Start", "Stop", "GetEffectStatus", "Download", "Unload", "Escape", NULL };

static uint32_t make_vtbl(const char* iface, const char* const* names)
{
    uint32_t n = 0;
    while (names[n])
        n++;
    uint32_t v = gheap_alloc(4 * n, 1);
    char full[96];
    for (uint32_t i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, names[i]);
        wr32(v + 4 * i, thunk_for("dinput8.dll", full));
    }
    return v;
}

/* --- XInput: the game folder's xinputdll.dll -----------------------------------------------------------
 * FFXiMain loads xinputdll.dll from its own folder (a Square Enix wrapper over XInput 1.3) and uses
 * its three exports when the pad is set to XInput; it keeps retrying the load while it fails, and
 * reads no gamepad meanwhile. KERNEL32's LoadLibraryA hands out a module for it (k32.c
 * KNOWN_DLLS) whose exports are these. All three are cdecl, as the wrapper declares them. */
#define ERROR_DEVICE_NOT_CONNECTED 1167u

static uint32_t g_xpacket[4];
static XPad g_xlast[4];
void (*dinput_xpad_hook)(uint32_t user, XPad* pad);
static int g_xseen[4] = { -1, -1, -1, -1 }; /* connected at the last poll: -1 never polled */
static int g_xinput_seen[4];

/* XInputGetState_(dwUserIndex, XINPUT_STATE* pState) */
static void XI_GetState(Guest* g)
{
    uint32_t user = ARG(0), out = ARG(1);
    XPad x;
    int connected = user < 4 && input_xpad((int)user, &x);
    if (user < 4 && connected != g_xseen[user])
    {
        rt_log("[recomp] xinput: pad %u %s\n", user,
            g_xseen[user] < 0 ? (connected ? "polled, connected" : "polled, nothing connected") : connected ? "connected" : "disconnected");
        g_xseen[user] = connected;
    }
    if (connected && !g_xinput_seen[user] && (x.buttons || x.lt > 32 || x.rt > 32 || x.lx > 8000 || x.lx < -8000 || x.ly > 8000 ||
                                                 x.ly < -8000 || x.rx > 8000 || x.rx < -8000 || x.ry > 8000 || x.ry < -8000))
    {
        g_xinput_seen[user] = 1;
        rt_log("[recomp] xinput: pad %u first input (buttons %04x)\n", user, x.buttons);
    }
    if (!connected)
    {
        if (out)
            memset(GUEST_PTR(out), 0, 16);
        RETC(ERROR_DEVICE_NOT_CONNECTED);
    }
    if (dinput_xpad_hook)
        dinput_xpad_hook(user, &x); /* the addon host: button events, and buttons it keeps from the game */
    if (memcmp(&x, &g_xlast[user], sizeof x))
        g_xlast[user] = x, g_xpacket[user]++;
    /* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD {wButtons, bLeftTrigger, bRightTrigger,
     * sThumbLX, sThumbLY, sThumbRX, sThumbRY} */
    wr32(out, g_xpacket[user]);
    wr16(out + 4, x.buttons);
    wr8(out + 6, x.lt);
    wr8(out + 7, x.rt);
    wr16(out + 8, (uint16_t)x.lx);
    wr16(out + 10, (uint16_t)x.ly);
    wr16(out + 12, (uint16_t)x.rx);
    wr16(out + 14, (uint16_t)x.ry);
    RETC(0);
}

/* XInputSetState_(dwUserIndex, XINPUT_VIBRATION* {wLeftMotorSpeed, wRightMotorSpeed}) */
static void XI_SetState(Guest* g)
{
    uint32_t user = ARG(0), v = ARG(1);
    if (user >= 4 || user >= (uint32_t)input_pad_count())
        RETC(ERROR_DEVICE_NOT_CONNECTED);
    if (v)
        input_rumble((int)user, rd16(v), rd16(v + 2));
    RETC(0);
}

/* XInputEnable_(BOOL): the wrapper returns 1 */
static void XI_Enable(Guest* g) { RETC(1); }

static const ShimDef XINPUT[] = {
    { "xinputdll.dll", "XInputGetState_", XI_GetState },
    { "xinputdll.dll", "XInputSetState_", XI_SetState },
    { "xinputdll.dll", "XInputEnable_", XI_Enable },
    { NULL, NULL, NULL },
};

void dinput_init(void)
{
    thunk_register(DINPUT);
    thunk_register(XINPUT);
}

void dinput_setup(void)
{
    g_vtbl_di = make_vtbl("IDirectInput8A", kDI8);
    g_vtbl_dev = make_vtbl("IDirectInputDevice8A", kDev8);
    g_vtbl_eff = make_vtbl("IDirectInputEffect", kEff);
}
