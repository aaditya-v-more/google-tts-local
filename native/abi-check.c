#include "guest.h"
#include "speech.h"

#include <errno.h>
#include <locale.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

extern char abi_thread_start[],abi_thread_end[],abi_return_start[],abi_return_end[];
extern char abi_callback_start[],abi_callback_end[],abi_destructor_start[],abi_destructor_end[];
extern char abi_compare_start[],abi_compare_end[];
extern char abi_destructor_order_start[],abi_destructor_order_end[];

#define CHECK(c) do {if(!(c)){fprintf(stderr,"ABI check failed at %s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}} while(0)

static uint64_t call(Guest *g,const char *name,const uint64_t args[8]) {
    uint64_t address=guest_import(g,name),result=0;CHECK(address);
    if(guest_call(g,address,args,&result)){fprintf(stderr,"Hostcall failed: %s\n",name);exit(1);}return result;
}
#define CALL(name, ...) call(&g,(name),(uint64_t[8]){__VA_ARGS__})
static void *memory(Guest *g,size_t n) {void *p=guest_heap_alloc(g,n);CHECK(p);memset(p,0,n);return p;}
static char *guest_string(Guest *g,const char *value) {size_t n=strlen(value)+1;char *p=memory(g,n);memcpy(p,value,n);return p;}
static void *code(Guest *g,const char *start,const char *end) {if(g->native_mode)return (void *)start;size_t n=(size_t)(end-start);void *p=guest_alloc(g,n,16);CHECK(p);memcpy(p,start,n);__builtin___clear_cache(p,(char *)p+n);return p;}
static int in_guest(Guest *g,uint64_t p) {return p>=(uintptr_t)g->arena && p-(uintptr_t)g->arena<g->arena_size;}

