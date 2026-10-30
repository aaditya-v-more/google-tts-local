#include "quad.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Expected values were obtained from glibc on Linux arm64. */
int main(void) {
    static const struct {const char *text;unsigned rounding;uint64_t low,high;int range;} cases[]={
        {"0.1",0,UINT64_C(0x999999999999999a),UINT64_C(0x3ffb999999999999),0},
        {"0.1",2,UINT64_C(0x9999999999999999),UINT64_C(0x3ffb999999999999),0},
        {"-0.1",1,UINT64_C(0x9999999999999999),UINT64_C(0xbffb999999999999),0},
        {"1.25",0,0,UINT64_C(0x3fff400000000000),0},
        {"-42.75",0,0,UINT64_C(0xc004560000000000),0},
        {"1.0000000000000000000000000000000001",0,1,UINT64_C(0x3fff000000000000),0},
        {"3.14159265358979323846264338327950288",0,UINT64_C(0x8469898cc51701b8),UINT64_C(0x4000921fb54442d1),0},
        {"1e4000",0,UINT64_C(0x18c21ab905450cc3),UINT64_C(0x73e6a3750647fcab),0},
        {"1e-4000",0,UINT64_C(0x0b8049732d11a23d),UINT64_C(0x0c17387ae70c9e70),0},
        {"0x1p-16494",0,1,0,0},{"0x1p-16382",0,0,UINT64_C(0x0001000000000000),0},
        {"0x1.ffffffffffffffffffffffffffffp+16383",0,UINT64_MAX,UINT64_C(0x7ffeffffffffffff),0},
        {"1e5000",0,0,UINT64_C(0x7fff000000000000),1},
        {"1e5000",2,UINT64_MAX,UINT64_C(0x7ffeffffffffffff),1},
        {"-1e5000",1,UINT64_MAX,UINT64_C(0xfffeffffffffffff),1},
        {"1e-5000",1,1,0,1},{"-1e-5000",2,1,UINT64_C(0x8000000000000000),1},
        {"0x1p-16495",0,0,0,1},{"0x1p-16495",1,1,0,1},
        {"-0",0,0,UINT64_C(0x8000000000000000),0},
        {"nan(123)",0,123,UINT64_C(0x7fff800000000000),0},
        {"-nan(0xfffffffffffffffff)",0,UINT64_MAX,UINT64_C(0xffff800000000000),1}
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){uint64_t result[2]={0};int range=0;
        const char *text=cases[i].text;
        if(guest_quad_parse(text,text+strlen(text),".",cases[i].rounding,result,&range) ||
           result[0]!=cases[i].low || result[1]!=cases[i].high || range!=cases[i].range){
            fprintf(stderr,"binary128 check failed: %s (rounding %u)\n",text,cases[i].rounding);return 1;}}
    puts("Linux binary128 values, rounding, and range handling: passed.");return 0;
}
