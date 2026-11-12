// A small JavaVM for libroblox.so: classes, strings, arrays and method/field ids, with every JNIEnv slot in the
// SysV convention. There is no Java: calls into "Java" are logged once per method and answer zero/null until a
// real answer is written for them (Mocktail's src/jnivm, Apache-2.0, is the reference for what Roblox expects).
// Unwritten non-call slots stop the run with their name, like a missing import.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim.h"
#include "jni_slots.h"

typedef int32_t jint; typedef int64_t jlong;
#define OBJ_MAGIC 0x4a4f424au  // 'JOBJ'
enum Kind { K_OBJ, K_CLASS, K_STRING, K_ARRAY, K_BUFFER };
typedef struct Cls Cls;
typedef struct Field { char *name; uint64_t v; struct Field *next; } Field;  // v: raw bits (float in the low 32)
typedef struct { uint32_t magic, kind; Cls *cls; Field *fields; } Obj;
typedef struct { const char *name, *sig; void *fn; } Native;
struct Cls { Obj o; char *name; Native *natives; int nnat; Cls *next; Obj *single; };
typedef struct { Obj o; char *utf; } Str;
typedef struct { Obj o; int32_t len, elem; void *data; } Arr;  // elem 0 = object array (data holds Obj*)
typedef struct { Obj o; void *addr; int64_t cap; } Buf;
typedef struct Mid { Cls *cls; char *name, *sig; int stat, field, logged; struct Mid *next; } Mid;

static SRWLOCK lock = SRWLOCK_INIT;
static Cls *classes; static Mid *ids;
static Cls *java_class;  // java/lang/Class: the class of class objects

static Cls *intern_class(const char *name) {
    AcquireSRWLockExclusive(&lock);
    Cls *c = classes;
    for (; c; c = c->next) if (!strcmp(c->name, name)) break;
    if (!c) {
        c = calloc(1, sizeof *c);
        c->o = (Obj){ OBJ_MAGIC, K_CLASS, java_class }; c->name = _strdup(name);
        c->next = classes; classes = c;
    }
    ReleaseSRWLockExclusive(&lock);
    return c;
}
static Mid *intern_id(Cls *c, const char *name, const char *sig, int stat, int field) {
    AcquireSRWLockExclusive(&lock);
    Mid *m = ids;
    for (; m; m = m->next) if (m->cls == c && m->stat == stat && m->field == field && !strcmp(m->name, name) && !strcmp(m->sig, sig)) break;
    if (!m) {
        m = calloc(1, sizeof *m);
        *m = (Mid){ c, _strdup(name), _strdup(sig), stat, field, 0, ids }; ids = m;
    }
    ReleaseSRWLockExclusive(&lock);
    return m;
}
static Cls *class_of_name(const char *n) { return intern_class(n); }
static Str *new_string(const char *utf) {
    Str *s = calloc(1, sizeof *s);
    s->o = (Obj){ OBJ_MAGIC, K_STRING, class_of_name("java/lang/String") }; s->utf = _strdup(utf ? utf : "");
    return s;
}
static int is_obj(const void *p) { return p && ((const Obj *)p)->magic == OBJ_MAGIC; }
static const char *cname(const void *p) {
    if (!is_obj(p)) return "?";
    const Obj *o = p;
    return o->kind == K_CLASS ? ((const Cls *)o)->name : o->cls ? o->cls->name : "?";
}

// --- fields (instance fields on the object, static fields on the class object) ----------------------------
static uint64_t field_get(void *o, const char *name) {
    if (!is_obj(o)) return 0;
    uint64_t v = 0;
    AcquireSRWLockShared(&lock);
    for (Field *f = ((Obj *)o)->fields; f; f = f->next) if (!strcmp(f->name, name)) { v = f->v; break; }
    ReleaseSRWLockShared(&lock);
    return v;
}
static void field_set(void *o, const char *name, uint64_t v) {
    if (!is_obj(o)) return;
    AcquireSRWLockExclusive(&lock);
    Field *f = ((Obj *)o)->fields;
    for (; f; f = f->next) if (!strcmp(f->name, name)) break;
    if (!f) { f = calloc(1, sizeof *f); f->name = _strdup(name); f->next = ((Obj *)o)->fields; ((Obj *)o)->fields = f; }
    f->v = v;
    ReleaseSRWLockExclusive(&lock);
}
static void set_str(void *o, const char *name, const char *s) { field_set(o, name, (uint64_t)new_string(s)); }
static void set_float(void *o, const char *name, float f) { uint32_t b; memcpy(&b, &f, 4); field_set(o, name, b); }
static Obj *new_obj(Cls *c) { Obj *o = calloc(1, sizeof *o); *o = (Obj){ OBJ_MAGIC, K_OBJ, c }; return o; }
static Obj *singleton(const char *cls) {  // one shared object per class, like Mocktail's SingletonObject
    Cls *c = intern_class(cls);
    if (!c->single) {
        Obj *o = new_obj(c);
        if (InterlockedCompareExchangePointer((void **)&c->single, o, 0)) free(o);
    }
    return c->single;
}

