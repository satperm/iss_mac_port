/* Gravity Rush 2: libSceJson2 (sce::Json value/serializer API).
 *
 * What the game uses it for (eboot 1.00, Ghidra addresses = vaddr + 0x1000000):
 *   - Telemetry ("GameAnalytics" thread FUN_0204f280, POSTs to /games/<id>/telemetry):
 *     events BootStart, UserProfile, MissionStart/MissionEnd (FUN_01546740), each a
 *     sce::Json::Object filled with episode_id, talisman, power_up, player_pos_*, ...
 *     Mission events are only built when sceNpGetState() reports signed-in.
 *   - UGC photo report (FUN_02050d10/FUN_02051c60): reporter/item objects serialized
 *     and sent over HTTP.
 * The game never parses JSON (no Parser/deserialize imports) and never reads values
 * back: it only builds Values/Objects/Arrays, serializes them into a String and reads
 * String::c_str() / Array::size(). Initializer::initialize is called once at boot
 * (FUN_01445bf0) with a game-derived MemAllocator; failure would only disable the
 * telemetry thread.
 *
 * Object sizes, from the stack slots the game reserves at every call site (e.g.
 * FUN_02050d10, FUN_01545020, FUN_01546740):
 *   Value 0x20 bytes, String 8, Object 8, Array 8.
 *   Initializer: heap block of 1 byte; MemAllocator (game subclass): 8 bytes (vtable only).
 * Fields are never touched inline by the game, so the internal layout is ours:
 *   String/Object/Array hold one host pointer (NULL = empty);
 *   Value = {u32 type; u32 pad; u64 payload; u64 reserved[2]}.
 * Object/Array elements are individually allocated nodes, so a Value& returned by
 * Object::operator[] stays valid while other keys are added (the game assigns through it
 * right after constructing the right-hand Value). Copies are deep.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONCE(what) do { static int seen_; \
    if (!__atomic_exchange_n(&seen_,1,__ATOMIC_RELAXED)) printf("Runtime: sce::Json %s\n",what); } while (0)

enum { JT_NULL, JT_BOOL, JT_INT, JT_UINT, JT_REAL, JT_STRING, JT_ARRAY, JT_OBJECT };

struct JNode;
typedef struct { struct JNode *head, *tail; size_t count; } JList; /* Array and Object bodies */
typedef struct {
    uint32_t type, pad;
    union { uint64_t b; int64_t i; uint64_t u; double r; char *s; JList *list; } v;
    uint64_t reserved[2];
} JValue;
typedef struct JNode { struct JNode *next; char *key; JValue value; } JNode;
typedef struct { char *text; } JString;   /* sce::Json::String */
typedef struct { JList *list; } JObject;  /* sce::Json::Object */
typedef struct { JList *list; } JArray;   /* sce::Json::Array */
_Static_assert(sizeof(JValue)==0x20,"sce::Json::Value is 0x20 bytes");
_Static_assert(sizeof(JString)==8 && sizeof(JObject)==8 && sizeof(JArray)==8,"8-byte handles");

static void *xcalloc(size_t n, size_t size) {
    void *p=calloc(n?n:1,size?size:1);
    if (!p) { fprintf(stderr,"Runtime: sce::Json out of memory\n"); abort(); }
    return p;
}
static char *xstrdup(const char *s) {
    size_t n=strlen(s?s:"");
    char *p=xcalloc(n+1,1);
    memcpy(p,s?s:"",n);
    return p;
}

static void value_free(JValue *v);
static void value_copy(JValue *dst, const JValue *src);

