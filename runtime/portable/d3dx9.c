/* The parts of D3DX 9 (d3dx9_41.dll) FINAL FANTASY XIV 1.23b calls.
 *
 *   D3DXGetShaderConstantTable   parses the CTAB comment the HLSL compiler leaves in every shader
 *                                into an ID3DXConstantTable: the game looks its constants up by name
 *                                and sets them through it.
 *   D3DXGet{Vertex,Pixel}ShaderProfile   the best the device takes: vs_3_0 / ps_3_0.
 *   D3DXCreate{,Cube,Volume}Texture      the device's Create*Texture with D3DX's defaults.
 *   D3DXLoadSurfaceFromMemory, D3DXLoadVolumeFromMemory   copies between equal formats.
 *   D3DXCompileShader(FromFileA)  not available: the game's shaders come compiled (logged).
 *   D3DXSaveSurfaceToFileW        screenshots: not written yet (logged).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include "../../third_party/stb/stb_image.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_FAIL 0x80004005u
#define E_NOINTERFACE 0x80004002u

void d3d9_set_constants(int pixel, int set, uint32_t start, const void* data, uint32_t count);

/* --- ID3DXConstantTable --------------------------------------------------------------------------- */

typedef struct Constant
{
    uint32_t name;              /* guest address of its name (our D3DXHANDLE) */
    uint32_t set, reg, count;   /* D3DXREGISTER_SET, first register, register count */
    uint32_t cls, type, rows, cols, elements, members, bytes;
    uint32_t def;               /* guest address of its default value, or 0 */
} Constant;

typedef struct Table
{
    uint32_t guest;   /* the COM object */
    int32_t refs;
    int pixel;
    uint32_t blob, blob_size; /* guest copy of the CTAB data */
    uint32_t creator, version, target;
    Constant* c;
    uint32_t n;
} Table;

#define MAX_TABLES 4096
static Table* g_tables[MAX_TABLES];
static uint32_t g_vtbl;

static Table* table(uint32_t p)
{
    if (!p)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < MAX_TABLES && g_tables[i] && g_tables[i]->guest == p ? g_tables[i] : NULL;
}

/* a handle is one of our names, or a name string the game passes instead (D3DX allows both) */
static Constant* constant(Table* t, uint32_t h)
{
    if (!t || !h)
        return NULL;
    for (uint32_t i = 0; i < t->n; ++i)
        if (t->c[i].name == h)
            return &t->c[i];
    const char* s = (const char*)GUEST_PTR(h);
    for (uint32_t i = 0; i < t->n; ++i)
        if (!strcmp((const char*)GUEST_PTR(t->c[i].name), s))
            return &t->c[i];
    return NULL;
}

static void CT_QueryInterface(Guest* g) { if (ARG(2)) wr32(ARG(2), 0); RET(E_NOINTERFACE, 3); }
static void CT_AddRef(Guest* g) { Table* t = table(ARG(0)); RET(t ? (uint32_t)++t->refs : 0, 1); }
static void CT_Release(Guest* g)
{
    Table* t = table(ARG(0));
    if (!t)
        RET(0, 1);
    if (--t->refs > 0)
        RET((uint32_t)t->refs, 1);
    uint32_t idx = rd32(t->guest + 4);
    gheap_free(t->blob);
    gheap_free(t->guest);
    free(t->c);
    free(t);
    g_tables[idx] = NULL;
    RET(0, 1);
}
static void CT_GetBufferPointer(Guest* g) { Table* t = table(ARG(0)); RET(t ? t->blob : 0, 1); }
static void CT_GetBufferSize(Guest* g) { Table* t = table(ARG(0)); RET(t ? t->blob_size : 0, 1); }

/* D3DXCONSTANTTABLE_DESC { Creator, Version, Constants } */
static void CT_GetDesc(Guest* g)
{
    Table* t = table(ARG(0));
    if (!t)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(ARG(1), t->creator), wr32(ARG(1) + 4, t->version), wr32(ARG(1) + 8, t->n);
    RET(D3D_OK, 2);
}

/* GetConstantDesc(handle, D3DXCONSTANT_DESC*, UINT* count) */
static void CT_GetConstantDesc(Guest* g)
{
    Constant* c = constant(table(ARG(0)), ARG(1));
    if (!c)
        RET(D3DERR_INVALIDCALL, 4);
    uint32_t p = ARG(2);
    if (p)
    {
        uint32_t v[12] = { c->name, c->set, c->reg, c->count, c->cls, c->type, c->rows, c->cols, c->elements,
                           c->members, c->bytes, c->def };
        for (int i = 0; i < 12; ++i)
            wr32(p + 4u * i, v[i]);
    }
    if (ARG(3))
        wr32(ARG(3), 1);
    RET(D3D_OK, 4);
}