// --- answers to Java calls, by method name ----------------------------------------------------------------
// Device identity is honest: this is Skatestrap on Windows, presenting an Android 13 (API 33) runtime.
static uint64_t a_device_static_params(Obj *self) {
    static Obj *o;
    if (o) return (uint64_t)o;
    Obj *d = new_obj(intern_class("com/roblox/engine/jni/model/DeviceStaticParams"));
    const char *skatestrap_version(void), *ver = skatestrap_version();
    set_str(d, "osVersion", "Android 13"); set_str(d, "deviceName", "Skatestrap");
    set_str(d, "appVersion", ver); set_str(d, "manufacturer", "Skatestrap");
    set_str(d, "model", "Skatestrap Windows"); set_str(d, "brand", "Skatestrap"); set_str(d, "device", "skatestrap");
    set_str(d, "deviceSku", "skatestrap"); set_str(d, "appBuildVariant", "release");
    set_str(d, "socModel", "x86_64"); set_str(d, "soc_model", "x86_64");
    field_set(d, "cpu64Bit", 1);
    field_set(d, "screenWidth", GetSystemMetrics(SM_CXSCREEN)); field_set(d, "screenHeight", GetSystemMetrics(SM_CYSCREEN));
    field_set(d, "screenDensityDpi", 160); field_set(d, "apiVersion", 33); field_set(d, "sdkVersion", 33);
    set_float(d, "density", 1); set_float(d, "scaledDensity", 1); set_float(d, "xdpi", 160); set_float(d, "ydpi", 160);
    o = d;
    return (uint64_t)o;
}
static uint64_t a_class_loader(Obj *self) { return (uint64_t)singleton("java/lang/ClassLoader"); }
static uint64_t a_locale(Obj *self) { static void *s; if (!s) s = new_string("en_US"); return (uint64_t)s; }  // shortcut: fixed locale, read GetUserDefaultLocaleName when translations matter
// login identity for NativeUserJavaInterface (Mocktail: MOCKTAIL_ROBLOX_USER_ID / _USERNAME); 0 and "" = logged out
static uint64_t a_user_id(Obj *self) { const char *v = getenv("SKATESTRAP_USER_ID"); long long id = v ? _atoi64(v) : 0; return id > 0 ? (uint64_t)id : 0; }
static uint64_t a_username(Obj *self) { static void *s; if (!s) { const char *v = getenv("SKATESTRAP_USERNAME"); s = new_string(v ? v : ""); } return (uint64_t)s; }
static uint64_t a_activity_thread(Obj *self) { return (uint64_t)singleton("android/app/ActivityThread"); }
static uint64_t a_application(Obj *self) { return (uint64_t)singleton("android/app/Application"); }
// shortcut: one empty store for every name, nothing saved; back it with a file when settings must persist
static uint64_t a_prefs(Obj *self) { return (uint64_t)singleton("android/content/SharedPreferences"); }
static uint64_t a_editor(Obj *self) { return (uint64_t)singleton("android/content/SharedPreferences$Editor"); }
static uint64_t a_true(Obj *self) { return 1; }
typedef uint64_t (*Answer)(Obj *self);
static const struct { const char *name; Answer fn; } answers[] = {
    { "getDeviceStaticParams", a_device_static_params },
    { "getClassLoader", a_class_loader },
    { "getLocale", a_locale },
    { "getUserId", a_user_id },
    { "getUsername", a_username }, { "getLastLoggedInUser", a_username },
    { "getDisplayName", a_username }, { "getAlternateName", a_username },
    { "currentActivityThread", a_activity_thread },
    { "getApplication", a_application },
    { "getApplicationContext", a_application },
    { "getSharedPreferences", a_prefs },
    { "edit", a_editor },
    { "checkInit", a_true },  // org/fmod/FMOD: the Java side is "ready" (AudioDevice below plays the sound)
};
static Answer answer_for(const char *name) {
    for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) if (!strcmp(answers[i].name, name)) return answers[i].fn;
    return 0;
}

// --- slots that stop the run -------------------------------------------------------------------------------
static SYSV void missing_slot(const char *name) {
    printf("\nJNI slot not written: %s\n", name);
    fflush(stdout);
    ExitProcess(5);
}
#define TRAP(n) static SYSV void t_##n(void) { missing_slot(#n); }
JNI_ENV_SLOTS(TRAP)
JNI_VM_SLOTS(TRAP)

// --- the env and the vm ----------------------------------------------------------------------------------
static void *env_table[JNI_ENV_COUNT], *vm_table[JNI_VM_COUNT];
static void **vm = vm_table;  // JavaVM* points at this
static __thread void **env = env_table;  // JNIEnv*: one per thread, as on Android (Roblox hooks each thread's table separately)
#define IDX_ENV(n) idx_##n,
enum { idx_r0, idx_r1, idx_r2, idx_r3, JNI_ENV_SLOTS(IDX_ENV) };
#define IDX_VM(n) vidx_##n,
enum { vidx_r0, vidx_r1, vidx_r2, JNI_VM_SLOTS(IDX_VM) };