static void list_free(JList *l) {
    if (!l) return;
    for (JNode *n=l->head,*next;n;n=next) { next=n->next; free(n->key); value_free(&n->value); free(n); }
    free(l);
}
static JNode *list_append(JList *l, const char *key) {
    JNode *n=xcalloc(1,sizeof *n);
    n->key=key?xstrdup(key):NULL;
    n->value.type=JT_NULL;
    if (l->tail) l->tail->next=n; else l->head=n;
    l->tail=n; l->count++;
    return n;
}
static JList *list_copy(const JList *src) {
    if (!src) return NULL;
    JList *l=xcalloc(1,sizeof *l);
    for (const JNode *n=src->head;n;n=n->next) value_copy(&list_append(l,n->key)->value,&n->value);
    return l;
}
static void value_free(JValue *v) {
    if (v->type==JT_STRING) free(v->v.s);
    else if (v->type==JT_ARRAY || v->type==JT_OBJECT) list_free(v->v.list);
    memset(v,0,sizeof *v);
}
static void value_copy(JValue *dst, const JValue *src) {
    memset(dst,0,sizeof *dst);
    dst->type=src->type;
    if (src->type==JT_STRING) dst->v.s=xstrdup(src->v.s);
    else if (src->type==JT_ARRAY || src->type==JT_OBJECT) dst->v.list=list_copy(src->v.list);
    else dst->v=src->v;
}
/* Assignment that tolerates src living inside dst (copy first, then release). */
static void value_assign(JValue *dst, const JValue *src) {
    if (dst==src) return;
    JValue tmp;
    value_copy(&tmp,src);
    value_free(dst);
    *dst=tmp;
}

/* ---- Initializer / MemAllocator ---- */
static ABI int32_t init_ctor(void *self) { ONCE("Initializer::Initializer"); (void)self; return 0; }
static ABI int32_t init_dtor(void *self) { (void)self; return 0; }
static ABI int32_t init_initialize(void *self, const void *param) {
    (void)self; (void)param; ONCE("Initializer::initialize");
    return 0; /* storage comes from the host heap; the game's allocator is not needed */
}
static ABI int32_t init_terminate(void *self) { (void)self; ONCE("Initializer::terminate"); return 0; }

static ABI void *allocator_base_slot(void *self) {
    (void)self; ONCE("MemAllocator base virtual called");
    return NULL;
}
/* Base vtable written by MemAllocator::MemAllocator(); the game overwrites it with its own
 * subclass vtable right after (FUN_01445bf0), so these slots are only a safety net. */
static void *const allocator_vtable[8]={
    (void *)allocator_base_slot,(void *)allocator_base_slot,(void *)allocator_base_slot,(void *)allocator_base_slot,
    (void *)allocator_base_slot,(void *)allocator_base_slot,(void *)allocator_base_slot,(void *)allocator_base_slot,
};
static ABI void allocator_ctor(void **self) { ONCE("MemAllocator::MemAllocator"); *self=(void *)allocator_vtable; }
static ABI void allocator_dtor(void **self) { (void)self; }
static ABI void allocator_notify_error(void *self, int32_t error, uint64_t size, void *user) {
    (void)self; (void)user;
    printf("Runtime: sce::Json MemAllocator::notifyError(%#x, %llu)\n",(unsigned)error,(unsigned long long)size);
}

/* ---- String ---- */
static const char empty_text[1];
static ABI void string_ctor(JString *self) { self->text=NULL; }
static ABI void string_ctor_cstr(JString *self, const char *s) { self->text=(s && *s)?xstrdup(s):NULL; }
static ABI void string_dtor(JString *self) { free(self->text); self->text=NULL; }
static ABI const char *string_c_str(const JString *self) { return self->text?self->text:empty_text; }

/* ---- Value ---- */
static ABI void value_ctor(JValue *self) { ONCE("Value (first use)"); memset(self,0,sizeof *self); }
static ABI void value_ctor_int(JValue *self, int64_t i) { memset(self,0,sizeof *self); self->type=JT_INT; self->v.i=i; }
static ABI void value_ctor_uint(JValue *self, uint64_t u) { memset(self,0,sizeof *self); self->type=JT_UINT; self->v.u=u; }
static ABI void value_ctor_real(JValue *self, double r) { memset(self,0,sizeof *self); self->type=JT_REAL; self->v.r=r; }
static ABI void value_ctor_string(JValue *self, const JString *s) {
    memset(self,0,sizeof *self); self->type=JT_STRING; self->v.s=xstrdup(string_c_str(s));
}
static ABI void value_ctor_array(JValue *self, const JArray *a) {
    memset(self,0,sizeof *self); self->type=JT_ARRAY; self->v.list=list_copy(a->list);
}
static ABI void value_ctor_object(JValue *self, const JObject *o) {
    memset(self,0,sizeof *self); self->type=JT_OBJECT; self->v.list=list_copy(o->list);
}
static ABI void value_dtor(JValue *self) { value_free(self); }
static ABI int32_t value_clear(JValue *self) { value_free(self); return 0; }
static ABI int32_t value_set(JValue *self, const JValue *other) { value_assign(self,other); return 0; }
static ABI JValue *value_operator_assign(JValue *self, const JValue *other) { value_assign(self,other); return self; }

