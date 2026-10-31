/* Gravity Rush 2: PSN and network (NpToolkit, NpManager, Http, Ssl, Net, ShareUtility) as an
 * offline console whose user is signed out of PSN.
 *
 * NpToolkit is the C++ library the game ships as sce_module/libSceNpToolkit.prx. It is not linked:
 * its own imports reach ~25 more libraries (Matching2, WebApi, Score, Tus, dialogs...). The game
 * only uses a small surface of it, reimplemented here with the layouts read from that prx (1.00):
 *  - constructors of request/parameter structs write exactly what the prx constructors write;
 *  - Utilities::Future<T> = FutureImpl (0x20 bytes: condattr, cond, mutex pointers, state, pad),
 *    additional info (0x210 bytes), then T at +0x230. state 0x400 = empty, -1 = busy,
 *    0 = result, other negative = error. No OS objects are created: nothing ever waits on them;
 *  - every service request completes at once with SCE_NP_ERROR_SIGNED_OUT in its Future
 *    (an async call returns 0 like a queued request, a sync call returns the error);
 *    requests answered only through the event callback (trophy, commerce, TUS set) fail
 *    synchronously so the game never waits for an event;
 *  - AppSTLAllocator forwards to the AllocImplementation the game passes in Parameters,
 *    since the game builds its own strings and vectors with it.
 * std::basic_string<char, ..., AppSTLAllocator> is Dinkumware: allocator (8), buffer/pointer (16),
 * size (8), capacity (8); heap storage when capacity >= 16, freed with capacity + 1 bytes.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define TOOLKIT_INVALID_ARGUMENT ((int32_t)0x80552c00) /* null Future/request (prx value) */
#define TOOLKIT_INVALID_PARAMS ((int32_t)0x80552c07)   /* Parameters with user data (prx value) */
#define TOOLKIT_NO_ALLOCATOR ((int32_t)0x80552cf4)     /* Parameters without allocator (prx value) */
#define TROPHY_UNKNOWN ((int32_t)0x80551600)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_OUT_OF_MEMORY ((int32_t)0x80431022)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_PARSE_NOT_FOUND ((int32_t)0x80432025)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_ENOTCONN ((int32_t)0x80410139)
#define NET_EINVAL ((int32_t)0x80410116)
#define SSL_INVALID_CERT ((int32_t)0x8095f005)

static void once(const char *what) {
    /* First call of a placeholder is logged; the list is short and fixed. */
    static const char *seen[64];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<64) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (offline)\n",what);
}

#define AT(p,off,type) (*(type *)((char *)(p)+(off)))

/* ---- AppSTLAllocator: the game's AllocImplementation (vtable: allocate(int), deallocate(void*,unsigned)) ---- */
typedef void *(ABI *AllocFn)(void *self,int32_t size);
typedef void (ABI *FreeFn)(void *self,void *p,uint32_t size);
static void *_Atomic external_alloc; /* AllocImplementation::externalAlloc */

static void *tk_alloc(uint64_t bytes) {
    void *impl=atomic_load(&external_alloc);
    if (!impl) { once("NpToolkit AppSTLAllocator::allocate before Parameters"); return NULL; }
    return (*(AllocFn **)impl)[0](impl,(int32_t)bytes);
}
static void tk_free(void *p,uint64_t n) {
    void *impl=atomic_load(&external_alloc);
    if (impl) (*(FreeFn **)impl)[1](impl,p,(uint32_t)n);
}
static ABI void *alloc_char(void *self,uint64_t n,const void *hint) { (void)self; (void)hint; return tk_alloc(n); }
static ABI void *alloc_npuser(void *self,uint64_t n,const void *hint) { (void)self; (void)hint; return tk_alloc((uint32_t)n*0x310u); }
static ABI void *alloc_tus_node(void *self,uint64_t n,const void *hint) { (void)self; (void)hint; return tk_alloc(n<<5); }
static ABI void dealloc_any(void *self,void *p,uint64_t n) { (void)self; tk_free(p,n); }
static ABI uint64_t max_size_char(const void *self) { (void)self; return UINT64_MAX; }
static ABI uint64_t max_size_npuser(const void *self) { (void)self; return UINT64_C(0x5397829cbc14e5); }
static ABI uint64_t max_size_tus_node(const void *self) { (void)self; return UINT64_C(0x7ffffffffffffff); }
static ABI int alloc_equal(const void *a,const void *b) { (void)a; (void)b; return 1; }
static ABI void nothing(void *self) { (void)self; }
static ABI void nothing2(void *self,const void *other) { (void)self; (void)other; }