static SYSV jint j_GetVersion(void *e) { return 0x10006; }
static SYSV Cls *j_FindClass(void *e, const char *name) { return intern_class(name); }
static SYSV Cls *j_GetSuperclass(void *e, Cls *c) { return strcmp(c->name, "java/lang/Object") ? intern_class("java/lang/Object") : 0; }
static SYSV int j_IsAssignableFrom(void *e, Cls *a, Cls *b) { return a == b || !strcmp(b->name, "java/lang/Object"); }
static SYSV jint j_Throw(void *e, Obj *t) { printf("JNI Throw %s (not modelled)\n", cname(t)); return 0; }
static SYSV jint j_ThrowNew(void *e, Cls *c, const char *msg) { printf("JNI ThrowNew %s: %s (not modelled)\n", c->name, msg ? msg : ""); return 0; }
static SYSV void *j_null(void *e) { return 0; }
static SYSV void j_nop(void *e) {}
static SYSV int j_zero(void *e) { return 0; }
static SYSV void *j_same(void *e, void *o) { return o; }
static SYSV void *j_PopLocalFrame(void *e, void *res) { return res; }
static SYSV int j_IsSameObject(void *e, void *a, void *b) { return a == b; }
static SYSV Cls *j_GetObjectClass(void *e, Obj *o) { return !is_obj(o) ? 0 : o->kind == K_CLASS ? java_class : o->cls; }
static SYSV int j_IsInstanceOf(void *e, Obj *o, Cls *c) { return !o || (is_obj(o) && (o->cls == c || !strcmp(c->name, "java/lang/Object"))); }
static SYSV jint j_GetObjectRefType(void *e, void *o) { return o ? 2 : 0; }  // everything lives forever: "global"

static Mid *log_id(Mid *m, const char *what) {
    if (!m->logged) { m->logged = 1; printf("JNI %s %s.%s %s\n", what, m->cls->name, m->name, m->sig); }
    return m;
}
static SYSV Mid *j_GetMethodID(void *e, Cls *c, const char *n, const char *s) { return intern_id(c, n, s, 0, 0); }
static SYSV Mid *j_GetStaticMethodID(void *e, Cls *c, const char *n, const char *s) { return intern_id(c, n, s, 1, 0); }
static SYSV Mid *j_GetFieldID(void *e, Cls *c, const char *n, const char *s) { return intern_id(c, n, s, 0, 1); }
static SYSV Mid *j_GetStaticFieldID(void *e, Cls *c, const char *n, const char *s) { return intern_id(c, n, s, 1, 1); }

// --- FMOD's sound output: org/fmod/AudioDevice (the APK's AudioTrack wrapper; Mocktail's fmod_java_audio_runtime)
// on waveOut. init(channels, rate, block frames, block count) opens 16-bit PCM; write(byte[], size) queues one mix
// block and blocks while every buffer is still playing, like AudioTrack's WRITE_BLOCKING; close() stops it.
#include <mmsystem.h>
static struct { HWAVEOUT h; WAVEHDR *hdr; int n, next, block; HANDLE done; } ad;
static uint64_t ad_init(int ch, int rate, int frames, int count) {
    if (ad.h || !(ch == 1 || ch == 2 || ch == 6 || ch == 8) || rate <= 0 || frames <= 0 || count <= 0 || count > 256) return 0;
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, (WORD)ch, (DWORD)rate, (DWORD)(rate * ch * 2), (WORD)(ch * 2), 16, 0 };
    ad.done = CreateEventA(0, FALSE, FALSE, 0);
    if (waveOutOpen(&ad.h, WAVE_MAPPER, &f, (DWORD_PTR)ad.done, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        printf("  [audio] waveOut refused %d ch %d Hz\n", ch, rate);
        CloseHandle(ad.done); ad.h = 0; return 0;
    }
    ad.block = frames * ch * 2; ad.n = count < 3 ? 3 : count; ad.next = 0;
    ad.hdr = calloc(ad.n, sizeof *ad.hdr);
    for (int i = 0; i < ad.n; i++) {
        ad.hdr[i].lpData = calloc(1, ad.block); ad.hdr[i].dwBufferLength = ad.block;
        waveOutPrepareHeader(ad.h, &ad.hdr[i], sizeof *ad.hdr);
    }
    printf("  [audio] FMOD output: %d ch %d Hz, %d blocks of %d frames (waveOut)\n", ch, rate, ad.n, frames);
    return 1;
}
static void ad_write(Arr *a, int size) {
    if (!ad.h || !is_obj(a) || size <= 0 || size > ad.block || size > a->len) return;
    WAVEHDR *h = &ad.hdr[ad.next];
    while (ad.h && (h->dwFlags & WHDR_INQUEUE)) WaitForSingleObject(ad.done, 20);
    if (!ad.h) return;
    memcpy(h->lpData, a->data, size); h->dwBufferLength = size;
    waveOutWrite(ad.h, h, sizeof *h);
    ad.next = (ad.next + 1) % ad.n;
}
static void ad_close(void) {
    HWAVEOUT h = ad.h;
    if (!h) return;
    ad.h = 0; waveOutReset(h);
    for (int i = 0; i < ad.n; i++) { waveOutUnprepareHeader(h, &ad.hdr[i], sizeof *ad.hdr); free(ad.hdr[i].lpData); }
    free(ad.hdr); ad.hdr = 0; waveOutClose(h); CloseHandle(ad.done);
}
static int is_audio_device(Mid *m) { return !strcmp(m->cls->name, "org/fmod/AudioDevice"); }
// MessageBus.doSubscribeRaw hands its messages to RawCallback.run(String) (Mocktail jnivm DispatchMessageBusRawCallback)
static const char RAW_CALLBACK[] = "com/roblox/universalapp/messagebus/RawCallback";
static int is_raw_run(void *o, Mid *m) { return !strcmp(m->name, "run") && (!strcmp(m->cls->name, RAW_CALLBACK) || (is_obj(o) && !strcmp(cname(o), RAW_CALLBACK))); }
// a Roblox TextBox took focus (showKeyboard: handle, ?, UTF-8 text, info) or let it go (hideKeyboard)
static int is_keyboard(Mid *m) { return !strcmp(m->name, "showKeyboard") || !strcmp(m->name, "hideKeyboard"); }
static int reads_args(void *o, Mid *m) { return is_audio_device(m) || is_raw_run(o, m) || is_keyboard(m); }
void (*jni_on_raw_message)(void *callback, const char *utf);  // set by startup.c
void (*jni_on_cookie_sync)(void);  // MainGameActivity.syncCookiesFromEngine: the app signed in (or out)
void (*jni_on_keyboard)(int show, uint64_t handle, const char *utf, int len, void *info);  // set by startup.c

