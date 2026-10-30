#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

int main(int argc,char **argv) {
    if(argc!=2 || setenv("GTTS_NATIVE_LIB",argv[1],1))return 1;
    Guest guest;if(guest_create(&guest) || guest_load_objects(&guest) || guest_initialize_objects(&guest))return 1;
    uint64_t function=guest_symbol(&guest,"caught_exception"),result=0;
    if(!function || guest_call(&guest,function,NULL,&result) || (int32_t)result!=42){
        fprintf(stderr,"Guest C++ exception was not caught (result=%lld)\n",(long long)result);return 1;
    }
    static const struct {const char *name,*text;int range;} cases[]={
        {"quad_tenth","0.1",0},{"quad_exact","1.25",0},{"quad_negative","-42.75",0},
        {"quad_precision","1.0000000000000000000000000000000001",0},
        {"quad_pi","3.14159265358979323846264338327950288",0},{"quad_huge","1e4000",0},
        {"quad_tiny","1e-4000",0},{"quad_minsub","0x1p-16494",0},{"quad_minnormal","0x1p-16382",0},
        {"quad_maxfinite","0x1.ffffffffffffffffffffffffffffp+16383",0},{"quad_overflow","1e5000",1},
        {"quad_underflow","1e-5000",1},{"quad_negativezero","-0",0}
    };
    uint64_t convert=guest_import(&guest,"strtold"),wide_convert=guest_import(&guest,"wcstold");
    if(!convert || !wide_convert || guest_set_fpcr(&guest,0))return 1;
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){
        hv_simd_fp_uchar16_t expected,actual;function=guest_symbol(&guest,cases[i].name);
        if(!function || guest_call(&guest,function,NULL,&result) || guest_get_simd(&guest,0,&expected))return 1;
        size_t n=strlen(cases[i].text);char *text=guest_heap_alloc(&guest,n+2);wchar_t *wide=guest_heap_alloc(&guest,(n+2)*sizeof *wide);
        uint64_t *end=guest_heap_alloc(&guest,sizeof *end);if(!text || !wide || !end)return 1;
        memcpy(text,cases[i].text,n);text[n]='x';text[n+1]=0;for(size_t j=0;j<n+2;++j)wide[j]=(unsigned char)text[j];
        uint64_t args[8]={(uintptr_t)text,(uintptr_t)end};*(int *)(uintptr_t)guest.errno_slot=22;
        if(guest_call(&guest,convert,args,&result) || guest_get_simd(&guest,0,&actual) ||
           memcmp(&expected,&actual,sizeof actual) || *end!=(uintptr_t)(text+n) || *(int *)(uintptr_t)guest.errno_slot!=(cases[i].range?34:22)){
            fprintf(stderr,"binary128 conversion failed: %s\n",cases[i].text);return 1;}
        args[0]=(uintptr_t)wide;*(int *)(uintptr_t)guest.errno_slot=22;
        if(guest_call(&guest,wide_convert,args,&result) || guest_get_simd(&guest,0,&actual) ||
           memcmp(&expected,&actual,sizeof actual) || *end!=(uintptr_t)(wide+n) || *(int *)(uintptr_t)guest.errno_slot!=(cases[i].range?34:22)){
            fprintf(stderr,"wide binary128 conversion failed: %s\n",cases[i].text);return 1;}
        guest_heap_free(&guest,text);guest_heap_free(&guest,wide);guest_heap_free(&guest,end);
    }
    guest_destroy(&guest);puts("Guest constructors and caught C++ exception: passed.");return 0;
}
