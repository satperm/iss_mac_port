/* Gravity Rush 2: libSceAudio3d (object-based 3D audio port), mixed down to stereo.
 *
 * How the game uses it (eboot 1.00, Ghidra addresses = vaddr + 0x1000000). The sndx sound engine
 * is initialized with "loadAudio3d=1 ..." (string 0x24bc430), and its NGS2 engine init
 * (FUN_0125af..., sndx_engine_ngs2_orbis.c) aborts the whole sound system when Audio3d fails, so
 * neither the AudioOut main port nor anything else is opened without a working Audio3d:
 *   - sceAudio3dInitialize(0); sceAudio3dGetDefaultOpenParameters(p) into a 0x20-byte buffer the
 *     game zeroed, then overwritten: szSizeThis=0x20, uiGranularity=numGrains (256), eRate=0
 *     (48 kHz), uiMaxObjects=512, uiQueueDepth=3, eBufferMode=2 (ADVANCE_AND_PUSH).
 *   - sceAudio3dPortOpen(0xff, p, &port); sceAudio3dPortSetAttribute(port, 0x10001
 *     LATE_REVERB_LEVEL, &0.0f, 4). No beds: sceAudio3dBedWrite is not imported.
 *   - Every voice flagged 3D reserves an object (FUN_0125cb90, sceAudio3dObjectReserve) and frees it
 *     with sceAudio3dObjectUnreserve. An NGS2 voice callback (FUN_0125f7c0) sets, per render, 5
 *     attributes in this order: POSITION (3 floats), GAIN (float), SPREAD (float, radians, 2*pi
 *     times a 0..1 width), PRIORITY (0), PCM {FLOAT, buffer, numSamples} (mono).
 *   - The sndx out thread (FUN_012aba20) renders NGS2 (FUN_01263d10), outputs the 8ch bed mix with
 *     sceAudioOutOutputs on its MAIN port (which paces it), then
 *     sceAudio3dPortGetQueueLevel(port, NULL, &available) and, if available > 1,
 *     sceAudio3dPortAdvance(port) + sets the "sndx_audio3d_notify" event flag. A dedicated thread
 *     "sndx_audio3d_out_thread" (FUN_0125c6d0) waits on the flag and calls
 *     sceAudio3dPortPush(port, SCE_AUDIO3D_BLOCKING_SYNC).
 *
 * Implementation: objects keep their last position/gain/spread; a PCM attribute stores one granule
 * of samples for the object. PortAdvance mixes every object with PCM into a stereo float granule
 * (equal-power pan from the position's x/distance, narrowed toward center by spread, times gain)
 * and queues it; PortPush hands queued granules to a host AudioOut port of type AUDIO3D (126) in
 * runtime_audio.c, which shares the SDL device with the guest's ports and returns once per
 * granule period on a steady clock (blocking mode). Positions are listener-relative with +x to
 * the right, which holds for both the left- and right-handed conventions.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ONCE(what) do { static int seen_; \
    if (!__atomic_exchange_n(&seen_,1,__ATOMIC_RELAXED)) printf("Runtime: Audio3d %s\n",what); } while (0)

#define A3D_ERR_UNKNOWN ((int32_t)0x80EA0001)
#define A3D_ERR_INVALID_PORT ((int32_t)0x80EA0002)
#define A3D_ERR_INVALID_OBJECT ((int32_t)0x80EA0003)
#define A3D_ERR_INVALID_PARAMETER ((int32_t)0x80EA0004)
#define A3D_ERR_OUT_OF_MEMORY ((int32_t)0x80EA0005)
#define A3D_ERR_OUT_OF_RESOURCES ((int32_t)0x80EA0006)
#define A3D_ERR_NOT_READY ((int32_t)0x80EA0007)

enum { ATTR_PCM=1, ATTR_PRIORITY=2, ATTR_POSITION=3, ATTR_SPREAD=4, ATTR_GAIN=5, ATTR_PASSTHROUGH=6,
       ATTR_RESET_STATE=7, ATTR_APPLICATION_SPECIFIC=8, ATTR_AMBISONICS=9, ATTR_RESTRICTED=10,
       ATTR_OUTPUT_ROUTE=11, PORT_ATTR_LATE_REVERB_LEVEL=0x10001 };
enum { FORMAT_S16=0, FORMAT_FLOAT=1 };
enum { BLOCKING_ASYNC=0, BLOCKING_SYNC=1 };

#define MAX_PORTS 4
#define MAX_GRANULARITY 2048
#define MAX_QUEUE 64
#define AUDIOOUT_TYPE_AUDIO3D 126
#define AUDIOOUT_FLOAT_STEREO 4

/* Guest layouts (PS4 SDK audio3d.h). */
typedef struct {
    uint64_t size_this;
    uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad;
    uint32_t num_beds, pad2;       /* only when size_this >= 0x28 (newer SDKs) */
} OpenParameters;
_Static_assert(sizeof(OpenParameters)==0x28,"SceAudio3dOpenParameters with uiNumBeds");
typedef struct { uint32_t id, pad; const void *value; uint64_t size; } Attribute;
_Static_assert(sizeof(Attribute)==0x18,"SceAudio3dAttribute");
typedef struct { uint32_t format, pad; const void *buffer; uint32_t samples, pad2; } Pcm;
_Static_assert(sizeof(Pcm)==0x18,"SceAudio3dPcm");