static void CT_GetSamplerIndex(Guest* g)
{
    Constant* c = constant(table(ARG(0)), ARG(1));
    RET(c && c->set == 3 ? c->reg : 0xFFFFFFFFu, 2);
}
/* GetConstant(parent, index) / GetConstantByName(parent, name) / GetConstantElement(handle, index):
 * top-level constants only (no structure members, no array elements of their own) */
static void CT_GetConstant(Guest* g)
{
    Table* t = table(ARG(0));
    RET(t && !ARG(1) && ARG(2) < t->n ? t->c[ARG(2)].name : 0, 3);
}
static void CT_GetConstantByName(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = ARG(1) ? NULL : constant(t, ARG(2));
    RET(c ? c->name : 0, 3);
}
static void CT_GetConstantElement(Guest* g)
{
    Constant* c = constant(table(ARG(0)), ARG(1));
    RET(c && ARG(2) == 0 ? c->name : 0, 3);
}

/* writes floats into a constant's registers: values as D3DX lays them out for its class */
static void set_floats(Table* t, Constant* c, const float* v, uint32_t n)
{
    if (c->set == 2)
    {
        float r[4 * 256];
        memset(r, 0, sizeof r);
        uint32_t regs = c->count < 256 ? c->count : 256;
        if (c->cls == 3 /* MATRIX_COLUMNS: registers hold columns */)
        {
            uint32_t per = c->cols ? c->cols : 4, rows = c->rows ? c->rows : 4;
            for (uint32_t e = 0; e * per < regs && e * 16 < n; ++e)
                for (uint32_t col = 0; col < per && e * per + col < regs; ++col)
                    for (uint32_t row = 0; row < rows && row < 4; ++row)
                        if (e * 16 + row * 4 + col < n)
                            r[4 * (e * per + col) + row] = v[e * 16 + row * 4 + col];
        }
        else if (c->cls == 2 /* MATRIX_ROWS */)
        {
            uint32_t per = c->rows ? c->rows : 4, cols = c->cols ? c->cols : 4;
            for (uint32_t e = 0; e * per < regs && e * 16 < n; ++e)
                for (uint32_t row = 0; row < per && e * per + row < regs; ++row)
                    for (uint32_t col = 0; col < cols && col < 4; ++col)
                        if (e * 16 + row * 4 + col < n)
                            r[4 * (e * per + row) + col] = v[e * 16 + row * 4 + col];
        }
        else
        {
            /* scalars and vectors: each element in its own register, components packed */
            uint32_t per = c->cls == 0 ? 1 : (c->cols ? c->cols : 4);
            for (uint32_t i = 0; i < regs && i * per < n; ++i)
                for (uint32_t k = 0; k < per && i * per + k < n; ++k)
                    r[4 * i + k] = v[i * per + k];
        }
        d3d9_set_constants(t->pixel, 2, c->reg, r, regs);
    }
    else if (c->set == 1 || c->set == 0)
    {
        int32_t r[4 * 16];
        memset(r, 0, sizeof r);
        uint32_t regs = c->count < 16 ? c->count : 16;
        for (uint32_t i = 0; i < n && i < 4 * regs; ++i)
            r[c->set == 0 ? i : i] = (int32_t)v[i];
        if (c->set == 0)
        {
            uint32_t b[16];
            for (uint32_t i = 0; i < regs; ++i)
                b[i] = r[i] != 0;
            d3d9_set_constants(t->pixel, 0, c->reg, b, regs);
        }
        else
            d3d9_set_constants(t->pixel, 1, c->reg, r, regs);
    }
}

static void read_floats(uint32_t p, float* out, uint32_t n) { memcpy(out, GUEST_PTR(p), 4 * n); }

