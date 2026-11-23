// HAUNTEDCAT'S HOLDABLE
// Native (il2cpp) holdable mod menu for Gorilla Tag on Quest / Android arm64.
//
// How it works
//  * A constructor spawns a thread that waits for libil2cpp.so, then resolves everything by
//    NAME through the il2cpp C API (no hard-coded offsets, so it survives most game updates
//    unless the game renames the classes/fields used below).
//  * GorillaTagger.Update is hooked (Dobby) so all Unity calls happen on the main thread.
//  * The menu is a small panel parented to your LEFT controller. Y (left secondary) shows/hides it.
//    Poke a button with your RIGHT controller to use it.
//
// Local-player features only. Use it in modded / private lobbies.
//
// UNTESTED against a live game build: the cfg:: values below (menu pose, class and field
// names) are the things you may need to tune.

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dobby.h"

#define TAG "HAUNTEDCATS_HOLDABLE"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct V3 { float x, y, z; };
struct Q  { float x, y, z, w; };
struct Col { float r, g, b, a; };

// ------------------------------------------------------------------ config
namespace cfg {
constexpr const char* TITLE = "HAUNTEDCAT'S HOLDABLE";

// Pose of the menu relative to the left controller (tune to taste).
constexpr V3 MENU_POS   = {0.00f, 0.06f, 0.10f};
constexpr V3 MENU_EULER = {-60.0f, 0.0f, 0.0f};

constexpr float PANEL_W = 0.24f, PANEL_H = 0.20f;
constexpr float BTN_W = 0.20f, BTN_H = 0.035f, BTN_STEP = 0.043f, BTN_TOP = 0.035f;
constexpr float TOUCH_DEPTH = 0.035f;  // how close (m) the right controller must be to the button plane

const Col C_PANEL = {0.07f, 0.03f, 0.12f, 1.0f};
const Col C_OFF   = {0.28f, 0.13f, 0.45f, 1.0f};
const Col C_ON    = {0.30f, 0.90f, 0.50f, 1.0f};
const Col C_TITLE = {0.85f, 0.65f, 1.00f, 1.0f};
const Col C_TEXT  = {1.00f, 1.00f, 1.00f, 1.0f};
}  // namespace cfg

// ------------------------------------------------------------------ il2cpp api
static struct Il2 {
    void*  (*domain_get)();
    void*  (*thread_attach)(void*);
    void** (*domain_get_assemblies)(void*, size_t*);
    void*  (*assembly_get_image)(void*);
    void*  (*class_from_name)(void*, const char*, const char*);
    void*  (*class_get_method_from_name)(void*, const char*, int);
    void*  (*class_get_field_from_name)(void*, const char*);
    void   (*field_get_value)(void*, void*, void*);
    void   (*field_set_value)(void*, void*, void*);
    void   (*field_static_get_value)(void*, void*);
    void*  (*runtime_invoke)(void*, void*, void**, void**);
    void*  (*string_new)(const char*);
    void*  (*object_new)(void*);
    void*  (*object_unbox)(void*);
    void*  (*class_get_type)(void*);
    void*  (*type_get_object)(void*);
} il;