/* ---- Array ---- */
static ABI void array_ctor(JArray *self) { self->list=NULL; }
static ABI void array_dtor(JArray *self) { list_free(self->list); self->list=NULL; }
static ABI void array_push_back(JArray *self, const JValue *value) {
    if (!self->list) self->list=xcalloc(1,sizeof *self->list);
    JValue tmp;
    value_copy(&tmp,value); /* value may live inside this array */
    list_append(self->list,NULL)->value=tmp;
}
static ABI uint64_t array_size(const JArray *self) { return self->list?self->list->count:0; }

/* ---- Object ---- */
static ABI void object_ctor(JObject *self) { self->list=NULL; }
static ABI void object_ctor_copy(JObject *self, const JObject *other) { self->list=list_copy(other->list); }
static ABI void object_dtor(JObject *self) { list_free(self->list); self->list=NULL; }
static ABI JObject *object_operator_assign(JObject *self, const JObject *other) {
    if (self!=other) {
        JList *copy=list_copy(other->list);
        list_free(self->list);
        self->list=copy;
    }
    return self;
}
static ABI JValue *object_operator_index(JObject *self, const JString *key) {
    const char *k=string_c_str(key);
    if (!self->list) self->list=xcalloc(1,sizeof *self->list);
    for (JNode *n=self->list->head;n;n=n->next) if (!strcmp(n->key,k)) return &n->value;
    return &list_append(self->list,k)->value;
}

/* ---- serialize ---- */
typedef struct { char *data; size_t len, cap; } Buffer;
static void put(Buffer *b, const char *s, size_t n) {
    if (b->len+n+1>b->cap) {
        size_t cap=b->cap?b->cap:256;
        while (b->len+n+1>cap) cap*=2;
        char *p=realloc(b->data,cap);
        if (!p) { fprintf(stderr,"Runtime: sce::Json out of memory\n"); abort(); }
        b->data=p; b->cap=cap;
    }
    memcpy(b->data+b->len,s,n);
    b->len+=n;
    b->data[b->len]=0;
}
static void puts_(Buffer *b, const char *s) { put(b,s,strlen(s)); }
static void put_quoted(Buffer *b, const char *s) {
    put(b,"\"",1);
    for (const unsigned char *p=(const unsigned char *)s;*p;++p) {
        char esc[8];
        switch (*p) {
        case '"': puts_(b,"\\\""); break;
        case '\\': puts_(b,"\\\\"); break;
        case '\b': puts_(b,"\\b"); break;
        case '\f': puts_(b,"\\f"); break;
        case '\n': puts_(b,"\\n"); break;
        case '\r': puts_(b,"\\r"); break;
        case '\t': puts_(b,"\\t"); break;
        default:
            if (*p<0x20) { snprintf(esc,sizeof esc,"\\u%04x",*p); puts_(b,esc); }
            else put(b,(const char *)p,1); /* UTF-8 passes through */
        }
    }
    put(b,"\"",1);
}
static void put_value(Buffer *b, const JValue *v, int depth) {
    char num[40];
    if (depth>256) { puts_(b,"null"); return; }
    switch (v->type) {
    case JT_BOOL: puts_(b,v->v.b?"true":"false"); break;
    case JT_INT: snprintf(num,sizeof num,"%lld",(long long)v->v.i); puts_(b,num); break;
    case JT_UINT: snprintf(num,sizeof num,"%llu",(unsigned long long)v->v.u); puts_(b,num); break;
    case JT_REAL:
        if (!isfinite(v->v.r)) { puts_(b,"null"); break; }
        for (int precision=1;precision<=17;++precision) { /* shortest text that round-trips */
            snprintf(num,sizeof num,"%.*g",precision,v->v.r);
            if (strtod(num,NULL)==v->v.r) break;
        }
        puts_(b,num);
        break;
    case JT_STRING: put_quoted(b,v->v.s?v->v.s:""); break;
    case JT_ARRAY: case JT_OBJECT: {
        int object=v->type==JT_OBJECT;
        put(b,object?"{":"[",1);
        int first=1;
        for (const JNode *n=v->v.list?v->v.list->head:NULL;n;n=n->next) {
            if (!first) put(b,",",1);
            first=0;
            if (object) { put_quoted(b,n->key); put(b,":",1); }
            put_value(b,&n->value,depth+1);
        }
        put(b,object?"}":"]",1);
        break;
    }
    default: puts_(b,"null");
    }
}
static ABI int32_t value_serialize(const JValue *self, JString *out) {
    ONCE("Value::serialize");
    Buffer b={0};
    put_value(&b,self,0);
    free(out->text);
    out->text=b.data;
    if (getenv("TLG_JSON_LOG")) printf("Runtime: sce::Json serialize %s\n",b.data);
    return 0;
}