/* Set*(device, handle, ...): */
static void CT_SetDefaults(Guest* g)
{
    Table* t = table(ARG(0));
    for (uint32_t i = 0; t && i < t->n; ++i)
        if (t->c[i].def && t->c[i].set == 2)
        {
            float v[4 * 256];
            uint32_t n = t->c[i].bytes / 4 < 1024 ? t->c[i].bytes / 4 : 1024;
            read_floats(t->c[i].def, v, n);
            set_floats(t, &t->c[i], v, n);
        }
    RET(D3D_OK, 2);
}
static void CT_SetValue(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    if (!c)
        RET(D3DERR_INVALIDCALL, 5);
    float v[1024];
    uint32_t n = ARG(4) / 4 < 1024 ? ARG(4) / 4 : 1024;
    read_floats(ARG(3), v, n);
    set_floats(t, c, v, n);
    RET(D3D_OK, 5);
}
static void CT_SetBool(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v = ARG(3) ? 1.0f : 0.0f;
    if (c)
        set_floats(t, c, &v, 1);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 4);
}
static void CT_SetInt(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v = (float)(int32_t)ARG(3);
    if (c)
        set_floats(t, c, &v, 1);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 4);
}
static void CT_SetFloat(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v;
    uint32_t u = ARG(3);
    memcpy(&v, &u, 4);
    if (c)
        set_floats(t, c, &v, 1);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 4);
}
/* SetBoolArray / SetIntArray (device, handle, values, count): integers */
static void int_array(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v[1024];
    uint32_t n = ARG(4) < 1024 ? ARG(4) : 1024;
    for (uint32_t i = 0; i < n; ++i)
        v[i] = (float)(int32_t)rd32(ARG(3) + 4 * i);
    if (c)
        set_floats(t, c, v, n);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 5);
}
static void CT_SetFloatArray(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v[1024];
    uint32_t n = ARG(4) < 1024 ? ARG(4) : 1024;
    read_floats(ARG(3), v, n);
    if (c)
        set_floats(t, c, v, n);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 5);
}
static void CT_SetVector(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v[4];
    read_floats(ARG(3), v, 4);
    if (c)
        set_floats(t, c, v, 4);
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 4);
}
static void CT_SetVectorArray(Guest* g)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    float v[1024];
    uint32_t n = 4 * ARG(4) < 1024 ? 4 * ARG(4) : 1024;
    read_floats(ARG(3), v, n);
    /* vectors of an array: one per register whatever the declared width */
    if (c)
    {
        Constant cc = *c;
        cc.cls = 1, cc.cols = 4;
        set_floats(t, &cc, v, n);
    }
    RET(c ? D3D_OK : D3DERR_INVALIDCALL, 5);
}
static void matrices(Guest* g, uint32_t count, int pointers, int transpose, int nargs)
{
    Table* t = table(ARG(0));
    Constant* c = constant(t, ARG(2));
    if (!c)
        RET(D3DERR_INVALIDCALL, nargs);
    float v[1024];
    if (count > 64)
        count = 64;
    for (uint32_t m = 0; m < count; ++m)
    {
        uint32_t src = pointers ? rd32(ARG(3) + 4 * m) : ARG(3) + 64 * m;
        float x[16];
        read_floats(src, x, 16);
        for (int i = 0; i < 16; ++i)
            v[16 * m + (uint32_t)i] = transpose ? x[(i % 4) * 4 + i / 4] : x[i];
    }
    set_floats(t, c, v, 16 * count);
    RET(D3D_OK, nargs);
}
static void CT_SetMatrix(Guest* g) { matrices(g, 1, 0, 0, 4); }
static void CT_SetMatrixArray(Guest* g) { matrices(g, ARG(4), 0, 0, 5); }
static void CT_SetMatrixPointerArray(Guest* g) { matrices(g, ARG(4), 1, 0, 5); }
static void CT_SetMatrixTranspose(Guest* g) { matrices(g, 1, 0, 1, 4); }
static void CT_SetMatrixTransposeArray(Guest* g) { matrices(g, ARG(4), 0, 1, 5); }
static void CT_SetMatrixTransposePointerArray(Guest* g) { matrices(g, ARG(4), 1, 1, 5); }

static const char* const kTable[] = { "QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize",
    "GetDesc", "GetConstantDesc", "GetSamplerIndex", "GetConstant", "GetConstantByName", "GetConstantElement",
    "SetDefaults", "SetValue", "SetBool", "SetBoolArray", "SetInt", "SetIntArray", "SetFloat", "SetFloatArray",
    "SetVector", "SetVectorArray", "SetMatrix", "SetMatrixArray", "SetMatrixPointerArray", "SetMatrixTranspose",
    "SetMatrixTransposeArray", "SetMatrixTransposePointerArray", NULL };