// Calls into Java: (env, obj|cls, mid, ...) or nonvirtual (env, obj, cls, mid, ...).
// Arguments (v: the first four, as 64-bit slots) are read only for the methods that use them (reads_args).
static int field_has(void *o, const char *name);
static Arr *new_array(const char *cls, int elem, jint n);
static uint64_t call(void *o, Mid *m, const uint64_t *v) {
    if (v && is_raw_run(o, m)) {
        if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s (answered)\n", m->cls->name, m->name, m->sig); }
        Str *s = (Str *)v[0];
        if (jni_on_raw_message && is_obj(s) && s->o.kind == K_STRING) jni_on_raw_message(o, s->utf);
        return 0;
    }
    if (jni_on_cookie_sync && !strcmp(m->name, "syncCookiesFromEngine")) {
        if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s (answered)\n", m->cls->name, m->name, m->sig); }
        jni_on_cookie_sync(); return 0;
    }
    if (jni_on_keyboard && is_keyboard(m)) {
        if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s (answered)\n", m->cls->name, m->name, m->sig); }
        Arr *t = v && m->name[0] == 's' && is_obj((void *)v[2]) ? (Arr *)v[2] : 0;
        jni_on_keyboard(m->name[0] == 's', v ? v[0] : 0, t ? t->data : "", t ? t->len : 0, v && m->name[0] == 's' ? (void *)v[3] : 0);
        return 0;
    }
    if (!strcmp(m->name, "getBytes") && is_obj(o) && ((Obj *)o)->kind == K_STRING) {  // TextBox text back to the engine (UTF-8)
        if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s (answered)\n", m->cls->name, m->name, m->sig); }
        const char *u = ((Str *)o)->utf; int n = (int)strlen(u);
        Arr *a = new_array("[B", 1, n); memcpy(a->data, u, n); return (uint64_t)a;
    }
    if (v && is_audio_device(m)) {
        if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s (answered)\n", m->cls->name, m->name, m->sig); }
        if (!strcmp(m->name, "init")) return ad_init((int)v[0], (int)v[1], (int)v[2], (int)v[3]);
        if (!strcmp(m->name, "write")) { ad_write((Arr *)v[0], (int)v[1]); return 0; }
        if (!strcmp(m->name, "close")) { ad_close(); return 0; }
    }
    Answer a = answer_for(m->name);
    int getter = !a && !strncmp(m->sig, "()", 2) && field_has(o, m->name);  // AutoValue-style getter on a value we built
    if (!m->logged) { m->logged = 1; printf("JNI call %s.%s %s%s\n", m->cls->name, m->name, m->sig, a ? " (answered)" : getter ? " (field)" : ""); }
    if (a) return a(o);
    if (getter) return field_get(o, m->name);
    // shortcut: unknown String answers are "" (Java getters rarely hand back null); add a real answer when a value matters
    size_t n = strlen(m->sig);
    if (n >= 19 && !strcmp(m->sig + n - 19, ")Ljava/lang/String;")) { static void *empty; if (!empty) empty = new_string(""); return (uint64_t)empty; }
    return 0;
}
static float bits_f(uint64_t v) { float f; uint32_t b = (uint32_t)v; memcpy(&f, &b, 4); return f; }
static double bits_d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
// (SysV varargs: integer arguments land in the next registers then the stack, as for a plain call)
static SYSV uint64_t j_call(void *e, void *o, Mid *m, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t v[4] = { a, b, c, d }; return call(o, m, v);
}
typedef struct { uint32_t gp, fp; uint8_t *over, *save; } SysvVa;  // the guest's va_list (x86-64 SysV)
static SYSV uint64_t j_callV(void *e, void *o, Mid *m, SysvVa *va) {
    uint64_t v[4] = { 0 };
    if (!reads_args(o, m)) return call(o, m, 0);
    SysvVa c = *va;  // (integer arguments only: these methods take no floats)
    for (int i = 0; i < 4; i++) {
        if (c.gp < 48) { memcpy(&v[i], c.save + c.gp, 8); c.gp += 8; } else { memcpy(&v[i], c.over, 8); c.over += 8; }
    }
    return call(o, m, v);
}
static SYSV uint64_t j_callA(void *e, void *o, Mid *m, const uint64_t *args) {
    return call(o, m, reads_args(o, m) && args ? args : 0);
}
static SYSV float j_call_f(void *e, void *o, Mid *m) { return bits_f(call(o, m, 0)); }
static SYSV double j_call_d(void *e, void *o, Mid *m) { return bits_d(call(o, m, 0)); }
static SYSV uint64_t j_call_nv(void *e, void *o, void *c, Mid *m) { return call(o, m, 0); }
static SYSV float j_call_nv_f(void *e, void *o, void *c, Mid *m) { return bits_f(call(o, m, 0)); }
static SYSV double j_call_nv_d(void *e, void *o, void *c, Mid *m) { return bits_d(call(o, m, 0)); }
// NativeTextBoxInfo(FFFFFZIIIIIIZZZ): x, y, width, height, fontSize, multiline, xAlignment, yAlignment, textColor, ...
// Android draws a focused TextBox's text with its own text field (the engine hides it), so Skatestrap keeps the box and colour to draw it
static const char TBI[] = "com/roblox/engine/jni/model/NativeTextBoxInfo";
static int is_tbi(Cls *c, Mid *m) { return c && !strcmp(c->name, TBI) && !strncmp(m->sig, "(FFFFFZIII", 10); }
static Obj *tbi_new(Cls *c, const double *f, uint64_t color) {
    static const char *fn[] = { "x", "y", "width", "height", "fontSize" };
    Obj *o = new_obj(c);
    for (int k = 0; k < 5; k++) set_float(o, fn[k], (float)f[k]);
    field_set(o, "textColor", (uint32_t)color);
    return o;
}
int jni_textbox_info(void *o, float r[5], uint32_t *color) {
    static const char *fn[] = { "x", "y", "width", "height", "fontSize" };
    if (!is_obj(o) || ((Obj *)o)->kind != K_OBJ || strcmp(cname(o), TBI)) return 0;
    for (int k = 0; k < 5; k++) { uint32_t b = (uint32_t)field_get(o, fn[k]); memcpy(&r[k], &b, 4); }
    *color = (uint32_t)field_get(o, "textColor");
    return 1;
}
// (SysV varargs: floats arrive as doubles in xmm0-4; ints after the sixth register slot sit on the stack, so i3 = textColor)
static SYSV Obj *j_NewObject(void *e, Cls *c, Mid *m, uint64_t i0, uint64_t i1, uint64_t i2,
                             double f0, double f1, double f2, double f3, double f4, uint64_t i3) {
    log_id(m, "new");
    if (is_tbi(c, m)) { double f[5] = { f0, f1, f2, f3, f4 }; return tbi_new(c, f, i3); }
    return new_obj(c);
}
static SYSV Obj *j_NewObjectV(void *e, Cls *c, Mid *m, SysvVa *va) {
    log_id(m, "new");
    if (!is_tbi(c, m)) return new_obj(c);
    SysvVa a = *va; double f[5]; uint64_t i[4];
    for (int k = 0; k < 5; k++) if (a.fp < 176) { memcpy(&f[k], a.save + a.fp, 8); a.fp += 16; } else { memcpy(&f[k], a.over, 8); a.over += 8; }
    for (int k = 0; k < 4; k++) if (a.gp < 48) { memcpy(&i[k], a.save + a.gp, 8); a.gp += 8; } else { memcpy(&i[k], a.over, 8); a.over += 8; }
    return tbi_new(c, f, i[3]);
}
static SYSV Obj *j_NewObjectA(void *e, Cls *c, Mid *m, const uint64_t *args) {  // jvalue[]: a float sits in the low 4 bytes
    log_id(m, "new");
    if (!is_tbi(c, m) || !args) return new_obj(c);
    double f[5]; for (int k = 0; k < 5; k++) f[k] = bits_f(args[k]);
    return tbi_new(c, f, args[8]);
}
static SYSV Obj *j_AllocObject(void *e, Cls *c) { return new_obj(c); }
// a read of a field nobody set answers 0; log it once so the log shows what Roblox wants filled in
static int field_has(void *o, const char *name) {
    int set = 0;
    if (!is_obj(o)) return 0;
    AcquireSRWLockShared(&lock);
    for (Field *f = ((Obj *)o)->fields; f; f = f->next) if (!strcmp(f->name, name)) { set = 1; break; }
    ReleaseSRWLockShared(&lock);
    return set;
}
static uint64_t field_read(void *o, Mid *m) {
    int set = field_has(o, m->name);
    if (!set && !m->logged) {
        m->logged = 1;
        printf("JNI field unset %s.%s %s%s\n", is_obj(o) && ((Obj *)o)->cls ? ((Obj *)o)->cls->name : "?", m->name, m->sig, o ? "" : " (null object)");
    }
    return set ? field_get(o, m->name) : 0;
}
static SYSV uint64_t j_get_field(void *e, void *o, Mid *m) { return field_read(o, m); }
static SYSV float j_get_field_f(void *e, void *o, Mid *m) { return bits_f(field_read(o, m)); }
static SYSV double j_get_field_d(void *e, void *o, Mid *m) { return bits_d(field_read(o, m)); }
static SYSV void j_set_field(void *e, void *o, Mid *m, uint64_t v) { field_set(o, m->name, v); }
static SYSV void j_set_field_f(void *e, void *o, Mid *m, float v) { uint32_t b; memcpy(&b, &v, 4); field_set(o, m->name, b); }
static SYSV void j_set_field_d(void *e, void *o, Mid *m, double v) { uint64_t b; memcpy(&b, &v, 8); field_set(o, m->name, b); }