/* ---- Dinkumware strings inside toolkit structs ---- */
static void str_init(void *s) { AT(s,8,uint8_t)=0; AT(s,0x18,uint64_t)=0; AT(s,0x20,uint64_t)=15; }
static void str_free(void *s) {
    uint64_t cap=AT(s,0x20,uint64_t);
    if (cap>=16) tk_free(AT(s,8,void *),cap+1);
    str_init(s);
}
static void str_copy(void *d,const void *s) {
    str_init(d);
    uint64_t n=AT(s,0x18,uint64_t);
    const char *src=AT(s,0x20,uint64_t)>=16 ? AT(s,8,const char *) : (const char *)s+8;
    char *dst=(char *)d+8;
    if (n>=16) {
        uint64_t cap=n|15;
        char *p=tk_alloc(cap+1);
        if (!p) return;
        AT(d,8,char *)=p; AT(d,0x20,uint64_t)=cap; dst=p;
    }
    memcpy(dst,src,n); dst[n]=0; AT(d,0x18,uint64_t)=n;
}

/* NpUser (0x310 bytes): strings at these offsets, plain data in between. */
static const uint32_t npuser_strings[]={0x00,0x38,0x60,0x88,0x130,0x2c0};
#define NPUSER_SIZE 0x310
static void npuser_init(void *u) { for (unsigned i=0;i<6;++i) str_init((char *)u+npuser_strings[i]); }
static ABI void npuser_destroy(void *self,void *u) {
    (void)self;
    for (int i=5;i>=0;--i) str_free((char *)u+npuser_strings[i]);
}
static ABI void npuser_construct(void *self,void *d,const void *s) {
    (void)self;
    memcpy(d,s,NPUSER_SIZE); /* plain fields; strings are rebuilt below */
    for (unsigned i=0;i<6;++i) str_copy((char *)d+npuser_strings[i],(const char *)s+npuser_strings[i]);
}

/* ---- Utilities::FutureImpl ---- */
#define FUTURE_EMPTY 0x400
#define FUTURE_BUSY (-1)
#define FUTURE_RESULT 0x230
static _Atomic int32_t *future_state(void *f) { return (_Atomic int32_t *)((char *)f+0x18); }
static void future_init(void *f,uint64_t size) {
    memset((char *)f+0x20,0,size-0x20);
    memset(f,0,0x18);
    atomic_store(future_state(f),FUTURE_EMPTY);
    AT(f,0x1c,int32_t)=0;
}
static void future_fail(void *f,int32_t error) { atomic_store(future_state(f),error); }
static ABI void future_destroy(void *f) { atomic_store(future_state(f),FUTURE_EMPTY); }
static ABI int future_has_result(void *f) { return atomic_load(future_state(f))==0; }
static ABI int future_has_error(void *f) { int32_t s=atomic_load(future_state(f)); return s<0 && s!=FUTURE_BUSY; }
static ABI int32_t future_error(void *f) { int32_t s=atomic_load(future_state(f)); return s<0 && s!=FUTURE_BUSY ? s : 0; }
static ABI int future_is_busy(void *f) { return atomic_load(future_state(f))==FUTURE_BUSY; }
static ABI int future_reset(void *f) {
    if (atomic_load(future_state(f))==FUTURE_BUSY) return 0;
    atomic_store(future_state(f),FUTURE_EMPTY); return 1;
}
static ABI void *future_get(void *f) { return future_has_result(f) ? (char *)f+FUTURE_RESULT : NULL; }

/* Future<T> constructors: total object sizes from the prx constructors. */
static ABI void future_activity_story(void *f) { future_init(f,0x358); }
static ABI void future_ticket(void *f) { future_init(f,0x340); }
static ABI void future_tss_data(void *f) { future_init(f,0x378); }
static ABI void future_tss_status(void *f) { future_init(f,0x348); }
static ABI void future_country(void *f) { future_init(f,0x338); }
static ABI void future_npuser_vector(void *f) { future_init(f,0x350); }
static ABI void future_string(void *f) { future_init(f,0x358); str_init((char *)f+FUTURE_RESULT); }
/* NpUsers: count/header (8) then 50 NpUser. */
#define NPUSERS_FIRST (FUTURE_RESULT+8)
static ABI void future_npusers(void *f) {
    future_init(f,0x9c58);
    for (unsigned i=0;i<50;++i) npuser_init((char *)f+NPUSERS_FIRST+i*NPUSER_SIZE);
}
static ABI void future_string_destroy(void *f) { str_free((char *)f+FUTURE_RESULT); future_destroy(f); }
static ABI void future_npusers_destroy(void *f) {
    for (int i=49;i>=0;--i) npuser_destroy(NULL,(char *)f+NPUSERS_FIRST+i*NPUSER_SIZE);
    future_destroy(f);
}
/* vector<NpUser>: allocator, begin, end, capacity end. */
static ABI void future_npuser_vector_destroy(void *f) {
    char *begin=AT(f,0x238,char *),*end=AT(f,0x240,char *),*cap=AT(f,0x248,char *);
    if (begin) {
        for (char *u=begin;u<end;u+=NPUSER_SIZE) npuser_destroy(NULL,u);
        tk_free(begin,(uint64_t)(cap-begin)/NPUSER_SIZE);
        AT(f,0x238,void *)=NULL; AT(f,0x240,void *)=NULL; AT(f,0x248,void *)=NULL;
    }
    future_destroy(f);
}