/* D3DXGetShaderConstantTable(const DWORD* function, ID3DXConstantTable**) */
static void sh_D3DXGetShaderConstantTable(Guest* g)
{
    uint32_t code = ARG(0), out = ARG(1);
    wr32(out, 0);
    if (!code)
        RET(D3DERR_INVALIDCALL, 2);
    uint32_t ver = rd32(code);
    /* the CTAB comment: among the comments before the first instruction */
    uint32_t at = code + 4, ctab = 0, ctab_size = 0;
    for (int k = 0; k < 64; ++k)
    {
        uint32_t tok = rd32(at);
        if ((tok & 0xFFFF) != 0xFFFE)
            break;
        uint32_t n = (tok >> 16) & 0x7FFF;
        if (n >= 1 && rd32(at + 4) == 0x42415443u) /* "CTAB" */
        {
            ctab = at + 8;
            ctab_size = 4 * (n - 1);
            break;
        }
        at += 4 + 4 * n;
    }
    if (!ctab)
        RET(E_FAIL, 2);
    if (!g_vtbl)
    {
        uint32_t n = 0;
        while (kTable[n])
            n++;
        g_vtbl = gheap_alloc(4 * n, 1);
        for (uint32_t i = 0; i < n; ++i)
        {
            char full[96];
            snprintf(full, sizeof full, "ID3DXConstantTable::%s", kTable[i]);
            wr32(g_vtbl + 4 * i, thunk_for("d3dx9_41.dll", full));
        }
    }
    uint32_t idx = 1;
    while (idx < MAX_TABLES && g_tables[idx])
        idx++;
    if (idx >= MAX_TABLES)
        RET(E_FAIL, 2);
    Table* t = (Table*)calloc(1, sizeof *t);
    t->refs = 1;
    t->pixel = (ver >> 16) == 0xFFFF;
    t->blob = gheap_alloc(ctab_size, 0);
    t->blob_size = ctab_size;
    memcpy(GUEST_PTR(t->blob), GUEST_PTR(ctab), ctab_size);
    uint32_t b = t->blob;
    /* D3DXSHADER_CONSTANTTABLE { Size, Creator, Version, Constants, ConstantInfo, Flags, Target } */
    t->creator = b + rd32(b + 4);
    t->version = rd32(b + 8);
    t->n = rd32(b + 12);
    uint32_t info = b + rd32(b + 16);
    t->target = b + rd32(b + 24);
    if (t->n > 4096)
        t->n = 0;
    t->c = (Constant*)calloc(t->n ? t->n : 1, sizeof *t->c);
    for (uint32_t i = 0; i < t->n; ++i)
    {
        /* D3DXSHADER_CONSTANTINFO { Name, RegisterSet:16, RegisterIndex:16, RegisterCount:16, Reserved:16,
         * TypeInfo, DefaultValue } */
        uint32_t ci = info + 20 * i;
        Constant* c = &t->c[i];
        c->name = b + rd32(ci);
        c->set = rd16(ci + 4);
        c->reg = rd16(ci + 6);
        c->count = rd16(ci + 8);
        c->def = rd32(ci + 16) ? b + rd32(ci + 16) : 0;
        /* D3DXSHADER_TYPEINFO { Class, Type, Rows, Columns, Elements, StructMembers:16, StructMemberInfo } */
        uint32_t ti = b + rd32(ci + 12);
        c->cls = rd16(ti), c->type = rd16(ti + 2), c->rows = rd16(ti + 4), c->cols = rd16(ti + 6);
        c->elements = rd16(ti + 8), c->members = rd16(ti + 10);
        c->bytes = 4 * c->rows * c->cols * (c->elements ? c->elements : 1);
    }
    t->guest = gheap_alloc(8, 1);
    wr32(t->guest, g_vtbl);
    wr32(t->guest + 4, idx);
    g_tables[idx] = t;
    wr32(out, t->guest);
    RET(D3D_OK, 2);
}

/* --- profiles, compiling, textures ------------------------------------------------------------- */

static uint32_t guest_string(const char* s)
{
    uint32_t p = gheap_alloc((uint32_t)strlen(s) + 1, 0);
    memcpy(GUEST_PTR(p), s, strlen(s) + 1);
    return p;
}
static void sh_D3DXGetVertexShaderProfile(Guest* g)
{
    static uint32_t s;
    if (!s)
        s = guest_string("vs_3_0");
    RET(s, 1);
}
static void sh_D3DXGetPixelShaderProfile(Guest* g)
{
    static uint32_t s;
    if (!s)
        s = guest_string("ps_3_0");
    RET(s, 1);
}

/* D3DXCompileShader(src, len, defines, include, function, profile, flags, shader**, errors**, table**) */
static void sh_D3DXCompileShader(Guest* g)
{
    static int n;
    if (n++ < 8)
        rt_log("[d3dx9] D3DXCompileShader(%s, %s): no HLSL compiler on this host\n",
            ARG(4) ? (const char*)GUEST_PTR(ARG(4)) : "?", ARG(5) ? (const char*)GUEST_PTR(ARG(5)) : "?");
    if (ARG(7)) wr32(ARG(7), 0);
    if (ARG(8)) wr32(ARG(8), 0);
    if (ARG(9)) wr32(ARG(9), 0);
    RET(E_FAIL, 10);
}
/* D3DXCompileShaderFromFileA(file, defines, include, function, profile, flags, shader**, errors**, table**) */
static void sh_D3DXCompileShaderFromFileA(Guest* g)
{
    rt_log("[d3dx9] D3DXCompileShaderFromFileA(%s): no HLSL compiler on this host\n",
        ARG(0) ? (const char*)GUEST_PTR(ARG(0)) : "?");
    if (ARG(6)) wr32(ARG(6), 0);
    if (ARG(7)) wr32(ARG(7), 0);
    if (ARG(8)) wr32(ARG(8), 0);
    RET(E_FAIL, 9);
}

/* a device method by vtable slot: this, then the arguments */
static uint32_t device_call(uint32_t dev, unsigned slot, unsigned nargs, const uint32_t* args)
{
    uint32_t all[12] = { dev };
    for (unsigned i = 0; i < nargs && i < 11; ++i)
        all[i + 1] = args[i];
    return guest_call(rd32(rd32(dev) + 4 * slot), nargs + 1, all);
}