typedef struct {
    int used, has_pcm, passthrough;
    float position[3], gain, spread;
    float *pcm;                    /* one granule, mono */
} Object;
typedef struct {
    int used;
    uint32_t granularity, max_objects, queue_depth, buffer_mode;
    float late_reverb;
    Object *objects;
    float *queue;                  /* queue_depth stereo granules */
    uint32_t head, count;          /* ring of queued granules */
    int32_t out;                   /* host AudioOut handle, <0 = none */
    pthread_mutex_t lock;          /* port state; never held while output blocks */
    pthread_mutex_t push_lock;     /* serializes pushes so granules leave in order */
} Port;

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[MAX_PORTS];
static int initialized;
static uint64_t stat_advanced, stat_pushed, stat_dropped, stat_pcm_sets, stat_reserved, stat_peak_objects,
                stat_nonsilent, stat_samples;
static float stat_peak;
static uint64_t stat_last_ns;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }

static Port *port_of(uint32_t id) {
    if (id>=MAX_PORTS || !ports[id].used) return NULL;
    return &ports[id];
}
static Object *object_of(Port *p, uint32_t id) {
    if (id>=p->max_objects || !p->objects[id].used) return NULL;
    return &p->objects[id];
}
static void object_reset(Object *o) {
    o->has_pcm=0; o->passthrough=0;
    o->position[0]=o->position[1]=o->position[2]=0.0f;
    o->gain=1.0f; o->spread=0.0f;
}