int main(void) {
    if(!getenv("GTTS_DATA")){const char *home=getenv("HOME");char path[PATH_MAX];
        CHECK(home && snprintf(path,sizeof path,"%s/Library/Application Support/google-tts-local",home)<(int)sizeof path);
        CHECK(!setenv("GTTS_DATA",path,1));}
    Guest g;CHECK(!guest_create(&g));CHECK(!guest_load_objects(&g));
    char split[1800];memset(split,'x',sizeof split);memcpy(split+450,"\xe3\x80\x82",3);
    CHECK(speech_chunk_end(split,0,sizeof split)==453);
    memcpy(split+450,"\xe0\xa5\xa4",3);CHECK(speech_chunk_end(split,0,sizeof split)==453);
    for(size_t i=0;i+3<=sizeof split;i+=3)memcpy(split+i,"\xe3\x81\x82",3);
    CHECK(speech_chunk_end(split,0,sizeof split)%3==0);
    CHECK(in_guest(&g,*(uint64_t *)(uintptr_t)g.environ_slot));
    CHECK(in_guest(&g,((uint64_t *)(uintptr_t)g.tzname_slot)[0]));
    CHECK(*(uint64_t *)(uintptr_t)g.argv!=0);
    if (g.native_mode) {
        struct sigaction before_action, after_action;
        CHECK(!sigaction(SIGUSR1,NULL,&before_action));
        uint8_t *action=memory(&g,152), *old_action=memory(&g,152);
        *(uint64_t *)action=UINT64_C(0x1234);action[8]=0x55;action[136]=4;
        CHECK(!CALL("sigaction",10,(uintptr_t)action,(uintptr_t)old_action));
        CHECK(!*(uint64_t *)old_action);
        memset(old_action,0,152);
        CHECK(!CALL("sigaction",10,0,(uintptr_t)old_action));
        CHECK(!memcmp(action,old_action,152));
        CHECK(!sigaction(SIGUSR1,NULL,&after_action) && before_action.sa_handler==after_action.sa_handler);
        uint64_t *mask=memory(&g,128), *oldmask=memory(&g,128);
        CHECK(!CALL("sigfillset",(uintptr_t)mask));
        CHECK(!CALL("pthread_sigmask",0,(uintptr_t)mask,(uintptr_t)oldmask));
        CHECK(!oldmask[0]);
        CHECK(!CALL("pthread_sigmask",2,0,(uintptr_t)oldmask) && oldmask[0]==UINT64_MAX);
    }

    char *envname=guest_string(&g,"GTTS_DATA");uint64_t env=CALL("getenv",(uintptr_t)envname);
    CHECK(in_guest(&g,env) && !strcmp((char *)(uintptr_t)env,getenv("GTTS_DATA")));
    size_t before=g.arena_used;for(int i=0;i<500;++i)CHECK(CALL("getenv",(uintptr_t)envname)==env);CHECK(g.arena_used==before);

    uint64_t *lc=(uint64_t *)(uintptr_t)CALL("localeconv",0);
    CHECK(in_guest(&g,(uintptr_t)lc));for(int i=0;i<10;++i)CHECK(in_guest(&g,lc[i]));
    for(int i=0;i<10;++i)CALL("localeconv",0);before=g.arena_used;
    for(int i=0;i<100;++i)CALL("localeconv",0);CHECK(g.arena_used==before);
    char *locale=guest_string(&g,"C.UTF-8");
    uint64_t loc=CALL("newlocale",0x1fbf,(uintptr_t)locale,0);CHECK(loc);
    unsigned short *classes=*(unsigned short **)(uintptr_t)(loc+0x68);
    int *lower=*(int **)(uintptr_t)(loc+0x70);
    CHECK((classes['A']&0x0100) && classes[0xc0]==0 && lower[0xc0]==0xc0);
    CHECK(in_guest(&g,*(uint64_t *)(uintptr_t)(loc+0xe0)));
    CHECK(CALL("tolower_l",'A',UINT64_MAX)=='a');CALL("freelocale",loc);
    loc=CALL("newlocale",0x1fbf,(uintptr_t)locale,0);char *decimal=guest_string(&g,"0.1");char **decimal_end=memory(&g,sizeof *decimal_end);
    CHECK(loc && !guest_set_fpcr(&g,0));
    CALL("strtold_l",(uintptr_t)decimal,(uintptr_t)decimal_end,loc);hv_simd_fp_uchar16_t quad;
    CHECK(!guest_get_simd(&g,0,&quad));
    uint64_t quad_bits[2];memcpy(quad_bits,&quad,sizeof quad_bits);
    CHECK(*decimal_end==decimal+3 && quad_bits[0]==UINT64_C(0x999999999999999a) && quad_bits[1]==UINT64_C(0x3ffb999999999999));CALL("freelocale",loc);
    char *german=guest_string(&g,"de_DE.UTF-8");loc=CALL("newlocale",0x1fbf,(uintptr_t)german,0);
    if(loc){char *comma=guest_string(&g,"0,1");CALL("strtold_l",(uintptr_t)comma,(uintptr_t)decimal_end,loc);
        CHECK(!guest_get_simd(&g,0,&quad));memcpy(quad_bits,&quad,sizeof quad_bits);
        CHECK(*decimal_end==comma+3 && quad_bits[0]==UINT64_C(0x999999999999999a) && quad_bits[1]==UINT64_C(0x3ffb999999999999));
        CALL("freelocale",loc);guest_heap_free(&g,comma);}
    for(int i=0;i<10;++i){loc=CALL("newlocale",0x1fbf,(uintptr_t)locale,0);CHECK(loc);CALL("freelocale",loc);}
    before=g.arena_used;for(int i=0;i<100;++i){loc=CALL("newlocale",0x1fbf,(uintptr_t)locale,0);CHECK(loc);CALL("freelocale",loc);}
    CHECK(g.arena_used==before);
    CALL("strerror",110);before=g.arena_used;for(int i=0;i<100;++i)CALL("strerror",110);CHECK(g.arena_used==before);
    CALL("setlocale",6,(uintptr_t)locale);before=g.arena_used;for(int i=0;i<100;++i)CALL("setlocale",6,(uintptr_t)locale);CHECK(g.arena_used==before);

    char *bin=guest_string(&g," -0b101rest");char **end=memory(&g,sizeof *end);
    CHECK((int64_t)CALL("__isoc23_strtol",(uintptr_t)bin,(uintptr_t)end,0)==-5 && !strcmp(*end,"rest"));

    int64_t *tv=memory(&g,16);tv[0]=tv[1]=-1;CHECK(!CALL("gettimeofday",(uintptr_t)tv,0));CHECK(tv[1]>=0 && tv[1]<1000000);
    int *policy=memory(&g,4);unsigned char *sched=memory(&g,8);memset(sched,0xa5,8);
    CHECK(!CALL("pthread_getschedparam",(uintptr_t)pthread_self(),(uintptr_t)policy,(uintptr_t)sched));
    CHECK(sched[4]==0xa5 && sched[5]==0xa5 && sched[6]==0xa5 && sched[7]==0xa5);

    uint64_t *mutex=memory(&g,64),*cond=memory(&g,64);struct timespec *past=memory(&g,sizeof *past);
    clock_gettime(CLOCK_REALTIME,past);--past->tv_sec;
    CHECK(!CALL("pthread_mutex_init",(uintptr_t)mutex,0));CHECK(!CALL("pthread_cond_init",(uintptr_t)cond,0));
    CHECK(!CALL("pthread_mutex_lock",(uintptr_t)mutex));CHECK(CALL("pthread_cond_timedwait",(uintptr_t)cond,(uintptr_t)mutex,(uintptr_t)past)==110);
    CHECK(!CALL("pthread_mutex_unlock",(uintptr_t)mutex));CHECK(!CALL("pthread_cond_destroy",(uintptr_t)cond));CHECK(!CALL("pthread_mutex_destroy",(uintptr_t)mutex));

    uint64_t *sem=memory(&g,32);CHECK(!CALL("sem_init",(uintptr_t)sem,0,0));
    CHECK(CALL("sem_trywait",(uintptr_t)sem)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==11);
    CHECK(!CALL("sem_post",(uintptr_t)sem));CHECK(!CALL("sem_wait",(uintptr_t)sem));
    CHECK(CALL("sem_timedwait",(uintptr_t)sem,(uintptr_t)past)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==110);
    CHECK(!CALL("sem_destroy",(uintptr_t)sem));
    uint64_t *rw=memory(&g,64);CHECK(!CALL("pthread_rwlock_rdlock",(uintptr_t)rw));
    CHECK(pthread_rwlock_trywrlock((pthread_rwlock_t *)(uintptr_t)*rw)==EBUSY);
    CHECK(!CALL("pthread_rwlock_unlock",(uintptr_t)rw));CHECK(!CALL("pthread_rwlock_wrlock",(uintptr_t)rw));
    CHECK(!CALL("pthread_rwlock_unlock",(uintptr_t)rw));CHECK(!CALL("pthread_rwlock_destroy",(uintptr_t)rw));

    CHECK(setlocale(LC_ALL,"C.UTF-8"));unsigned char *state=memory(&g,16);memset(state+8,0xa5,8);
    char *utf=memory(&g,4);memcpy(utf,"\xe3\x81\x82",3);wchar_t *wc=memory(&g,sizeof *wc);
    CHECK(CALL("mbrtowc",(uintptr_t)wc,(uintptr_t)utf,2,(uintptr_t)state)==UINT64_MAX-1);
    for(int i=8;i<16;++i)CHECK(state[i]==0xa5);
    CHECK(CALL("mbrtowc",(uintptr_t)wc,(uintptr_t)(utf+2),1,(uintptr_t)state)==1 && *wc==0x3042);

    uint64_t *map=memory(&g,12*sizeof(uint64_t));ElfObject *obj=&g.objects[0];
    CHECK(!CALL("_dl_find_object",(uintptr_t)obj->base,(uintptr_t)map));
    CHECK(map[1]==(uintptr_t)obj->base && map[4]==obj->eh_frame && in_guest(&g,map[3]));
    void *callback=code(&g,abi_callback_start,abi_callback_end);uint64_t *seen=memory(&g,8);
    CHECK(CALL("dl_iterate_phdr",(uintptr_t)callback,(uintptr_t)seen)==1 && *seen==(uintptr_t)obj->base);
    int *items=memory(&g,5*sizeof *items);items[0]=4;items[1]=-1;items[2]=3;items[3]=0;items[4]=2;
    void *compare=code(&g,abi_compare_start,abi_compare_end);
    CALL("qsort",(uintptr_t)items,5,sizeof *items,(uintptr_t)compare);
    CHECK(items[0]==-1 && items[1]==0 && items[2]==2 && items[3]==3 && items[4]==4);

    char *tmpdir=guest_string(&g,"native/tmp");uint64_t cwd=CALL("getcwd",0,0);CHECK(in_guest(&g,cwd));CALL("free",cwd);
    uint64_t real=CALL("realpath",(uintptr_t)tmpdir,0);CHECK(in_guest(&g,real));CALL("free",real);
    uint64_t fd=CALL("open",(uintptr_t)tmpdir,0x4000,0);CHECK((int64_t)fd>=0);CALL("close",fd);
    CHECK(CALL("open",(uintptr_t)tmpdir,0x10000,0)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==95);
    unlink("native/tmp/abi-symlink");CHECK(!symlink(".","native/tmp/abi-symlink"));
    char *link=guest_string(&g,"native/tmp/abi-symlink");
    CHECK(CALL("open",(uintptr_t)link,0x8000,0)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==40);unlink("native/tmp/abi-symlink");

    uint64_t first=0;for(int i=0;i<150;++i){uint64_t p=CALL("mmap",0,100000,3,0x22,UINT64_MAX,0);CHECK(p!=UINT64_MAX);
        if(!i)first=p;else CHECK(p==first);CHECK(!CALL("munmap",p,4096));CHECK(!CALL("munmap",p+4096,100000-4096));}
    CHECK(CALL("mmap",first,4096,3,0x32,UINT64_MAX,0)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==95);
    if(g.native_mode) {
        CHECK(CALL("mmap",0,4096,7,0x22,UINT64_MAX,0)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==95);
        CHECK(CALL("mprotect",first,4096,5)==UINT64_MAX && *(int *)(uintptr_t)g.errno_slot==95);
    }

    char *fmt=guest_string(&g,"%s %.2f %d"),*word=guest_string(&g,"value"),*output=memory(&g,128);
    double number=1.25;hv_simd_fp_uchar16_t q={0};memcpy(&q,&number,sizeof number);CHECK(!guest_set_simd(&g,0,q));
    CHECK(CALL("snprintf",(uintptr_t)output,128,(uintptr_t)fmt,(uintptr_t)word,7)==12 && !strcmp(output,"value 1.25 7"));
    FILE *format_file=fopen("native/tmp/abi-format.txt","w+");CHECK(format_file);
    char *print_fmt=guest_string(&g,"%d %.1f");number=2.5;memset(&q,0,sizeof q);memcpy(&q,&number,sizeof number);
    CHECK(!guest_set_simd(&g,0,q));
    CHECK(CALL("__fprintf_chk",(uintptr_t)format_file,0,(uintptr_t)print_fmt,4)==5);
    rewind(format_file);char printed[16]={0};CHECK(fread(printed,1,sizeof printed-1,format_file)==5 && !strcmp(printed,"4 2.5"));fclose(format_file);
    wchar_t *wide_fmt=memory(&g,64),*wide_out=memory(&g,256);wcscpy(wide_fmt,L"%d %.1f");
    number=2.5;memset(&q,0,sizeof q);memcpy(&q,&number,sizeof number);CHECK(!guest_set_simd(&g,0,q));
    CHECK(CALL("swprintf",(uintptr_t)wide_out,64,(uintptr_t)wide_fmt,4)==5 && !wcscmp(wide_out,L"4 2.5"));
    double *integral=memory(&g,sizeof *integral);number=3.25;memset(&q,0,sizeof q);memcpy(&q,&number,sizeof number);
    CHECK(!guest_set_simd(&g,0,q));CALL("modf",(uintptr_t)integral);
    CHECK(!guest_get_simd(&g,0,&q));double fraction;memcpy(&fraction,&q,sizeof fraction);
    CHECK(*integral==3 && fraction==0.25);
    char *scan=guest_string(&g,"73 sample"),*scan_fmt=guest_string(&g,"%d %s"),*scanned=memory(&g,32);int *num=memory(&g,sizeof *num);
    CHECK(CALL("__isoc99_sscanf",(uintptr_t)scan,(uintptr_t)scan_fmt,(uintptr_t)num,(uintptr_t)scanned)==2 && *num==73 && !strcmp(scanned,"sample"));
    char *binary=guest_string(&g,"value=0b101 17!"),*binary_fmt=guest_string(&g,"value=%i %d%n");
    int *other=memory(&g,sizeof *other),*consumed=memory(&g,sizeof *consumed);
    CHECK(CALL("__isoc23_sscanf",(uintptr_t)binary,(uintptr_t)binary_fmt,(uintptr_t)num,(uintptr_t)other,(uintptr_t)consumed)==2 &&
          *num==5 && *other==17 && *consumed==14);
    char *plain_binary=guest_string(&g,"101 0b11"),*plain_fmt=guest_string(&g,"%3b %i");
    typedef struct {uint64_t stack,gr_top,vr_top;int32_t gr_offs,vr_offs;} TestVa;
    uint64_t *saved=memory(&g,2*sizeof *saved);saved[0]=(uintptr_t)num;saved[1]=(uintptr_t)other;
    TestVa *va=memory(&g,sizeof *va);va->gr_top=(uintptr_t)(saved+2);va->gr_offs=-16;
    CHECK(CALL("__isoc23_vsscanf",(uintptr_t)plain_binary,(uintptr_t)plain_fmt,(uintptr_t)va)==2 && *num==5 && *other==3);
    char *short_binary=guest_string(&g,"0b2"),*short_fmt=guest_string(&g,"%i%n");*num=-1;*consumed=-1;
    CHECK(CALL("__isoc23_sscanf",(uintptr_t)short_binary,(uintptr_t)short_fmt,(uintptr_t)num,(uintptr_t)consumed)==1 && *num==0 && *consumed==2);

    uint64_t *thread=memory(&g,8),*thread_result=memory(&g,8);void *simple=code(&g,abi_return_start,abi_return_end);
    guest_import(&g,"pthread_create");guest_import(&g,"pthread_join");size_t saved_size=g.arena_size;g.arena_size=g.arena_used;
    CHECK(CALL("pthread_create",(uintptr_t)thread,0,(uintptr_t)simple,0)==11);g.arena_size=saved_size;
    CHECK(!CALL("pthread_create",(uintptr_t)thread,0,(uintptr_t)simple,0));CHECK(!CALL("pthread_join",*thread,(uintptr_t)thread_result));CHECK(*thread_result==42);
    before=g.arena_used;for(int i=0;i<140;++i){CHECK(!CALL("pthread_create",(uintptr_t)thread,0,(uintptr_t)simple,0));
        CHECK(!CALL("pthread_join",*thread,(uintptr_t)thread_result) && *thread_result==42);}CHECK(g.arena_used==before);
    void *thread_code=code(&g,abi_thread_start,abi_thread_end);uint64_t *thread_args=memory(&g,16);
    char *log=guest_string(&g,"speaker: \"thread\" gender: \"female\"");thread_args[1]=(uintptr_t)log;
    CHECK(!CALL("pthread_create",(uintptr_t)thread,0,(uintptr_t)thread_code,(uintptr_t)thread_args));
    uint64_t logger=guest_import(&g,"__guest_logger");CHECK(logger);__atomic_store_n(&thread_args[0],logger,__ATOMIC_RELEASE);
    CHECK(!CALL("pthread_join",*thread,(uintptr_t)thread_result));CHECK(g.speaker_count && !strcmp(g.speakers[0].name,"thread"));
    char *more=guest_string(&g,"speaker: \"thread\" gender: \"male\" speaker: \"nogender\" marker gender: \"female\"");
    CALL("__guest_logger",0,(uintptr_t)more);CHECK(g.speaker_count==1 && !strcmp(g.speakers[0].gender,"female"));
    CHECK(g.fallback_speaker_count==2 && !strcmp(g.fallback_speakers[1].name,"nogender"));

    void *destructor=code(&g,abi_destructor_start,abi_destructor_end);int *destroyed=memory(&g,sizeof *destroyed);
    CHECK(!CALL("__cxa_thread_atexit_impl",(uintptr_t)destructor,(uintptr_t)destroyed,0));guest_run_thread_destructors(&g);CHECK(*destroyed==42);
    *destroyed=0;
    CHECK(!CALL("__cxa_atexit",(uintptr_t)destructor,(uintptr_t)destroyed,0x1234));
    CALL("__cxa_finalize",0x5678);CHECK(!*destroyed);
    CALL("__cxa_finalize",0x1234);CHECK(*destroyed==42);
    *destroyed=0;CALL("__cxa_finalize",0x1234);CHECK(!*destroyed);
    void *ordered=code(&g,abi_destructor_order_start,abi_destructor_order_end);
    uint64_t *first_arg=memory(&g,16), *second_arg=memory(&g,16);
    first_arg[0]=(uintptr_t)destroyed;first_arg[1]=1;second_arg[0]=(uintptr_t)destroyed;second_arg[1]=2;
    CHECK(!CALL("__cxa_atexit",(uintptr_t)ordered,(uintptr_t)first_arg,0x99));
    CHECK(!CALL("__cxa_atexit",(uintptr_t)ordered,(uintptr_t)second_arg,0x99));
    CALL("__cxa_finalize",0x99);CHECK(*destroyed==0x21);

    uint64_t unsupported=guest_import(&g,"__unimplemented_abi_check"),ignored=0;
    CHECK(guest_call(&g,unsupported,NULL,&ignored)!=0 && g.broken);
    SpeechEngine engine={.guest=&g,.ready=1};float *audio=NULL;size_t count=0;
    CHECK(speech_synth(&engine,"hello","missing",NULL,1,1,&audio,&count)==SPEECH_ENGINE_BROKEN && engine.broken);
    speech_close(&engine);guest_destroy(&g);
    puts("Guest ABI, bounded allocations, thread lifecycle, and fault propagation: passed.");return 0;
}