/* D3DXCreateTexture(device, w, h, levels, usage, fmt, pool, IDirect3DTexture9**) */
static void sh_D3DXCreateTexture(Guest* g)
{
    uint32_t a[8] = { ARG(1) ? ARG(1) : 256, ARG(2) ? ARG(2) : 256, ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), 0 };
    RET(device_call(ARG(0), 23, 8, a), 8);
}
/* D3DXCreateCubeTexture(device, size, levels, usage, fmt, pool, IDirect3DCubeTexture9**) */
static void sh_D3DXCreateCubeTexture(Guest* g)
{
    uint32_t a[7] = { ARG(1) ? ARG(1) : 256, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0 };
    RET(device_call(ARG(0), 25, 7, a), 7);
}
/* D3DXCreateVolumeTexture(device, w, h, d, levels, usage, fmt, pool, IDirect3DVolumeTexture9**) */
static void sh_D3DXCreateVolumeTexture(Guest* g)
{
    uint32_t a[9] = { ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), 0 };
    RET(device_call(ARG(0), 24, 9, a), 9);
}

/* --- pixel conversion through float RGBA ----------------------------------------------------------- */

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e)
    {
        if (!m)
            u = s;
        else
        {
            e = 113;
            while (!(m & 1024)) m <<= 1, e--;
            u = s | (e << 23) | ((m & 1023) << 13);
        }
    }
    else if (e == 31)
        u = s | 0x7F800000u | (m << 13);
    else
        u = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint16_t float_to_half(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000u;
    int32_t e = (int32_t)((u >> 23) & 255) - 112;
    uint32_t m = u & 0x7FFFFFu;
    if (((u >> 23) & 255) == 255)
        return (uint16_t)(s | 0x7C00u | (m ? 0x200u : 0));
    if (e <= 0)
        return (uint16_t)s; /* flushes half denormals */
    if (e >= 31)
        return (uint16_t)(s | 0x7C00u);
    return (uint16_t)(s | ((uint32_t)e << 10) | ((m + 0x1000u) >> 13));
}

static uint32_t px_bytes(uint32_t f)
{
    switch (f)
    {
    case 21: case 22: case 32: case 33: case 114: case 112: case 34: return 4;
    case 113: case 115: case 36: return 8;
    case 116: return 16;
    case 111: case 51: return 2;
    case 28: case 50: return 1;
    default: return 0;
    }
}

static int px_read(uint32_t f, const uint8_t* p, float c[4])
{
    uint32_t v;
    c[0] = c[1] = c[2] = 0, c[3] = 1;
    switch (f)
    {
    case 21: case 22: memcpy(&v, p, 4); c[0] = ((v >> 16) & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = (v & 255) / 255.0f, c[3] = f == 21 ? (v >> 24) / 255.0f : 1.0f; return 1;
    case 32: case 33: memcpy(&v, p, 4); c[0] = (v & 255) / 255.0f, c[1] = ((v >> 8) & 255) / 255.0f, c[2] = ((v >> 16) & 255) / 255.0f, c[3] = f == 32 ? (v >> 24) / 255.0f : 1.0f; return 1;
    case 116: memcpy(c, p, 16); return 1;
    case 115: memcpy(c, p, 8); return 1;
    case 114: memcpy(c, p, 4); return 1;
    case 113: for (int i = 0; i < 4; ++i) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = half_to_float(h); } return 1;
    case 112: for (int i = 0; i < 2; ++i) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = half_to_float(h); } return 1;
    case 111: { uint16_t h; memcpy(&h, p, 2); c[0] = half_to_float(h); return 1; }
    case 36: for (int i = 0; i < 4; ++i) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h / 65535.0f; } return 1;
    case 34: for (int i = 0; i < 2; ++i) { uint16_t h; memcpy(&h, p + 2 * i, 2); c[i] = h / 65535.0f; } return 1;
    case 50: c[0] = c[1] = c[2] = p[0] / 255.0f; return 1;
    case 51: c[0] = c[1] = c[2] = p[0] / 255.0f, c[3] = p[1] / 255.0f; return 1;
    case 28: c[3] = p[0] / 255.0f; return 1;
    default: return 0;
    }
}

static uint8_t u8(float x) { return (uint8_t)(x <= 0 ? 0 : x >= 1 ? 255 : (int)(x * 255.0f + 0.5f)); }
static uint16_t u16(float x) { return (uint16_t)(x <= 0 ? 0 : x >= 1 ? 65535 : (int)(x * 65535.0f + 0.5f)); }