// strings (UTF-8 inside; UTF-16 views made on request)
static SYSV Str *j_NewStringUTF(void *e, const char *s) { return s ? new_string(s) : 0; }
static SYSV Str *j_NewString(void *e, const uint16_t *u, jint n) {
    int k = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)u, n, 0, 0, 0, 0);
    char *b = calloc(1, k + 1); WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)u, n, b, k, 0, 0);
    Str *s = new_string(b); free(b); return s;
}
static SYSV const char *j_GetStringUTFChars(void *e, Str *s, uint8_t *copy) { if (copy) *copy = 0; return s ? s->utf : 0; }
static SYSV jint j_GetStringUTFLength(void *e, Str *s) { return (jint)strlen(s->utf); }
static SYSV jint j_GetStringLength(void *e, Str *s) { return MultiByteToWideChar(CP_UTF8, 0, s->utf, -1, 0, 0) - 1; }
static SYSV uint16_t *j_GetStringChars(void *e, Str *s, uint8_t *copy) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s->utf, -1, 0, 0);
    uint16_t *w = malloc(n * 2); MultiByteToWideChar(CP_UTF8, 0, s->utf, -1, (wchar_t *)w, n);
    if (copy) *copy = 1;
    return w;
}
static SYSV void j_ReleaseStringChars(void *e, Str *s, uint16_t *w) { free(w); }
static SYSV void j_GetStringRegion(void *e, Str *s, jint at, jint n, uint16_t *out) {
    uint16_t *w = j_GetStringChars(e, s, 0); memcpy(out, w + at, n * 2); free(w);
}
static SYSV void j_GetStringUTFRegion(void *e, Str *s, jint at, jint n, char *out) {
    // shortcut: start/len counted in bytes, right for ASCII; convert through UTF-16 if non-ASCII text shows up
    memcpy(out, s->utf + at, n); out[n] = 0;
}