static bool ResolveApi() {
    void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    bool ok = true;
#define RESOLVE(n)                                       \
    *(void**)&il.n = dlsym(h, "il2cpp_" #n);             \
    if (!il.n) { LOG("missing il2cpp_" #n); ok = false; }
    RESOLVE(domain_get) RESOLVE(thread_attach) RESOLVE(domain_get_assemblies)
    RESOLVE(assembly_get_image) RESOLVE(class_from_name) RESOLVE(class_get_method_from_name)
    RESOLVE(class_get_field_from_name) RESOLVE(field_get_value) RESOLVE(field_set_value)
    RESOLVE(field_static_get_value) RESOLVE(runtime_invoke) RESOLVE(string_new)
    RESOLVE(object_new) RESOLVE(object_unbox) RESOLVE(class_get_type) RESOLVE(type_get_object)
#undef RESOLVE
    return ok;
}

static void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** asms = il.domain_get_assemblies(il.domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* k = il.class_from_name(il.assembly_get_image(asms[i]), ns, name);
        if (k) return k;
    }
    return nullptr;
}

static void* Meth(void* k, const char* name, int argc) {
    void* m = k ? il.class_get_method_from_name(k, name, argc) : nullptr;
    if (!m) LOG("method not found: %s/%d", name, argc);
    return m;
}

static void* Fld(void* k, const char* name) {
    void* f = k ? il.class_get_field_from_name(k, name) : nullptr;
    if (!f) LOG("field not found: %s", name);
    return f;
}

static void* Call(void* m, void* self, void** args = nullptr) {
    if (!m) return nullptr;
    void* exc = nullptr;
    void* r = il.runtime_invoke(m, self, args, &exc);
    if (exc) { LOG("managed exception"); return nullptr; }
    return r;
}

template <class T> static T Unbox(void* o) {
    T v{};
    if (o) v = *(T*)il.object_unbox(o);
    return v;
}

// ------------------------------------------------------------------ resolved game/unity bits
static struct Ctx {
    void *cGameObject, *cTransform, *cComponent, *cObject, *cCollider, *cRenderer, *cMaterial,
         *cTextMesh, *cFont, *cResources;
    void *cPlayer, *cPoller, *cTagger;
    void *fPlayerLeft, *fPlayerRight, *fPollerInstance, *fPollerLeftY;
    void *mPlayerInstance;
} c;

static std::atomic<bool> g_ready{false};
static bool g_built = false, g_dead = false, g_visible = true, g_prevY = false;
static int  g_buildTries = 0;
static void *g_root = nullptr, *g_rootT = nullptr, *g_rightT = nullptr;

// ------------------------------------------------------------------ small unity wrappers
static void* TypeObj(void* klass) { return il.type_get_object(il.class_get_type(klass)); }

static void* GetTransform(void* go) { return Call(Meth(c.cGameObject, "get_transform", 0), go); }

static void SetV3(const char* setter, void* t, V3 v) { void* a[] = {&v}; Call(Meth(c.cTransform, setter, 1), t, a); }

static Q Mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Q Euler(V3 d) {  // Unity order: Z, X, Y
    const float k = 0.0174532925f * 0.5f;
    Q qx{std::sin(d.x * k), 0, 0, std::cos(d.x * k)};
    Q qy{0, std::sin(d.y * k), 0, std::cos(d.y * k)};
    Q qz{0, 0, std::sin(d.z * k), std::cos(d.z * k)};
    return Mul(Mul(qy, qx), qz);
}

static void Parent(void* t, void* parent, V3 pos, V3 scale, Q rot = {0, 0, 0, 1}) {
    bool keepWorld = false;
    void* a[] = {parent, &keepWorld};
    Call(Meth(c.cTransform, "SetParent", 2), t, a);
    SetV3("set_localPosition", t, pos);
    { void* r[] = {&rot}; Call(Meth(c.cTransform, "set_localRotation", 1), t, r); }
    SetV3("set_localScale", t, scale);
}

static void SetColor(void* go, Col col) {
    void* a[] = {TypeObj(c.cRenderer)};
    void* rend = Call(Meth(c.cGameObject, "GetComponent", 1), go, a);
    if (!rend) return;
    void* mat = Call(Meth(c.cRenderer, "get_material", 0), rend);
    void* b[] = {&col};
    Call(Meth(c.cMaterial, "set_color", 1), mat, b);
}

static void* NewGameObject(const char* name) {
    void* go = il.object_new(c.cGameObject);
    void* a[] = {il.string_new(name)};
    Call(Meth(c.cGameObject, ".ctor", 1), go, a);
    return go;
}

static void* MakeBox(void* parentT, V3 pos, V3 scale, Col col) {
    int cube = 3;  // PrimitiveType.Cube
    void* a[] = {&cube};
    void* go = Call(Meth(c.cGameObject, "CreatePrimitive", 1), nullptr, a);
    if (!go) return nullptr;
    void* t[] = {TypeObj(c.cCollider)};
    void* col2 = Call(Meth(c.cGameObject, "GetComponent", 1), go, t);
    if (col2) { void* d[] = {col2}; Call(Meth(c.cObject, "Destroy", 1), nullptr, d); }  // purely visual
    Parent(GetTransform(go), parentT, pos, scale);
    SetColor(go, col);
    return go;
}

static void* g_font = nullptr;
static void* GetFont() {
    if (g_font) return g_font;
    const char* names[] = {"LegacyRuntime.ttf", "Arial.ttf"};
    for (auto n : names) {
        void* a[] = {TypeObj(c.cFont), il.string_new(n)};
        g_font = Call(Meth(c.cResources, "GetBuiltinResource", 2), nullptr, a);
        if (g_font) break;
    }
    return g_font;
}

static void SetText(void* tm, const char* s) {
    void* a[] = {il.string_new(s)};
    Call(Meth(c.cTextMesh, "set_text", 1), tm, a);
}

static void* MakeLabel(void* parentT, const char* text, V3 pos, float charSize, Col col) {
    void* go = NewGameObject("lbl");
    void* a[] = {TypeObj(c.cTextMesh)};
    void* tm = Call(Meth(c.cGameObject, "AddComponent", 1), go, a);
    if (!tm) return nullptr;
    int fontSize = 64, anchor = 4, align = 1;  // MiddleCenter, Center
    float cs = charSize;
    void* f = GetFont();
    { void* x[] = {f};         Call(Meth(c.cTextMesh, "set_font", 1), tm, x); }
    { void* x[] = {&fontSize}; Call(Meth(c.cTextMesh, "set_fontSize", 1), tm, x); }
    { void* x[] = {&anchor};   Call(Meth(c.cTextMesh, "set_anchor", 1), tm, x); }
    { void* x[] = {&align};    Call(Meth(c.cTextMesh, "set_alignment", 1), tm, x); }
    { void* x[] = {&cs};       Call(Meth(c.cTextMesh, "set_characterSize", 1), tm, x); }
    { void* x[] = {&col};      Call(Meth(c.cTextMesh, "set_color", 1), tm, x); }
    SetText(tm, text);
    // font material so the text actually renders
    void* rt[] = {TypeObj(c.cRenderer)};
    void* rend = Call(Meth(c.cGameObject, "GetComponent", 1), go, rt);
    void* fm = Call(Meth(c.cFont, "get_material", 0), f);
    if (rend && fm) { void* x[] = {fm}; Call(Meth(c.cRenderer, "set_material", 1), rend, x); }
    Parent(GetTransform(go), parentT, pos, {1, 1, 1});
    return tm;
}

// ------------------------------------------------------------------ menu entries
// Each entry cycles OFF -> step1 -> step2 ... -> OFF and multiplies the game's ORIGINAL value of
// the named float field on the local player (original is captured, so nothing is hard-coded).
struct Entry {
    const char*  name;
    const char*  field;  // nullptr = special button (RESET ALL)
    const float* steps;
    int          n;
    int          idx = 0;
    float        orig = 0;
    bool         have = false;
    void*        fieldInfo = nullptr;
    void*        box = nullptr;
    void*        label = nullptr;
    V3           pos{};
};

static const float SPEED_STEPS[] = {1.15f, 1.30f, 1.50f};
static const float JUMP_STEPS[]  = {1.15f, 1.30f, 1.50f};

static Entry g_entries[] = {
    {"Speed", "maxJumpSpeed", SPEED_STEPS, 3},
    {"Jump", "jumpMultiplier", JUMP_STEPS, 3},
    {"RESET ALL", nullptr, nullptr, 0},
};
constexpr int N_ENTRIES = sizeof(g_entries) / sizeof(g_entries[0]);

static void RefreshLabel(Entry& e) {
    char buf[48];
    if (!e.field) std::snprintf(buf, sizeof buf, "%s", e.name);
    else if (e.idx == 0) std::snprintf(buf, sizeof buf, "%s: OFF", e.name);
    else std::snprintf(buf, sizeof buf, "%s: x%.2f", e.name, e.steps[e.idx - 1]);
    SetText(e.label, buf);
    if (e.field) SetColor(e.box, e.idx ? cfg::C_ON : cfg::C_OFF);
}

static void ApplyField(void* player, Entry& e) {
    if (!e.field || !e.fieldInfo) return;
    if (!e.have) {  // capture original the first time we touch it
        il.field_get_value(player, e.fieldInfo, &e.orig);
        e.have = true;
    }
    float v = e.idx ? e.orig * e.steps[e.idx - 1] : e.orig;
    il.field_set_value(player, e.fieldInfo, &v);
}

static void Press(Entry& e, void* player) {
    if (!e.field) {  // RESET ALL
        for (auto& x : g_entries) {
            if (!x.field) continue;
            x.idx = 0;
            ApplyField(player, x);
            RefreshLabel(x);
        }
        return;
    }
    e.idx = (e.idx + 1) % (e.n + 1);
    ApplyField(player, e);
    RefreshLabel(e);
}

// ------------------------------------------------------------------ build + per-frame logic
static void* PlayerInstance() { return Call(c.mPlayerInstance, nullptr); }

static bool BuildMenu(void* player) {
    void* leftT = nullptr;
    il.field_get_value(player, c.fPlayerLeft, &leftT);
    il.field_get_value(player, c.fPlayerRight, &g_rightT);
    if (!leftT || !g_rightT) return false;

    g_root = NewGameObject("HAUNTEDCATS_HOLDABLE");
    g_rootT = GetTransform(g_root);
    Parent(g_rootT, leftT, cfg::MENU_POS, {1, 1, 1}, Euler(cfg::MENU_EULER));

    MakeBox(g_rootT, {0, 0, 0.004f}, {cfg::PANEL_W, cfg::PANEL_H, 0.006f}, cfg::C_PANEL);
    MakeLabel(g_rootT, cfg::TITLE, {0, cfg::PANEL_H * 0.5f - 0.025f, -0.002f}, 0.0035f, cfg::C_TITLE);

    for (int i = 0; i < N_ENTRIES; i++) {
        Entry& e = g_entries[i];
        e.pos = {0, cfg::BTN_TOP - i * cfg::BTN_STEP, 0};
        e.box = MakeBox(g_rootT, e.pos, {cfg::BTN_W, cfg::BTN_H, 0.008f}, cfg::C_OFF);
        e.label = MakeLabel(g_rootT, e.name, {e.pos.x, e.pos.y, -0.006f}, 0.0028f, cfg::C_TEXT);
        if (e.field) e.fieldInfo = Fld(c.cPlayer, e.field);
        if (!e.box || !e.label) return false;
        RefreshLabel(e);
    }
    LOG("menu built");
    return true;
}

static void SetVisible(bool v) {
    g_visible = v;
    void* a[] = {&v};
    Call(Meth(c.cGameObject, "SetActive", 1), g_root, a);
}

static bool g_touching[N_ENTRIES];

static void HandleTouch(void* player) {
    V3 w = Unbox<V3>(Call(Meth(c.cTransform, "get_position", 0), g_rightT));
    void* a[] = {&w};
    V3 p = Unbox<V3>(Call(Meth(c.cTransform, "InverseTransformPoint", 1), g_rootT, a));
    for (int i = 0; i < N_ENTRIES; i++) {
        Entry& e = g_entries[i];
        bool inside = std::fabs(p.x - e.pos.x) < cfg::BTN_W * 0.5f && std::fabs(p.y - e.pos.y) < cfg::BTN_H * 0.5f &&
                      std::fabs(p.z) < cfg::TOUCH_DEPTH;
        if (inside && !g_touching[i]) Press(e, player);  // rising edge only
        g_touching[i] = inside;
    }
}

static void Tick() {
    if (!g_ready || g_dead) return;
    void* player = PlayerInstance();
    if (!player) return;
    if (!g_built) {
        if (BuildMenu(player)) g_built = true;
        else if (++g_buildTries > 300) { LOG("giving up building menu"); g_dead = true; }
        return;
    }
    void* poller = nullptr;
    il.field_static_get_value(c.fPollerInstance, &poller);
    bool y = false;
    if (poller && c.fPollerLeftY) il.field_get_value(poller, c.fPollerLeftY, &y);
    if (y && !g_prevY) SetVisible(!g_visible);
    g_prevY = y;

    if (g_visible) HandleTouch(player);
    for (auto& e : g_entries) if (e.idx) ApplyField(player, e);  // game may reset values; keep them applied
}

// ------------------------------------------------------------------ hook + init
static void (*orig_Update)(void*, void*) = nullptr;
static void hk_Update(void* self, void* mi) {
    orig_Update(self, mi);
    Tick();
}

static void* InitThread(void*) {
    for (int i = 0; i < 240 && !ResolveApi(); i++) usleep(500 * 1000);
    if (!il.domain_get) return nullptr;

    // wait for Assembly-CSharp to be loaded
    for (int i = 0; i < 240; i++) {
        il.thread_attach(il.domain_get());
        c.cTagger = FindClass("", "GorillaTagger");
        if (c.cTagger) break;
        usleep(500 * 1000);
    }
    if (!c.cTagger) { LOG("GorillaTagger not found"); return nullptr; }

    c.cGameObject = FindClass("UnityEngine", "GameObject");
    c.cTransform  = FindClass("UnityEngine", "Transform");
    c.cComponent  = FindClass("UnityEngine", "Component");
    c.cObject     = FindClass("UnityEngine", "Object");
    c.cCollider   = FindClass("UnityEngine", "Collider");
    c.cRenderer   = FindClass("UnityEngine", "Renderer");
    c.cMaterial   = FindClass("UnityEngine", "Material");
    c.cTextMesh   = FindClass("UnityEngine", "TextMesh");
    c.cFont       = FindClass("UnityEngine", "Font");
    c.cResources  = FindClass("UnityEngine", "Resources");

    c.cPlayer = FindClass("GorillaLocomotion", "GTPlayer");
    if (!c.cPlayer) c.cPlayer = FindClass("GorillaLocomotion", "Player");  // older builds
    c.cPoller = FindClass("", "ControllerInputPoller");

    c.mPlayerInstance = Meth(c.cPlayer, "get_Instance", 0);
    c.fPlayerLeft     = Fld(c.cPlayer, "leftControllerTransform");
    c.fPlayerRight    = Fld(c.cPlayer, "rightControllerTransform");
    c.fPollerInstance = Fld(c.cPoller, "instance");
    c.fPollerLeftY    = Fld(c.cPoller, "leftControllerSecondaryButton");

    if (!c.cGameObject || !c.cTransform || !c.cPlayer || !c.mPlayerInstance || !c.fPlayerLeft ||
        !c.fPlayerRight || !c.fPollerInstance) {
        LOG("required classes/fields missing, not hooking");
        return nullptr;
    }

    void* m = Meth(c.cTagger, "Update", 0);
    if (!m) return nullptr;
    void* target = *(void**)m;  // MethodInfo::methodPointer is the first member
    if (DobbyHook(target, (void*)hk_Update, (void**)&orig_Update) != 0) { LOG("hook failed"); return nullptr; }
    g_ready = true;
    LOG("%s loaded", cfg::TITLE);
    return nullptr;
}

__attribute__((constructor)) static void OnLoad() {
    pthread_t t;
    pthread_create(&t, nullptr, InitThread, nullptr);
    pthread_detach(t);
}