static int px_write(uint32_t f, uint8_t* p, const float c[4])
{
    uint32_t v;
    switch (f)
    {
    case 21: case 22: v = (uint32_t)u8(c[2]) | ((uint32_t)u8(c[1]) << 8) | ((uint32_t)u8(c[0]) << 16) | ((uint32_t)(f == 21 ? u8(c[3]) : 255) << 24); memcpy(p, &v, 4); return 1;
    case 32: case 33: v = (uint32_t)u8(c[0]) | ((uint32_t)u8(c[1]) << 8) | ((uint32_t)u8(c[2]) << 16) | ((uint32_t)(f == 32 ? u8(c[3]) : 255) << 24); memcpy(p, &v, 4); return 1;
    case 116: memcpy(p, c, 16); return 1;
    case 115: memcpy(p, c, 8); return 1;
    case 114: memcpy(p, c, 4); return 1;
    case 113: for (int i = 0; i < 4; ++i) { uint16_t h = float_to_half(c[i]); memcpy(p + 2 * i, &h, 2); } return 1;
    case 112: for (int i = 0; i < 2; ++i) { uint16_t h = float_to_half(c[i]); memcpy(p + 2 * i, &h, 2); } return 1;
    case 111: { uint16_t h = float_to_half(c[0]); memcpy(p, &h, 2); return 1; }
    case 36: for (int i = 0; i < 4; ++i) { uint16_t h = u16(c[i]); memcpy(p + 2 * i, &h, 2); } return 1;
    case 34: for (int i = 0; i < 2; ++i) { uint16_t h = u16(c[i]); memcpy(p + 2 * i, &h, 2); } return 1;
    case 50: p[0] = u8(c[0]); return 1;
    case 51: p[0] = u8(c[0]), p[1] = u8(c[3]); return 1;
    case 28: p[0] = u8(c[3]); return 1;
    default: return 0;
    }
}

/* D3DXLoadSurfaceFromMemory(dst, dst palette, dst rect, src, src format, src pitch, src palette,
 * src rect, filter, color key): rows copied (equal formats) or converted pixel by pixel through
 * float RGBA, into the surface through its lock. No scaling: the source rectangle's size is drawn. */
static void sh_D3DXLoadSurfaceFromMemory(Guest* g)
{
    uint32_t dst = ARG(0), drect = ARG(2), src = ARG(3), sfmt = ARG(4), spitch = ARG(5), srect = ARG(7);
    uint32_t desc = gheap_alloc(32, 1), lr = gheap_alloc(8, 1);
    uint32_t a1[1] = { desc };
    device_call(dst, 12, 1, a1); /* IDirect3DSurface9::GetDesc */
    uint32_t dfmt = rd32(desc);
    int32_t r[4] = { 0, 0, (int32_t)rd32(desc + 24), (int32_t)rd32(desc + 28) };
    if (srect)
        for (int i = 0; i < 4; ++i)
            r[i] = (int32_t)rd32(srect + 4u * i);
    uint32_t a2[3] = { lr, drect, 0 };
    uint32_t hr = device_call(dst, 13, 3, a2); /* LockRect */
    if (!hr)
    {
        uint32_t dpitch = rd32(lr), bits = rd32(lr + 4);
        uint32_t w = (uint32_t)(r[2] - r[0]), rows = (uint32_t)(r[3] - r[1]);
        if (dfmt == sfmt)
        {
            int block = sfmt == 0x31545844u || sfmt == 0x33545844u || sfmt == 0x35545844u;
            uint32_t row_bytes = spitch < dpitch ? spitch : dpitch;
            if (block)
                rows = (rows + 3) / 4;
            for (uint32_t y = 0; y < rows; ++y)
                memcpy(GUEST_PTR(bits + y * dpitch), GUEST_PTR(src + y * spitch), row_bytes);
        }
        else if (px_bytes(sfmt) && px_bytes(dfmt))
        {
            uint32_t sb = px_bytes(sfmt), db = px_bytes(dfmt);
            for (uint32_t y = 0; y < rows; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    float c[4];
                    px_read(sfmt, GUEST_PTR(src + y * spitch + x * sb), c);
                    px_write(dfmt, GUEST_PTR(bits + y * dpitch + x * db), c);
                }
        }
        else
        {
            static int n;
            if (n++ < 8)
                rt_log("[d3dx9] D3DXLoadSurfaceFromMemory: format %08x into %08x is not converted\n", sfmt, dfmt);
        }
        device_call(dst, 14, 0, NULL); /* UnlockRect */
    }
    gheap_free(desc);
    gheap_free(lr);
    RET(hr, 10);
}

/* D3DXLoadVolumeFromMemory(dst, dst palette, dst box, src, src format, row pitch, slice pitch,
 * src palette, src box, filter, color key) */