// arrays
static Arr *new_array(const char *cls, int elem, jint n) {
    Arr *a = calloc(1, sizeof *a);
    a->o = (Obj){ OBJ_MAGIC, K_ARRAY, class_of_name(cls) }; a->len = n; a->elem = elem;
    a->data = calloc(n ? n : 1, elem ? elem : 8);
    return a;
}
static SYSV jint j_GetArrayLength(void *e, Arr *a) { return a->len; }
static SYSV Arr *j_NewObjectArray(void *e, jint n, Cls *c, Obj *init) {
    Arr *a = new_array("[Ljava/lang/Object;", 0, n);
    for (jint i = 0; i < n; i++) ((Obj **)a->data)[i] = init;
    return a;
}
static SYSV Obj *j_GetObjectArrayElement(void *e, Arr *a, jint i) { return ((Obj **)a->data)[i]; }
static SYSV void j_SetObjectArrayElement(void *e, Arr *a, jint i, Obj *v) { ((Obj **)a->data)[i] = v; }
#define PRIM(T, sz, sig) \
    static SYSV Arr *j_New##T##Array(void *e, jint n) { return new_array("[" sig, sz, n); } \
    static SYSV void *j_Get##T##ArrayElements(void *e, Arr *a, uint8_t *copy) { if (copy) *copy = 0; return a->data; } \
    static SYSV void j_Release##T##ArrayElements(void *e, Arr *a, void *p, jint mode) {} \
    static SYSV void j_Get##T##ArrayRegion(void *e, Arr *a, jint at, jint n, void *out) { memcpy(out, (char *)a->data + at * sz, n * sz); } \
    static SYSV void j_Set##T##ArrayRegion(void *e, Arr *a, jint at, jint n, const void *in) { memcpy((char *)a->data + at * sz, in, n * sz); }
#define PRIMS(X) X(Boolean, 1, "Z") X(Byte, 1, "B") X(Char, 2, "C") X(Short, 2, "S") X(Int, 4, "I") X(Long, 8, "J") X(Float, 4, "F") X(Double, 8, "D")
PRIMS(PRIM)
static SYSV void *j_GetPrimitiveArrayCritical(void *e, Arr *a, uint8_t *copy) { if (copy) *copy = 0; return a->data; }
static SYSV void j_ReleasePrimitiveArrayCritical(void *e, Arr *a, void *p, jint mode) {}

static SYSV jint j_RegisterNatives(void *e, Cls *c, const Native *m, jint n) {
    AcquireSRWLockExclusive(&lock);
    Native *all = realloc(c->natives, (c->nnat + n) * sizeof *all);
    if (all) { memcpy(all + c->nnat, m, n * sizeof *m); c->natives = all; c->nnat += n; }
    ReleaseSRWLockExclusive(&lock);
    printf("JNI RegisterNatives %s: %d\n", c->name, n);
    return all ? 0 : -1;
}
static SYSV jint j_UnregisterNatives(void *e, Cls *c) { c->nnat = 0; return 0; }
static SYSV jint j_GetJavaVM(void *e, void ****out) { *out = &vm; return 0; }
static SYSV Buf *j_NewDirectByteBuffer(void *e, void *addr, jlong cap) {
    Buf *b = calloc(1, sizeof *b); b->o = (Obj){ OBJ_MAGIC, K_BUFFER, class_of_name("java/nio/DirectByteBuffer") };
    b->addr = addr; b->cap = cap; return b;
}
static SYSV void *j_GetDirectBufferAddress(void *e, Buf *b) { return is_obj(b) && b->o.kind == K_BUFFER ? b->addr : 0; }
static SYSV jlong j_GetDirectBufferCapacity(void *e, Buf *b) { return is_obj(b) && b->o.kind == K_BUFFER ? b->cap : -1; }