/* ---- Request and parameter structs ---- */
static ABI void request_ctor(void *r) { memset(r,0,0x2c); }
static ABI void title_id_ctor(void *t) { memset(t,0,0x90); }
static ABI void user_profile_request(void *r) { request_ctor(r); memset((char *)r+0x30,0,0x28); }
static ABI void friend_info_request(void *r) { request_ctor(r); AT(r,0x30,uint64_t)=0; AT(r,0x38,int32_t)=0; }
static ABI void post_in_game_request(void *r) { request_ctor(r); memset((char *)r+0x30,0,0x144); }
static ABI void register_trophy_request(void *r) { request_ctor(r); AT(r,0x30,int32_t)=-1; memset((char *)r+0x34,0,0x10); }
static ABI void category_browse_params(void *r) {
    request_ctor(r); memset((char *)r+0x30,0,0x40);
    AT(r,0x70,int32_t)=0; AT(r,0x74,int32_t)=-1; AT(r,0x78,uint64_t)=0;
}
static ABI void voucher_input_params(void *r) {
    request_ctor(r); AT(r,0x30,uint64_t)=0; AT(r,0x38,int32_t)=0; AT(r,0x3c,int32_t)=-1; AT(r,0x40,uint64_t)=0;
}
static ABI void access_code_params(void *r) { request_ctor(r); str_init((char *)r+0x30); str_init((char *)r+0x58); }
static ABI void tss_input_params(void *r) { memset(r,0,0x10); AT(r,0x10,int32_t)=0; AT(r,0x14,int32_t)=-1; }
static ABI void tss_status_params(void *r) { AT(r,0,int32_t)=0; AT(r,4,int32_t)=-1; }
static ABI void tus_variable_ctor(void *v,int32_t slot,int64_t value) { AT(v,0,int64_t)=value; AT(v,8,int32_t)=slot; }
static ABI void tus_get_vars_params(void *r) { request_ctor(r); AT(r,0x30,uint64_t)=0; AT(r,0x9c,int32_t)=-1; AT(r,0xa0,uint8_t)=0; }
static ABI void tus_get_data_params(void *r) { request_ctor(r); AT(r,0x30,int32_t)=0; AT(r,0x58,int32_t)=-1; AT(r,0x5c,uint8_t)=0; }
static ABI void tus_set_data_params(void *r) {
    request_ctor(r); memset((char *)r+0x30,0,0x20); memset((char *)r+0x74,0,0x20);
    AT(r,0x94,int32_t)=0; AT(r,0x98,int32_t)=0; AT(r,0x9c,int32_t)=-1;
    memset((char *)r+0xa0,0,0x180); AT(r,0x220,uint8_t)=0;
}
/* std::list<TusVariable> at 0x30 (allocator, head node, size): the head is a 0x20-byte node. */
static ABI void tus_set_vars_params(void *r) {
    request_ctor(r); memset((char *)r+0x38,0,0x10);
    void **node=tk_alloc(0x20);
    if (node) { node[0]=node; node[1]=node; }
    AT(r,0x38,void *)=node; AT(r,0x6c,int32_t)=-1; AT(r,0x70,uint8_t)=0;
}
/* Parameters(callback, NpTitleId&, userData, size, AllocImplementation*) */
static ABI void parameters_ctor(void *p,void *callback,const void *title,void *user,uint64_t size,void *alloc) {
    AT(p,0,uint64_t)=0;
    str_init((char *)p+8);
    AT(p,0x30,void *)=user; AT(p,0x38,uint64_t)=size;
    memset((char *)p+0x40,0,0x30);
    memcpy((char *)p+0x70,title,0x90);
    AT(p,0x100,void *)=callback; AT(p,0x108,uint64_t)=0;
    AT(p,0x110,int64_t)=-1; AT(p,0x118,int64_t)=-1;
    memset((char *)p+0x128,0,0x80);
    if (alloc) atomic_store(&external_alloc,alloc);
}