static ABI int32_t a3d_initialize(int64_t reserved) {
    ONCE("sceAudio3dInitialize");
    if (reserved) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&table_lock);
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    return 0;
}
static ABI int32_t a3d_get_default_open_parameters(OpenParameters *params) {
    ONCE("sceAudio3dGetDefaultOpenParameters");
    if (!params) return A3D_ERR_INVALID_PARAMETER;
    /* The game hands a 0x20-byte stack buffer: never touch uiNumBeds. */
    OpenParameters d={0x20,256,0,512,2,2,0,0,0};
    memcpy(params,&d,0x20);
    return 0;
}
static ABI int32_t a3d_port_open(int32_t user, const OpenParameters *params, uint32_t *port_id) {
    ONCE("sceAudio3dPortOpen");
    (void)user;
    if (!initialized) return A3D_ERR_NOT_READY;
    if (!params || !port_id || params->size_this<0x20) return A3D_ERR_INVALID_PARAMETER;
    if (params->rate!=0 || params->granularity<64 || params->granularity>MAX_GRANULARITY || (params->granularity & 63))
        return A3D_ERR_INVALID_PARAMETER;
    uint32_t depth=params->queue_depth ? params->queue_depth : 1, objects=params->max_objects;
    if (depth>MAX_QUEUE || objects>4096) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=0;i<MAX_PORTS;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return A3D_ERR_OUT_OF_RESOURCES; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    p->granularity=params->granularity; p->max_objects=objects; p->queue_depth=depth;
    p->buffer_mode=params->buffer_mode;
    p->objects=calloc(objects ? objects : 1,sizeof(Object));
    p->queue=calloc((size_t)depth*params->granularity*2,sizeof(float));
    if (!p->objects || !p->queue) {
        free(p->objects); free(p->queue); pthread_mutex_unlock(&table_lock); return A3D_ERR_OUT_OF_MEMORY;
    }
    pthread_mutex_init(&p->lock,NULL);
    pthread_mutex_init(&p->push_lock,NULL);
    p->out=runtime_audio_host_open(AUDIOOUT_TYPE_AUDIO3D,params->granularity,AUDIOOUT_FLOAT_STEREO);
    if (p->out<0) fprintf(stderr,"Runtime: Audio3d port %d has no output (AudioOut error 0x%08x)\n",id,(uint32_t)p->out);
    p->used=1;
    pthread_mutex_unlock(&table_lock);
    *port_id=(uint32_t)id;
    printf("Runtime: Audio3d port %d opened (granularity %u, %u objects, queue depth %u, buffer mode %u, params 0x%llx bytes)\n",
           id,p->granularity,objects,depth,p->buffer_mode,(unsigned long long)params->size_this);
    return 0;
}
static ABI int32_t a3d_port_close(uint32_t port_id) {
    ONCE("sceAudio3dPortClose");
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(port_id);
    if (!p) { pthread_mutex_unlock(&table_lock); return A3D_ERR_INVALID_PORT; }
    pthread_mutex_lock(&p->push_lock);
    pthread_mutex_lock(&p->lock);
    p->used=0;
    if (p->out>=0) runtime_audio_host_close(p->out);
    for (uint32_t i=0;i<p->max_objects;++i) free(p->objects[i].pcm);
    free(p->objects); free(p->queue);
    p->objects=NULL; p->queue=NULL;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&p->push_lock);
    pthread_mutex_destroy(&p->lock);
    pthread_mutex_destroy(&p->push_lock);
    pthread_mutex_unlock(&table_lock);
    return 0;
}
static ABI int32_t a3d_port_set_attribute(uint32_t port_id, uint32_t attribute, const void *value, uint64_t size) {
    ONCE("sceAudio3dPortSetAttribute");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (!value && size) return A3D_ERR_INVALID_PARAMETER;
    if (attribute==PORT_ATTR_LATE_REVERB_LEVEL && size>=4) { memcpy(&p->late_reverb,value,4); return 0; }
    static int logged; /* any other port attribute: accepted, no effect on a stereo downmix */
    if (!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
        printf("Runtime: Audio3d port attribute 0x%x (%llu bytes) ignored\n",attribute,(unsigned long long)size);
    return 0;
}
static ABI int32_t a3d_object_reserve(uint32_t port_id, uint32_t *object_id) {
    ONCE("sceAudio3dObjectReserve");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (!object_id) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock);
    uint32_t id=p->max_objects, in_use=0;
    for (uint32_t i=0;i<p->max_objects;++i) {
        if (p->objects[i].used) ++in_use;
        else if (id==p->max_objects) id=i;
    }
    if (id==p->max_objects) { pthread_mutex_unlock(&p->lock); return A3D_ERR_OUT_OF_RESOURCES; }
    Object *o=&p->objects[id];
    if (!o->pcm) o->pcm=calloc(p->granularity,sizeof(float));
    if (!o->pcm) { pthread_mutex_unlock(&p->lock); return A3D_ERR_OUT_OF_MEMORY; }
    o->used=1; object_reset(o);
    if (in_use+1>stat_peak_objects) stat_peak_objects=in_use+1;
    ++stat_reserved;
    pthread_mutex_unlock(&p->lock);
    *object_id=id;
    return 0;
}
static ABI int32_t a3d_object_unreserve(uint32_t port_id, uint32_t object_id) {
    ONCE("sceAudio3dObjectUnreserve");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    pthread_mutex_lock(&p->lock);
    Object *o=object_of(p,object_id);
    if (o) { o->used=0; o->has_pcm=0; }
    pthread_mutex_unlock(&p->lock);
    return o ? 0 : A3D_ERR_INVALID_OBJECT;
}
static int read_float(const Attribute *a, float *out) {
    if (!a->value || a->size<4) return 0;
    memcpy(out,a->value,4);
    return isfinite(*out);
}
static ABI int32_t a3d_object_set_attributes(uint32_t port_id, uint32_t object_id, uint64_t count, const Attribute *attributes) {
    ONCE("sceAudio3dObjectSetAttributes");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (count && !attributes) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock);
    Object *o=object_of(p,object_id);
    if (!o) { pthread_mutex_unlock(&p->lock); return A3D_ERR_INVALID_OBJECT; }
    int32_t result=0;
    for (uint64_t i=0;i<count;++i) {
        const Attribute *a=&attributes[i];
        float f;
        switch (a->id) {
        case ATTR_PCM: {
            if (!a->value || a->size<sizeof(Pcm)) { result=A3D_ERR_INVALID_PARAMETER; break; }
            Pcm pcm; memcpy(&pcm,a->value,sizeof(pcm));
            if (!pcm.buffer || (pcm.format!=FORMAT_FLOAT && pcm.format!=FORMAT_S16)) { result=A3D_ERR_INVALID_PARAMETER; break; }
            static int logged;
            if (!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
                printf("Runtime: Audio3d first object PCM: %s, %u samples (granularity %u)\n",
                       pcm.format==FORMAT_FLOAT ? "float" : "s16",pcm.samples,p->granularity);
            uint32_t n=pcm.samples<p->granularity ? pcm.samples : p->granularity;
            if (pcm.format==FORMAT_FLOAT) memcpy(o->pcm,pcm.buffer,(size_t)n*4);
            else for (uint32_t s=0;s<n;++s) o->pcm[s]=(float)((const int16_t *)pcm.buffer)[s]/32768.0f;
            if (n<p->granularity) memset(o->pcm+n,0,(size_t)(p->granularity-n)*4);
            o->has_pcm=1;
            ++stat_pcm_sets;
            break;
        }
        case ATTR_POSITION:
            if (a->value && a->size>=12) {
                float v[3]; memcpy(v,a->value,12);
                if (isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2])) memcpy(o->position,v,12);
            }
            break;
        case ATTR_GAIN: if (read_float(a,&f)) o->gain=f; break;
        case ATTR_SPREAD: if (read_float(a,&f)) o->spread=f; break;
        case ATTR_PASSTHROUGH: if (a->value && a->size>=4) { uint32_t v; memcpy(&v,a->value,4); o->passthrough=v!=0; } break;
        case ATTR_RESET_STATE: object_reset(o); break;
        case ATTR_PRIORITY: case ATTR_APPLICATION_SPECIFIC: case ATTR_RESTRICTED: case ATTR_OUTPUT_ROUTE: break;
        default: {
            static int logged;
            if (!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
                printf("Runtime: Audio3d object attribute 0x%x (%llu bytes) ignored\n",a->id,(unsigned long long)a->size);
            break;
        }
        }
    }
    pthread_mutex_unlock(&p->lock);
    return result;
}
/* Mixes the current granule of every object into `out` (stereo, interleaved) and consumes it. */
static void mix_objects(Port *p, float *out) {
    uint32_t g=p->granularity;
    memset(out,0,(size_t)g*2*sizeof(float));
    for (uint32_t i=0;i<p->max_objects;++i) {
        Object *o=&p->objects[i];
        if (!o->used || !o->has_pcm) continue;
        o->has_pcm=0;
        float x=o->position[0], y=o->position[1], z=o->position[2];
        float distance=sqrtf(x*x+y*y+z*z), pan=0.0f;
        if (!o->passthrough && distance>1e-4f) {
            pan=x/distance;
            float width=o->spread/(2.0f*(float)M_PI);
            width=width<0.0f ? 0.0f : width>1.0f ? 1.0f : width;
            pan*=1.0f-width;
        }
        float angle=(pan+1.0f)*(float)M_PI*0.25f, gl=cosf(angle)*o->gain, gr=sinf(angle)*o->gain;
        for (uint32_t s=0;s<g;++s) { out[2*s]+=o->pcm[s]*gl; out[2*s+1]+=o->pcm[s]*gr; }
    }
    for (uint32_t s=0;s<2*g;++s) {
        float v=out[s];
        out[s]=v<-1.0f ? -1.0f : v>1.0f ? 1.0f : v;
        float m=fabsf(v);
        if (m>1e-4f) ++stat_nonsilent;
        if (m>stat_peak) stat_peak=m;
    }
    stat_samples+=2u*g;
}
static ABI int32_t a3d_port_advance(uint32_t port_id) {
    ONCE("sceAudio3dPortAdvance");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    pthread_mutex_lock(&p->lock);
    int32_t result=0;
    if (p->count>=p->queue_depth) { ++stat_dropped; result=A3D_ERR_NOT_READY; }
    else {
        uint32_t slot=(p->head+p->count)%p->queue_depth;
        mix_objects(p,p->queue+(size_t)slot*p->granularity*2);
        ++p->count; ++stat_advanced;
    }
    uint64_t now=now_ns();
    if (!stat_last_ns) stat_last_ns=now;
    else if (now-stat_last_ns>=10000000000u) {
        stat_last_ns=now;
        runtime_gr2_audio3d_report();
    }
    pthread_mutex_unlock(&p->lock);
    return result;
}
static ABI int32_t a3d_port_push(uint32_t port_id, uint32_t blocking) {
    ONCE("sceAudio3dPortPush");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (blocking>BLOCKING_SYNC) return A3D_ERR_INVALID_PARAMETER;
    static int logged;
    if (!__atomic_exchange_n(&logged,1,__ATOMIC_RELAXED))
        printf("Runtime: Audio3d first push (%s)\n",blocking==BLOCKING_SYNC ? "blocking" : "async");
    pthread_mutex_lock(&p->push_lock);
    float granule[MAX_GRANULARITY*2];
    for (;;) {
        pthread_mutex_lock(&p->lock);
        if (!p->count) { pthread_mutex_unlock(&p->lock); break; }
        memcpy(granule,p->queue+(size_t)p->head*p->granularity*2,(size_t)p->granularity*2*sizeof(float));
        p->head=(p->head+1)%p->queue_depth; --p->count;
        pthread_mutex_unlock(&p->lock);
        /* SYNC returns once the granule's period has elapsed, like the hardware queue. */
        if (p->out>=0) runtime_audio_host_output(p->out,granule,blocking==BLOCKING_SYNC);
        __atomic_add_fetch(&stat_pushed,1,__ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&p->push_lock);
    return 0;
}
static ABI int32_t a3d_port_get_queue_level(uint32_t port_id, uint32_t *level, uint32_t *available) {
    ONCE("sceAudio3dPortGetQueueLevel");
    Port *p=port_of(port_id);
    if (!p) return A3D_ERR_INVALID_PORT;
    if (!level && !available) return A3D_ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&p->lock);
    if (level) *level=p->count;
    if (available) *available=p->queue_depth-p->count;
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t a3d_terminate(void) {
    ONCE("sceAudio3dTerminate");
    for (uint32_t i=0;i<MAX_PORTS;++i) if (ports[i].used) a3d_port_close(i);
    pthread_mutex_lock(&table_lock);
    initialized=0;
    pthread_mutex_unlock(&table_lock);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceAudio3dInitialize",a3d_initialize}, {"sceAudio3dTerminate",a3d_terminate},
    {"sceAudio3dGetDefaultOpenParameters",a3d_get_default_open_parameters},
    {"sceAudio3dPortOpen",a3d_port_open}, {"sceAudio3dPortClose",a3d_port_close},
    {"sceAudio3dPortSetAttribute",a3d_port_set_attribute}, {"sceAudio3dPortPush",a3d_port_push},
    {"sceAudio3dPortAdvance",a3d_port_advance}, {"sceAudio3dPortGetQueueLevel",a3d_port_get_queue_level},
    {"sceAudio3dObjectReserve",a3d_object_reserve}, {"sceAudio3dObjectUnreserve",a3d_object_unreserve},
    {"sceAudio3dObjectSetAttributes",a3d_object_set_attributes},
};
uintptr_t runtime_gr2_audio3d_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_gr2_audio3d_report(void) {
    if (!stat_advanced && !stat_reserved) return;
    printf("Runtime: Audio3d granules advanced=%llu pushed=%llu dropped=%llu, objects reserved=%llu (peak %llu), "
           "pcm sets=%llu, non-silent samples=%llu/%llu, peak %.3f\n",
           (unsigned long long)stat_advanced,(unsigned long long)__atomic_load_n(&stat_pushed,__ATOMIC_RELAXED),
           (unsigned long long)stat_dropped,(unsigned long long)stat_reserved,(unsigned long long)stat_peak_objects,
           (unsigned long long)stat_pcm_sets,(unsigned long long)stat_nonsilent,(unsigned long long)stat_samples,stat_peak);
    fflush(stdout);
}