static SYSV jint v_DestroyJavaVM(void *v) { return 0; }
static SYSV jint v_GetEnv(void *v, void ****out, jint version) { *out = &env; return 0; }  // this thread's env
static SYSV jint v_Attach(void *v, void ****out, void *args) { *out = &env; return 0; }
static SYSV jint v_Detach(void *v) { return 0; }

void *jni_vm(void) {
    if (env_table[4]) return &vm;
    java_class = intern_class("java/lang/Class");
    java_class->o.cls = java_class;
#define FILL(n) env_table[idx_##n] = (void *)t_##n;
    JNI_ENV_SLOTS(FILL)
#define FILLV(n) vm_table[vidx_##n] = (void *)t_##n;
    JNI_VM_SLOTS(FILLV)
#define SET(n, f) env_table[idx_##n] = (void *)(f);
    SET(GetVersion, j_GetVersion) SET(FindClass, j_FindClass) SET(GetSuperclass, j_GetSuperclass)
    SET(IsAssignableFrom, j_IsAssignableFrom) SET(Throw, j_Throw) SET(ThrowNew, j_ThrowNew)
    SET(ExceptionOccurred, j_null) SET(ExceptionDescribe, j_nop) SET(ExceptionClear, j_nop) SET(ExceptionCheck, j_zero)
    SET(PushLocalFrame, j_zero) SET(PopLocalFrame, j_PopLocalFrame) SET(NewGlobalRef, j_same) SET(DeleteGlobalRef, j_nop)
    SET(DeleteLocalRef, j_nop) SET(IsSameObject, j_IsSameObject) SET(NewLocalRef, j_same) SET(EnsureLocalCapacity, j_zero)
    SET(NewWeakGlobalRef, j_same) SET(DeleteWeakGlobalRef, j_nop) SET(GetObjectRefType, j_GetObjectRefType)
    SET(AllocObject, j_AllocObject) SET(NewObject, j_NewObject) SET(NewObjectV, j_NewObjectV) SET(NewObjectA, j_NewObjectA)
    SET(GetObjectClass, j_GetObjectClass) SET(IsInstanceOf, j_IsInstanceOf)
    SET(GetMethodID, j_GetMethodID) SET(GetStaticMethodID, j_GetStaticMethodID)
    SET(GetFieldID, j_GetFieldID) SET(GetStaticFieldID, j_GetStaticFieldID)
#define CALLS(T) SET(Call##T##Method, j_call) SET(Call##T##MethodV, j_callV) SET(Call##T##MethodA, j_callA) \
    SET(CallStatic##T##Method, j_call) SET(CallStatic##T##MethodV, j_callV) SET(CallStatic##T##MethodA, j_callA) \
    SET(CallNonvirtual##T##Method, j_call_nv) SET(CallNonvirtual##T##MethodV, j_call_nv) SET(CallNonvirtual##T##MethodA, j_call_nv) \
    SET(Get##T##Field, j_get_field) SET(GetStatic##T##Field, j_get_field) SET(Set##T##Field, j_set_field) SET(SetStatic##T##Field, j_set_field)
#define CALLS_FP(T, s) SET(Call##T##Method, j_call_##s) SET(Call##T##MethodV, j_call_##s) SET(Call##T##MethodA, j_call_##s) \
    SET(CallStatic##T##Method, j_call_##s) SET(CallStatic##T##MethodV, j_call_##s) SET(CallStatic##T##MethodA, j_call_##s) \
    SET(CallNonvirtual##T##Method, j_call_nv_##s) SET(CallNonvirtual##T##MethodV, j_call_nv_##s) SET(CallNonvirtual##T##MethodA, j_call_nv_##s) \
    SET(Get##T##Field, j_get_field_##s) SET(GetStatic##T##Field, j_get_field_##s) SET(Set##T##Field, j_set_field_##s) SET(SetStatic##T##Field, j_set_field_##s)
    CALLS(Object) CALLS(Boolean) CALLS(Byte) CALLS(Char) CALLS(Short) CALLS(Int) CALLS(Long) CALLS_FP(Float, f) CALLS_FP(Double, d)
    SET(CallVoidMethod, j_call) SET(CallVoidMethodV, j_callV) SET(CallVoidMethodA, j_callA)
    SET(CallStaticVoidMethod, j_call) SET(CallStaticVoidMethodV, j_callV) SET(CallStaticVoidMethodA, j_callA)
    SET(CallNonvirtualVoidMethod, j_call_nv) SET(CallNonvirtualVoidMethodV, j_call_nv) SET(CallNonvirtualVoidMethodA, j_call_nv)
    SET(NewString, j_NewString) SET(GetStringLength, j_GetStringLength) SET(GetStringChars, j_GetStringChars)
    SET(ReleaseStringChars, j_ReleaseStringChars) SET(NewStringUTF, j_NewStringUTF) SET(GetStringUTFLength, j_GetStringUTFLength)
    SET(GetStringUTFChars, j_GetStringUTFChars) SET(ReleaseStringUTFChars, j_nop) SET(GetStringRegion, j_GetStringRegion)
    SET(GetStringUTFRegion, j_GetStringUTFRegion) SET(GetStringCritical, j_GetStringChars) SET(ReleaseStringCritical, j_ReleaseStringChars)
    SET(GetArrayLength, j_GetArrayLength) SET(NewObjectArray, j_NewObjectArray)
    SET(GetObjectArrayElement, j_GetObjectArrayElement) SET(SetObjectArrayElement, j_SetObjectArrayElement)
#define SETPRIM(T, sz, sig) SET(New##T##Array, j_New##T##Array) SET(Get##T##ArrayElements, j_Get##T##ArrayElements) \
    SET(Release##T##ArrayElements, j_Release##T##ArrayElements) SET(Get##T##ArrayRegion, j_Get##T##ArrayRegion) \
    SET(Set##T##ArrayRegion, j_Set##T##ArrayRegion)
    PRIMS(SETPRIM)
    SET(GetPrimitiveArrayCritical, j_GetPrimitiveArrayCritical) SET(ReleasePrimitiveArrayCritical, j_ReleasePrimitiveArrayCritical)
    SET(RegisterNatives, j_RegisterNatives) SET(UnregisterNatives, j_UnregisterNatives)
    SET(MonitorEnter, j_zero) SET(MonitorExit, j_zero) SET(GetJavaVM, j_GetJavaVM)
    SET(NewDirectByteBuffer, j_NewDirectByteBuffer) SET(GetDirectBufferAddress, j_GetDirectBufferAddress)
    SET(GetDirectBufferCapacity, j_GetDirectBufferCapacity)
    vm_table[vidx_DestroyJavaVM] = (void *)v_DestroyJavaVM; vm_table[vidx_GetEnv] = (void *)v_GetEnv;
    vm_table[vidx_AttachCurrentThread] = (void *)v_Attach; vm_table[vidx_AttachCurrentThreadAsDaemon] = (void *)v_Attach;
    vm_table[vidx_DetachCurrentThread] = (void *)v_Detach;
    return &vm;
}
void *jni_env(void) { jni_vm(); return &env; }

// --- helpers for startup.c (the "Java side" that drives the app's start-up natives) ----------------------
void *jni_class(const char *name) { return intern_class(name); }
void *jni_string(const char *utf) { return new_string(utf); }
const char *jni_utf(void *s) { return is_obj(s) && ((Obj *)s)->kind == K_STRING ? ((Str *)s)->utf : 0; }
void *jni_object(const char *cls) { return new_obj(intern_class(cls)); }
void *jni_array(int n) { return j_NewObjectArray(0, n, 0, 0); }
void jni_set(void *o, const char *field, uint64_t v) { field_set(o, field, v); }
void jni_set_str(void *o, const char *field, const char *s) { set_str(o, field, s); }
// shortcut: version of the APK in apk/ (scripts/apk_version.py); read it from the APK when users bring their own
const char *skatestrap_version(void) { const char *v = getenv("SKATESTRAP_ROBLOX_VERSION"); return v ? v : "2.746.1266"; }
// Selftest: FMOD's AudioDevice through all three call forms (varargs, va_list, jvalue[]): opens the real
// waveOut device, plays ~0.1 s of silence (more blocks than buffers, so write must wait), closes.
int shim_audio_selftest(void) {
    int bad = 0;
    Mid m = { intern_class("org/fmod/AudioDevice"), "init", "(IIII)Z", 0, 0, 1, 0 };
    if (j_call(0, 0, &m, 2, 48000, 512, 3) != 1) { printf("audio selftest: init (no output device?)\n"); return 1; }
    Arr *buf = new_array("[B", 1, 2048);
    Mid w = m; w.name = "write"; w.sig = "([BI)V";
    uint64_t save[6] = { 0, 0, 0, (uint64_t)buf, 2048, 0 }, over[2] = { 0, 0 };
    uint64_t args[2] = { (uint64_t)buf, 2048 };
    DWORD t = GetTickCount();
    for (int i = 0; i < 9; i++) {
        SysvVa va = { 24, 48, (uint8_t *)over, (uint8_t *)save };  // two ints already taken from registers
        if (i % 3 == 0) j_call(0, 0, &w, (uint64_t)buf, 2048, 0, 0);
        else if (i % 3 == 1) j_callV(0, 0, &w, &va);
        else j_callA(0, 0, &w, args);
        if (ad.next != (i + 1) % ad.n) { bad++; printf("audio selftest: write %d not queued\n", i); }
    }
    if (GetTickCount() - t < 50) { bad++; printf("audio selftest: write did not wait for playback\n"); }
    Mid c = m; c.name = "close"; c.sig = "()V";
    j_callA(0, 0, &c, args);
    if (ad.h) { bad++; printf("audio selftest: close\n"); }
    if (answer_for("checkInit") != a_true) { bad++; printf("audio selftest: checkInit\n"); }
    return bad;
}