/* ---- Interface: the toolkit starts, every PSN service answers "signed out" ---- */
static _Atomic int toolkit_ready;
static ABI int32_t toolkit_init(void *params,int async) {
    AT(params,0x120,uint8_t)=!async;
    if (AT(params,0x30,void *) || AT(params,0x38,uint64_t)) return TOOLKIT_INVALID_PARAMS;
    if (!atomic_load(&external_alloc)) return TOOLKIT_NO_ALLOCATOR;
    atomic_store(&toolkit_ready,1);
    printf("Runtime: NpToolkit initialized offline (user signed out of PSN)\n");
    return 0;
}
static ABI int32_t toolkit_terminate(void) {
    atomic_store(&toolkit_ready,0);
    atomic_store(&external_alloc,NULL);
    return 0;
}
static ABI int32_t toolkit_terminate_service(int32_t service) { (void)service; return 0; }
static ABI int32_t toolkit_abort(const uint32_t *id) { (void)id; return 0; }

static int32_t answer(void *future,const void *request,int async,const char *what) {
    once(what);
    if (!future || !request) return TOOLKIT_INVALID_ARGUMENT;
    future_fail(future,NP_SIGNED_OUT);
    return async ? 0 : NP_SIGNED_OUT;
}
static ABI int32_t get_avatar_url(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit UserProfile::getAvatarUrl"); }
static ABI int32_t get_country_info(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit UserProfile::getCountryInfo"); }
static ABI int32_t get_np_users(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit UserProfile::getNpUsersInformation"); }
static ABI int32_t post_in_game(const void *r,void *f,int a) { return answer(f,r,a,"NpToolkit ActivityFeed::postInGame"); }
static ABI int32_t get_access_code(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit Auth::getUserAccessCode"); }
static ABI int32_t get_friends(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit Friends::getFriendslist"); }
static ABI int32_t tss_get_status(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit TSS::getDataStatus"); }
static ABI int32_t tss_get_slot(void *f,const void *r,int a) { return answer(f,r,a,"NpToolkit TSS::getDataFromSlot"); }
static ABI int32_t tss_get(void *f,int a) { return answer(f,f,a,"NpToolkit TSS::getData"); }
static ABI int32_t tus_get_vars(void *f,void *r,int a) { return answer(f,r,a,"NpToolkit TUS::getVariables"); }
static ABI int32_t tus_get_data(void *f,void *r,int a) { return answer(f,r,a,"NpToolkit TUS::getData"); }
/* Answered only through events on a console: fail now so nothing waits for an event. */
static ABI int32_t tus_set_vars(void *r,int a) { (void)r; (void)a; once("NpToolkit TUS::setVariables"); return NP_SIGNED_OUT; }
static ABI int32_t tus_set_data(void *r,int a) { (void)r; (void)a; once("NpToolkit TUS::setData"); return NP_SIGNED_OUT; }
static ABI int32_t category_browse(const void *r,int a) { (void)r; (void)a; once("NpToolkit Commerce::categoryBrowse"); return NP_SIGNED_OUT; }
static ABI int32_t voucher_input(const void *r,int a) { (void)r; (void)a; once("NpToolkit Commerce::voucherCodeInput"); return NP_SIGNED_OUT; }
static ABI int32_t trophy_register(const void *r,int a) { (void)r; (void)a; once("NpToolkit Trophy::trophyRegisterSet (trophies off)"); return TROPHY_UNKNOWN; }
static ABI int32_t trophy_unlock(const void *r,int a) { (void)r; (void)a; once("NpToolkit Trophy::trophyUnlock (trophies off)"); return TROPHY_UNKNOWN; }

/* ---- NpManager ---- */
static _Atomic int32_t next_request=0x100;
static ABI int32_t np_create_request(void) { return atomic_fetch_add(&next_request,1); }
static ABI int32_t np_wait_async(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
/* PS4 language id (sceSystemServiceParamGetInt, TLG_LANGUAGE) -> account language and country. */
static void account_locale(const char **lang,const char **country) {
    static const char *const langs[][2]={
        {"ja","jp"},{"en","us"},{"fr","fr"},{"es","es"},{"de","de"},{"it","it"},{"nl","nl"},{"pt","pt"},
        {"ru","ru"},{"ko","kr"},{"zh-TW","tw"},{"zh-CN","cn"},{"fi","fi"},{"sv","se"},{"da","dk"},{"no","no"},
        {"pl","pl"},{"pt-BR","br"},{"en-GB","gb"},{"tr","tr"},{"es-LA","mx"},{"ar","ae"},{"fr-CA","ca"},
        {"cs","cz"},{"hu","hu"},{"el","gr"},{"ro","ro"},{"th","th"},{"vi","vn"},{"id","id"}};
    const char *v=getenv("TLG_LANGUAGE");
    int id=v ? atoi(v) : 1;
    if (id<0 || id>=(int)(sizeof(langs)/sizeof(*langs))) id=1;
    *lang=langs[id][0]; *country=langs[id][1];
}
/* Out-params get the system locale, but the user is signed out, as on a console. */
static ABI int32_t np_account_country(const void *online_id,char *country) {
    const char *lang,*code; (void)online_id;
    if (!country) return NP_INVALID_ARGUMENT;
    account_locale(&lang,&code);
    memset(country,0,4); memcpy(country,code,2);
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_language(int32_t request,const void *online_id,char *language) {
    const char *lang,*code; (void)request; (void)online_id;
    if (!language) return NP_INVALID_ARGUMENT;
    account_locale(&lang,&code);
    memset(language,0,6); memcpy(language,lang,strlen(lang)<5 ? strlen(lang) : 5);
    return NP_SIGNED_OUT;
}
static ABI int32_t np_account_id(const void *online_id,uint64_t *account) {
    (void)online_id; if (account) *account=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_user_by_online_id(const void *online_id,int32_t *user) {
    (void)online_id; if (user) *user=-1; return NP_SIGNED_OUT;
}
static ABI int32_t np_register_reachability(void *callback,void *user) {
    (void)callback; (void)user; /* never called: the state stays "unreachable" */
    once("sceNpRegisterNpReachabilityStateCallback");
    return 0;
}
static ABI int32_t np_unregister_reachability(void) { return 0; }

/* ---- Http: requests never reach a server (sceHttpSendRequest fails in runtime_services.c) ---- */
static ABI int32_t ok(void) { return 0; }
static ABI int32_t http_all_headers(int32_t request,char **header,uint64_t *size) {
    (void)request; if (header) *header=NULL; if (size) *size=0; return HTTP_NETWORK;
}
static ABI int32_t http_last_errno(int32_t request,int32_t *err) { (void)request; if (err) *err=51; return 0; } /* ENETUNREACH */
static int same_ci(const char *a,const char *b,uint64_t n) {
    for (uint64_t i=0;i<n;++i) if ((a[i]|32)!=(b[i]|32) || !a[i]) return 0; /* header names are ASCII */
    return 1;
}
static ABI int32_t http_parse_header(const char *header,uint64_t len,const char *field,const char **value,uint64_t *vlen) {
    if (!header || !field || !value || !vlen) return HTTP_INVALID_VALUE;
    uint64_t flen=strlen(field);
    for (uint64_t i=0;i+flen<len;) {
        uint64_t e=i; while (e<len && header[e]!='\n') ++e;
        if (e-i>flen && same_ci(header+i,field,flen) && header[i+flen]==':') {
            uint64_t v=i+flen+1; while (v<e && (header[v]==' ' || header[v]=='\t')) ++v;
            uint64_t end=e; while (end>v && (header[end-1]=='\r' || header[end-1]==' ')) --end;
            *value=header+v; *vlen=end-v; return 0;
        }
        i=e+1;
    }
    return HTTP_PARSE_NOT_FOUND;
}
static ABI int32_t https_ssl_error(int32_t id,int32_t *err,uint32_t *detail) {
    (void)id; if (err) *err=0; if (detail) *detail=0; return 0;
}
static int unreserved(unsigned char c) {
    return (c>='0' && c<='9') || ((c|32)>='a' && (c|32)<='z') || c=='-' || c=='.' || c=='_' || c=='~';
}
/* RFC 3986: unreserved characters stay, the rest becomes %XX; require counts the terminator. */
static ABI int32_t http_uri_escape(char *out,uint64_t *require,uint64_t prepare,const char *in) {
    static const char hex[]="0123456789ABCDEF";
    if (!in) return HTTP_INVALID_VALUE;
    uint64_t need=1;
    for (const unsigned char *c=(const unsigned char *)in;*c;++c)
        need+=unreserved(*c) ? 1 : 3;
    if (require) *require=need;
    if (!out) return 0;
    if (prepare<need) return HTTP_OUT_OF_MEMORY;
    char *o=out;
    for (const unsigned char *c=(const unsigned char *)in;*c;++c) {
        if (unreserved(*c)) *o++=(char)*c;
        else { *o++='%'; *o++=hex[*c>>4]; *o++=hex[*c&15]; }
    }
    *o=0;
    return 0;
}

/* ---- Ssl: only reached from the https certificate callback, which never runs ---- */
static ABI int32_t ssl_no_cert(void) { return SSL_INVALID_CERT; }
static ABI int32_t ssl_pool_stats(int32_t ctx,uint64_t *stats) {
    (void)ctx; if (stats) memset(stats,0,5*sizeof(uint64_t)); return 0;
}

/* ---- Net ---- */
static ABI int32_t net_mac_address(uint8_t *mac,int32_t flags) {
    (void)flags;
    if (!mac) return NET_EINVAL;
    static const uint8_t fixed[6]={0x02,0x00,0x00,0x00,0x47,0x52}; /* locally administered */
    memcpy(mac,fixed,6); return 0;
}
static ABI int32_t net_not_connected(void) { return NET_ENOTCONN; }
static ABI int32_t net_unreachable(void) { return NET_ENETUNREACH; }
static ABI int32_t net_resolver_error(int32_t resolver,int32_t *status) {
    (void)resolver; if (status) *status=NET_ENETUNREACH; return 0;
}

/* ---- ShareUtility: the share menu does not exist on PC ---- */
static ABI int32_t share_open(void) { once("sceShareUtilityOpenShareMenu"); return 0; }

#define FUT "_ZN3sce7Toolkit2NP9Utilities6Future"
#define ALC "_ZN3sce7Toolkit2NP15AppSTLAllocator"
static const RuntimeExport exports[]={
    /* FutureImpl */
    {"_ZN3sce7Toolkit2NP9Utilities10FutureImpl5resetEv",future_reset},
    {"_ZN3sce7Toolkit2NP9Utilities10FutureImpl6isBusyEv",future_is_busy},
    {"_ZNK3sce7Toolkit2NP9Utilities10FutureImpl8getErrorEv",future_error},
    {"_ZNK3sce7Toolkit2NP9Utilities10FutureImpl8hasErrorEv",future_has_error},
    {"_ZNK3sce7Toolkit2NP9Utilities10FutureImpl9hasResultEv",future_has_result},
    /* Future<T> */
    {FUT "I18SceNpTssDataStatusE3getEv",future_get}, {FUT "I18SceNpTssDataStatusEC1Ev",future_tss_status},
    {FUT "INS1_11CountryInfoEE3getEv",future_get}, {FUT "INS1_11CountryInfoEEC1Ev",future_country},
    {FUT "INS1_11CountryInfoEED1Ev",future_destroy},
    {FUT "INS1_15ActivityStoryIdEE3getEv",future_get}, {FUT "INS1_15ActivityStoryIdEEC1Ev",future_activity_story},
    {FUT "INS1_15ActivityStoryIdEED1Ev",future_destroy},
    {FUT "INS1_6TicketEE3getEv",future_get}, {FUT "INS1_6TicketEEC1Ev",future_ticket},
    {FUT "INS1_6TicketEED1Ev",future_destroy},
    {FUT "INS1_7NpUsersEE3getEv",future_get}, {FUT "INS1_7NpUsersEEC1Ev",future_npusers},
    {FUT "INS1_7NpUsersEED1Ev",future_npusers_destroy},
    {FUT "INS1_7TssDataEE3getEv",future_get}, {FUT "INS1_7TssDataEEC1Ev",future_tss_data},
    {FUT "ISbIcSt11char_traitsIcENS1_15AppSTLAllocatorIcEEEE3getEv",future_get},
    {FUT "ISbIcSt11char_traitsIcENS1_15AppSTLAllocatorIcEEEEC1Ev",future_string},
    {FUT "ISbIcSt11char_traitsIcENS1_15AppSTLAllocatorIcEEEED1Ev",future_string_destroy},
    {FUT "ISt6vectorINS1_6NpUserENS1_15AppSTLAllocatorIS5_EEEE3getEv",future_get},
    {FUT "ISt6vectorINS1_6NpUserENS1_15AppSTLAllocatorIS5_EEEEC1Ev",future_npuser_vector},
    {FUT "ISt6vectorINS1_6NpUserENS1_15AppSTLAllocatorIS5_EEEED1Ev",future_npuser_vector_destroy},
    {FUT "INS1_23CheckAvailabilityResultEED1Ev",future_destroy}, /* NID czlBWbAx1aU */
    /* AppSTLAllocator<char> */
    {ALC "IcE10deallocateEPcm",dealloc_any}, {ALC "IcE8allocateEmPKv",alloc_char},
    {ALC "IcEC1ERKS3_",nothing2}, {ALC "IcEC2ERKS3_",nothing2}, {ALC "IcEC2Ev",nothing},
    {ALC "IcED1Ev",nothing}, {ALC "IcED2Ev",nothing}, {ALC "IcEeqERKS3_",alloc_equal},
    {"_ZNK3sce7Toolkit2NP15AppSTLAllocatorIcE8max_sizeEv",max_size_char},
    /* AppSTLAllocator<NpUser> */
    {ALC "INS1_6NpUserEE10deallocateEPS3_m",dealloc_any}, {ALC "INS1_6NpUserEE7destroyEPS3_",npuser_destroy},
    {ALC "INS1_6NpUserEE8allocateEmPKv",alloc_npuser}, {ALC "INS1_6NpUserEE9constructEPS3_RKS3_",npuser_construct},
    {ALC "INS1_6NpUserEEC1ERKS4_",nothing2}, {ALC "INS1_6NpUserEEC1Ev",nothing}, {ALC "INS1_6NpUserEEC2Ev",nothing},
    {ALC "INS1_6NpUserEED1Ev",nothing}, {ALC "INS1_6NpUserEED2Ev",nothing}, {ALC "INS1_6NpUserEEeqERKS4_",alloc_equal},
    {"_ZNK3sce7Toolkit2NP15AppSTLAllocatorINS1_6NpUserEE8max_sizeEv",max_size_npuser},
    /* AppSTLAllocator<_List_node<TusVariable, void*>> */
    {ALC "ISt10_List_nodeINS1_11TusVariableEPvEE10deallocateEPS6_m",dealloc_any},
    {ALC "ISt10_List_nodeINS1_11TusVariableEPvEE8allocateEmPKv",alloc_tus_node},
    {ALC "ISt10_List_nodeINS1_11TusVariableEPvEEC2Ev",nothing},
    {ALC "ISt10_List_nodeINS1_11TusVariableEPvEED2Ev",nothing},
    {"_ZNK3sce7Toolkit2NP15AppSTLAllocatorISt10_List_nodeINS1_11TusVariableEPvEE8max_sizeEv",max_size_tus_node},
    {"_ZN3sce7Toolkit2NP19AllocImplementationD2Ev",nothing},
    /* Structs */
    {"_ZN3sce7Toolkit2NP10ParametersC1EPFvRKNS1_5EventEERNS1_9NpTitleIdEPvmPNS1_19AllocImplementationE",parameters_ctor},
    {"_ZN3sce7Toolkit2NP9NpTitleIdC1Ev",title_id_ctor},
    {"_ZN3sce7Toolkit2NP7RequestC2Ev",request_ctor},
    {"_ZN3sce7Toolkit2NP11TusVariableC1Eil",tus_variable_ctor},
    {"_ZN3sce7Toolkit2NP14TssInputParamsC1Ev",tss_input_params},
    {"_ZN3sce7Toolkit2NP17FriendInfoRequestC1Ev",friend_info_request},
    {"_ZN3sce7Toolkit2NP17PostInGameRequestC1Ev",post_in_game_request},
    {"_ZN3sce7Toolkit2NP18UserProfileRequestC1Ev",user_profile_request},
    {"_ZN3sce7Toolkit2NP18VoucherInputParamsC1Ev",voucher_input_params},
    {"_ZN3sce7Toolkit2NP20CategoryBrowseParamsC1Ev",category_browse_params},
    {"_ZN3sce7Toolkit2NP21AccessCodeInputParamsC1Ev",access_code_params},
    {"_ZN3sce7Toolkit2NP21RegisterTrophyRequestC1Ev",register_trophy_request},
    {"_ZN3sce7Toolkit2NP21TusGetDataInputParamsC1Ev",tus_get_data_params},
    {"_ZN3sce7Toolkit2NP21TusGetVarsInputParamsC1Ev",tus_get_vars_params},
    {"_ZN3sce7Toolkit2NP21TusSetDataInputParamsC1Ev",tus_set_data_params},
    {"_ZN3sce7Toolkit2NP21TusSetVarsInputParamsC1Ev",tus_set_vars_params},
    {"_ZN3sce7Toolkit2NP23TssGetStatusInputParamsC1Ev",tss_status_params},
    /* Interfaces */
    {"_ZN3sce7Toolkit2NP9Interface4initERNS1_10ParametersEb",toolkit_init},
    {"_ZN3sce7Toolkit2NP9Interface9terminateEv",toolkit_terminate},
    {"_ZN3sce7Toolkit2NP9Interface16terminateServiceENS1_11ServiceTypeE",toolkit_terminate_service},
    {"_ZN3sce7Toolkit2NP9Interface12abortRequestERKj",toolkit_abort},
    {"_ZN3sce7Toolkit2NP11UserProfile9Interface12getAvatarUrlEPNS1_9Utilities6FutureISbIcSt11char_traitsIcENS1_15AppSTLAllocatorIcEEEEEPKNS1_18UserProfileRequestEb",get_avatar_url},
    {"_ZN3sce7Toolkit2NP11UserProfile9Interface14getCountryInfoEPNS1_9Utilities6FutureINS1_11CountryInfoEEEPKNS1_18UserProfileRequestEb",get_country_info},
    {"_ZN3sce7Toolkit2NP11UserProfile9Interface21getNpUsersInformationEPNS1_9Utilities6FutureINS1_7NpUsersEEEPKNS1_18UserProfileRequestEb",get_np_users},
    {"_ZN3sce7Toolkit2NP12ActivityFeed9Interface10postInGameEPKNS1_17PostInGameRequestEPNS1_9Utilities6FutureINS1_15ActivityStoryIdEEEb",post_in_game},
    {"_ZN3sce7Toolkit2NP4Auth9Interface17getUserAccessCodeEPNS1_9Utilities6FutureINS1_6TicketEEERKNS1_21AccessCodeInputParamsEb",get_access_code},
    {"_ZN3sce7Toolkit2NP7Friends9Interface14getFriendslistEPNS1_9Utilities6FutureISt6vectorINS1_6NpUserENS1_15AppSTLAllocatorIS7_EEEEEPKNS1_17FriendInfoRequestEb",get_friends},
    {"_ZN3sce7Toolkit2NP3TSS9Interface13getDataStatusEPNS1_9Utilities6FutureI18SceNpTssDataStatusEERKNS1_23TssGetStatusInputParamsEb",tss_get_status},
    {"_ZN3sce7Toolkit2NP3TSS9Interface15getDataFromSlotEPNS1_9Utilities6FutureINS1_7TssDataEEERKNS1_14TssInputParamsEb",tss_get_slot},
    {"_ZN3sce7Toolkit2NP3TSS9Interface7getDataEPNS1_9Utilities6FutureINS1_7TssDataEEEb",tss_get},
    {"_ZN3sce7Toolkit2NP3TUS9Interface12getVariablesEPNS1_9Utilities6FutureISt6vectorI16SceNpTusVariableNS1_15AppSTLAllocatorIS7_EEEEERNS1_21TusGetVarsInputParamsEb",tus_get_vars},
    {"_ZN3sce7Toolkit2NP3TUS9Interface12setVariablesERNS1_21TusSetVarsInputParamsEb",tus_set_vars},
    {"_ZN3sce7Toolkit2NP3TUS9Interface7getDataEPNS1_9Utilities6FutureINS1_13TusDataOutputEEERNS1_21TusGetDataInputParamsEb",tus_get_data},
    {"_ZN3sce7Toolkit2NP3TUS9Interface7setDataERNS1_21TusSetDataInputParamsEb",tus_set_data},
    {"_ZN3sce7Toolkit2NP6Trophy9Interface12trophyUnlockEPKNS1_19UnlockTrophyRequestEb",trophy_unlock},
    {"_ZN3sce7Toolkit2NP6Trophy9Interface17trophyRegisterSetEPKNS1_21RegisterTrophyRequestEb",trophy_register},
    {"_ZN3sce7Toolkit2NP8Commerce9Interface14categoryBrowseERKNS1_20CategoryBrowseParamsEb",category_browse},
    {"_ZN3sce7Toolkit2NP8Commerce9Interface16voucherCodeInputERKNS1_18VoucherInputParamsEb",voucher_input},
    /* NpManager */
    {"sceNpCreateRequest",np_create_request}, {"sceNpWaitAsync",np_wait_async},
    {"sceNpGetAccountCountry",np_account_country}, {"sceNpGetAccountLanguage",np_account_language},
    {"sceNpGetAccountId",np_account_id}, {"sceNpGetUserIdByOnlineId",np_user_by_online_id},
    {"sceNpRegisterNpReachabilityStateCallback",np_register_reachability},
    {"sceNpUnregisterNpReachabilityStateCallback",np_unregister_reachability},
    /* Http */
    {"sceHttpAbortRequest",ok}, {"sceHttpGetAllResponseHeaders",http_all_headers},
    {"sceHttpGetLastErrno",http_last_errno}, {"sceHttpParseResponseHeader",http_parse_header},
    {"sceHttpSetAuthEnabled",ok}, {"sceHttpSetAutoRedirect",ok}, {"sceHttpSetRecvTimeOut",ok},
    {"sceHttpSetResolveRetry",ok}, {"sceHttpSetResolveTimeOut",ok}, {"sceHttpSetSendTimeOut",ok},
    {"sceHttpsGetSslError",https_ssl_error}, {"sceHttpsSetSslCallback",ok}, {"sceHttpUriEscape",http_uri_escape},
    /* Ssl */
    {"sceSslFreeSslCertName",ok}, {"sceSslGetIssuerName",ssl_no_cert}, {"sceSslGetMemoryPoolStats",ssl_pool_stats},
    {"sceSslGetNameEntryCount",ssl_no_cert}, {"sceSslGetNameEntryInfo",ssl_no_cert},
    {"sceSslGetNotAfter",ssl_no_cert}, {"sceSslGetNotBefore",ssl_no_cert},
    {"sceSslGetSerialNumber",ssl_no_cert}, {"sceSslGetSubjectName",ssl_no_cert},
    /* Net */
    {"sceNetGetMacAddress",net_mac_address}, {"sceNetGetpeername",net_not_connected},
    {"sceNetResolverAbort",ok}, {"sceNetResolverGetError",net_resolver_error},
    {"sceNetResolverStartNtoaMultipleRecords",net_unreachable},
    /* ShareUtility */
    {"sceShareUtilityInitialize",ok}, {"sceShareUtilityTerminate",ok},
    {"sceShareUtilityOpenShareMenu",share_open}, {"sceShareUtilityOpenShareMenuDefault",share_open},
};
uintptr_t runtime_gr2_np_resolve(const char *name) {
    return RUNTIME_LOOKUP(exports,name);
}