static void sh_D3DXLoadVolumeFromMemory(Guest* g)
{
    uint32_t dst = ARG(0), src = ARG(3), rpitch = ARG(5), spitch = ARG(6), sbox = ARG(8);
    uint32_t lb = gheap_alloc(12, 1);
    uint32_t a[3] = { lb, ARG(2), 0 };
    uint32_t hr = device_call(dst, 9, 3, a); /* IDirect3DVolume9::LockBox */
    if (!hr)
    {
        uint32_t drow = rd32(lb), dslice = rd32(lb + 4), bits = rd32(lb + 8);
        uint32_t h = sbox ? rd32(sbox + 12) - rd32(sbox + 4) : 1, d = sbox ? rd32(sbox + 20) - rd32(sbox + 16) : 1;
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < h; ++y)
                memcpy(GUEST_PTR(bits + z * dslice + y * drow), GUEST_PTR(src + z * spitch + y * rpitch),
                    rpitch < drow ? rpitch : drow);
        device_call(dst, 10, 0, NULL); /* UnlockBox */
    }
    gheap_free(lb);
    RET(hr, 11);
}

/* --- textures from image files in memory (ffxivboot's interface images) ----------------------------- */
#define D3DFMT_A8R8G8B8 21u
#define D3DFMT_X8R8G8B8 22u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* D3DXCreateTextureFromFileInMemoryEx(device, data, size, w, h, levels, usage, fmt, pool, filter,
 * mipfilter, colorkey, D3DXIMAGE_INFO*, palette, IDirect3DTexture9**): one level at the image's own
 * size and format. DDS (DXT1/3/5, 32-bit RGB) is copied as it is; PNG, JPEG, BMP and TGA are decoded
 * to A8R8G8B8, the colour key (ARGB) made transparent. */
static void sh_D3DXCreateTextureFromFileInMemoryEx(Guest* g)
{
    uint32_t dev = ARG(0), data = ARG(1), size = ARG(2), usage = ARG(6), pool = ARG(8), key = ARG(11), info = ARG(12),
             out = ARG(14);
    const uint8_t* src = GUEST_PTR(data);
    uint32_t w = 0, h = 0, fmt = 0, file_fmt = 0, pitch_src = 0, rows = 0;
    uint8_t* pixels = NULL; /* decoded BGRA */
    const uint8_t* copy = NULL;
    if (out)
        wr32(out, 0);
    if (size >= 128 && !memcmp(src, "DDS ", 4))
    {
        h = rd32(data + 12), w = rd32(data + 16);
        uint32_t pf_flags = rd32(data + 80), fourcc = rd32(data + 84), bits = rd32(data + 88);
        file_fmt = 4; /* D3DXIFF_DDS */
        copy = src + 128;
        if (pf_flags & 4) /* DDPF_FOURCC */
        {
            fmt = fourcc;
            uint32_t block = fourcc == FOURCC('D', 'X', 'T', '1') ? 8 : 16;
            pitch_src = ((w + 3) / 4) * block, rows = (h + 3) / 4;
        }
        else if (bits == 32)
        {
            fmt = rd32(data + 104) ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8; /* alpha mask */
            pitch_src = 4 * w, rows = h;
        }
        else
        {
            rt_log("[d3dx9] D3DXCreateTextureFromFileInMemoryEx: DDS with %u-bit pixels is not read yet\n", bits);
            RET(0x8876086Cu, 15); /* D3DERR_INVALIDCALL */
        }
    }
    else
    {
        int iw, ih, n;
        uint8_t* rgba = stbi_load_from_memory(src, (int)size, &iw, &ih, &n, 4);
        if (!rgba)
        {
            rt_log("[d3dx9] D3DXCreateTextureFromFileInMemoryEx: not an image this reads (%02x %02x %02x %02x)\n", src[0],
                src[1], src[2], src[3]);
            RET(0x88760B59u, 15); /* D3DXERR_INVALIDDATA */
        }
        w = (uint32_t)iw, h = (uint32_t)ih, fmt = D3DFMT_A8R8G8B8;
        file_fmt = src[0] == 0x89 ? 3 : src[0] == 0xFF ? 1 : src[0] == 'B' ? 0 : 2; /* PNG, JPG, BMP, TGA */
        pixels = malloc((size_t)w * h * 4);
        for (uint32_t i = 0; i < w * h; ++i)
        {
            uint8_t r = rgba[4 * i], gg = rgba[4 * i + 1], b = rgba[4 * i + 2], a = rgba[4 * i + 3];
            uint32_t argb = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | b;
            if (key && argb == key)
                a = 0;
            pixels[4 * i] = b, pixels[4 * i + 1] = gg, pixels[4 * i + 2] = r, pixels[4 * i + 3] = a;
        }
        stbi_image_free(rgba);
        copy = pixels, pitch_src = 4 * w, rows = h;
    }
    if (info)
    {
        wr32(info, w), wr32(info + 4, h), wr32(info + 8, 1), wr32(info + 12, 1), wr32(info + 16, fmt);
        wr32(info + 20, 3), wr32(info + 24, file_fmt); /* D3DRTYPE_TEXTURE */
    }
    uint32_t ptex = gheap_alloc(4, 1);
    uint32_t a[8] = { w, h, 1, usage, fmt, pool, ptex, 0 };
    uint32_t hr = device_call(dev, 23, 8, a); /* CreateTexture */
    uint32_t tex = rd32(ptex);
    gheap_free(ptex);
    if (!hr && tex)
    {
        uint32_t lr = gheap_alloc(8, 1);
        uint32_t lk[5] = { tex, 0, lr, 0, 0 };
        if (!guest_call(rd32(rd32(tex) + 4 * 19), 5, lk)) /* IDirect3DTexture9::LockRect */
        {
            uint32_t pitch = rd32(lr), bits = rd32(lr + 4);
            for (uint32_t y = 0; y < rows; ++y)
                memcpy(GUEST_PTR(bits + y * pitch), copy + (size_t)y * pitch_src, pitch_src < pitch ? pitch_src : pitch);
            uint32_t ul[2] = { tex, 0 };
            guest_call(rd32(rd32(tex) + 4 * 20), 2, ul); /* UnlockRect */
        }
        gheap_free(lr);
        if (out)
            wr32(out, tex);
    }
    free(pixels);
    RET(hr, 15);
}