static const RuntimeExport exports[]={
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",init_initialize},
    {"_ZN3sce4Json11Initializer9terminateEv",init_terminate},
    {"_ZN3sce4Json11InitializerC1Ev",init_ctor},
    {"_ZN3sce4Json11InitializerD1Ev",init_dtor},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv",allocator_notify_error},
    {"_ZN3sce4Json12MemAllocatorC2Ev",allocator_ctor},
    {"_ZN3sce4Json12MemAllocatorD2Ev",allocator_dtor},
    {"_ZN3sce4Json5Array9push_backERKNS0_5ValueE",array_push_back},
    {"_ZN3sce4Json5ArrayC1Ev",array_ctor},
    {"_ZN3sce4Json5ArrayD1Ev",array_dtor},
    {"_ZN3sce4Json5Value3setERKS1_",value_set},
    {"_ZN3sce4Json5Value5clearEv",value_clear},
    {"_ZN3sce4Json5Value9serializeERNS0_6StringE",value_serialize},
    {"_ZN3sce4Json5ValueaSERKS1_",value_operator_assign},
    {"_ZN3sce4Json5ValueC1Ed",value_ctor_real},
    {"_ZN3sce4Json5ValueC1El",value_ctor_int},
    {"_ZN3sce4Json5ValueC1Em",value_ctor_uint},
    {"_ZN3sce4Json5ValueC1ERKNS0_5ArrayE",value_ctor_array},
    {"_ZN3sce4Json5ValueC1ERKNS0_6ObjectE",value_ctor_object},
    {"_ZN3sce4Json5ValueC1ERKNS0_6StringE",value_ctor_string},
    {"_ZN3sce4Json5ValueC1Ev",value_ctor},
    {"_ZN3sce4Json5ValueD1Ev",value_dtor},
    {"_ZN3sce4Json6ObjectaSERKS1_",object_operator_assign},
    {"_ZN3sce4Json6ObjectC1ERKS1_",object_ctor_copy},
    {"_ZN3sce4Json6ObjectC1Ev",object_ctor},
    {"_ZN3sce4Json6ObjectD1Ev",object_dtor},
    {"_ZN3sce4Json6ObjectixERKNS0_6StringE",object_operator_index},
    {"_ZN3sce4Json6StringC1EPKc",string_ctor_cstr},
    {"_ZN3sce4Json6StringC1Ev",string_ctor},
    {"_ZN3sce4Json6StringD1Ev",string_dtor},
    {"_ZNK3sce4Json5Array4sizeEv",array_size},
    {"_ZNK3sce4Json6String5c_strEv",string_c_str},
};
uintptr_t runtime_gr2_json_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