/* screenshots and debug dumps: not written */
static void sh_D3DXSaveSurfaceToFileA(Guest* g) { RET(D3D_OK, 5); }
static void sh_D3DXSaveTextureToFileA(Guest* g) { RET(D3D_OK, 4); }
static void sh_D3DXSaveSurfaceToFileInMemory(Guest* g) { if (ARG(0)) wr32(ARG(0), 0); RET(0x80004001u, 5); }

static void sh_D3DXSaveSurfaceToFileW(Guest* g)
{
    rt_log("[d3dx9] D3DXSaveSurfaceToFileW: screenshots are not written yet\n");
    RET(D3D_OK, 5);
}

#define CT(name) { "d3dx9_41.dll", "ID3DXConstantTable::" #name, CT_##name }
static const ShimDef D3DX9[] = {
    { "d3dx9_41.dll", "D3DXGetShaderConstantTable", sh_D3DXGetShaderConstantTable },
    { "d3dx9_41.dll", "D3DXGetVertexShaderProfile", sh_D3DXGetVertexShaderProfile },
    { "d3dx9_41.dll", "D3DXGetPixelShaderProfile", sh_D3DXGetPixelShaderProfile },
    { "d3dx9_41.dll", "D3DXCompileShader", sh_D3DXCompileShader },
    { "d3dx9_41.dll", "D3DXCompileShaderFromFileA", sh_D3DXCompileShaderFromFileA },
    { "d3dx9_41.dll", "D3DXCreateTexture", sh_D3DXCreateTexture },
    { "d3dx9_41.dll", "D3DXCreateCubeTexture", sh_D3DXCreateCubeTexture },
    { "d3dx9_41.dll", "D3DXCreateVolumeTexture", sh_D3DXCreateVolumeTexture },
    { "d3dx9_41.dll", "D3DXLoadSurfaceFromMemory", sh_D3DXLoadSurfaceFromMemory },
    { "d3dx9_41.dll", "D3DXLoadVolumeFromMemory", sh_D3DXLoadVolumeFromMemory },
    { "d3dx9_41.dll", "D3DXSaveSurfaceToFileW", sh_D3DXSaveSurfaceToFileW },
    { "d3dx9_41.dll", "D3DXCreateTextureFromFileInMemoryEx", sh_D3DXCreateTextureFromFileInMemoryEx },
    { "d3dx9_41.dll", "D3DXSaveSurfaceToFileA", sh_D3DXSaveSurfaceToFileA },
    { "d3dx9_41.dll", "D3DXSaveTextureToFileA", sh_D3DXSaveTextureToFileA },
    { "d3dx9_41.dll", "D3DXSaveSurfaceToFileInMemory", sh_D3DXSaveSurfaceToFileInMemory },
    CT(QueryInterface), CT(AddRef), CT(Release), CT(GetBufferPointer), CT(GetBufferSize), CT(GetDesc),
    CT(GetConstantDesc), CT(GetSamplerIndex), CT(GetConstant), CT(GetConstantByName), CT(GetConstantElement),
    CT(SetDefaults), CT(SetValue), CT(SetBool), { "d3dx9_41.dll", "ID3DXConstantTable::SetBoolArray", int_array },
    CT(SetInt), { "d3dx9_41.dll", "ID3DXConstantTable::SetIntArray", int_array }, CT(SetFloat), CT(SetFloatArray),
    CT(SetVector), CT(SetVectorArray), CT(SetMatrix), CT(SetMatrixArray), CT(SetMatrixPointerArray),
    CT(SetMatrixTranspose), CT(SetMatrixTransposeArray), CT(SetMatrixTransposePointerArray),
    { NULL, NULL, NULL },
};

void d3dx9_init(void) { thunk_register(D3DX9); }
