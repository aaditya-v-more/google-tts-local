#include "quad.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BIG_WORDS 2048
typedef struct {uint32_t word[BIG_WORDS];size_t count;} Big;
typedef unsigned __int128 U128;

static void trim(Big *n) {while(n->count && !n->word[n->count-1])--n->count;}
static int mul_small(Big *n,uint32_t factor) {
    uint64_t carry=0;
    for(size_t i=0;i<n->count;++i){uint64_t v=(uint64_t)n->word[i]*factor+carry;n->word[i]=(uint32_t)v;carry=v>>32;}
    if(carry){if(n->count==BIG_WORDS)return -1;n->word[n->count++]=(uint32_t)carry;}return 0;
}
static int add_small(Big *n,uint32_t add) {
    uint64_t carry=add;size_t i=0;
    while(carry && i<n->count){uint64_t v=(uint64_t)n->word[i]+carry;n->word[i++]=(uint32_t)v;carry=v>>32;}
    if(carry){if(n->count==BIG_WORDS)return -1;n->word[n->count++]=(uint32_t)carry;}return 0;
}
static int bits(const Big *n) {return n->count?(int)((n->count-1)*32+32-__builtin_clz(n->word[n->count-1])):0;}
static int compare(const Big *a,const Big *b) {
    if(a->count!=b->count)return a->count>b->count?1:-1;
    for(size_t i=a->count;i>0;--i)if(a->word[i-1]!=b->word[i-1])return a->word[i-1]>b->word[i-1]?1:-1;
    return 0;
}
static int shift(Big *out,const Big *in,unsigned amount) {
    size_t words=amount/32;unsigned rem=amount%32;
    if(in->count && in->count+words+(rem?1:0)>BIG_WORDS)return -1;
    memset(out,0,sizeof *out);uint64_t carry=0;
    for(size_t i=0;i<in->count;++i){uint64_t v=((uint64_t)in->word[i]<<rem)|carry;out->word[words+i]=(uint32_t)v;carry=v>>32;}
    out->count=in->count+words;if(carry)out->word[out->count++]=(uint32_t)carry;trim(out);return 0;
}
static void subtract(Big *a,const Big *b) {
    uint64_t borrow=0;
    for(size_t i=0;i<a->count;++i){uint64_t av=a->word[i],bv=(i<b->count?b->word[i]:0)+borrow;
        a->word[i]=(uint32_t)(av-bv);borrow=av<bv;}
    trim(a);
}
static int is_outward(unsigned rounding,int negative) {return (rounding==1 && !negative) || (rounding==2 && negative);}
static void store(uint64_t result[2],int negative,unsigned exponent,U128 fraction) {
    result[0]=(uint64_t)fraction;result[1]=(uint64_t)(fraction>>64)|((uint64_t)exponent<<48)|((uint64_t)negative<<63);
}
static void extreme(uint64_t result[2],int negative,unsigned rounding,int overflow) {
    if(overflow) {
        if(rounding==0 || is_outward(rounding,negative))store(result,negative,0x7fff,0);
        else store(result,negative,0x7ffe,((U128)1<<112)-1);
    } else store(result,negative,0,is_outward(rounding,negative)?1:0);
}
static int digit_value(unsigned char c,int base) {
    if(c>='0' && c<='9')return c-'0';
    if(base==16 && c>='a' && c<='f')return c-'a'+10;
    if(base==16 && c>='A' && c<='F')return c-'A'+10;
    return -1;
}

int guest_quad_parse(const char *start,const char *end,const char *decimal,unsigned rounding,uint64_t result[2],int *range_error) {
    *range_error=0;result[0]=result[1]=0;
    if(start==end)return 0;
    const char *p=start;while(p<end && isspace((unsigned char)*p))++p;
    int negative=0;if(p<end && (*p=='+' || *p=='-')){negative=*p=='-';++p;}
    if(end-p>=3 && !strncasecmp(p,"inf",3)){store(result,negative,0x7fff,0);return 0;}
    if(end-p>=3 && !strncasecmp(p,"nan",3)) {
        uint64_t payload=0;
        if(end-p>3){if(p[3]!='(' || end[-1]!=')')return -1;
            if(end-p>5){char *tail=NULL;errno=0;payload=strtoull(p+4,&tail,0);
                if(tail!=end-1)payload=0;else if(errno==ERANGE)*range_error=1;}}
        store(result,negative,0x7fff,((U128)1<<111)|payload);return 0;
    }
    int hex=end-p>=2 && p[0]=='0' && (p[1]=='x' || p[1]=='X');if(hex)p+=2;
    int base=hex?16:10,digits=0,fractional=0,seen=0,point=0;
    size_t decimal_len=decimal?strlen(decimal):0;Big n={0},denom={0},work={0},temporary={0};
    while(p<end) {
        int d=digit_value((unsigned char)*p,base);
        if(d>=0 && d<base) {
            if(d || seen){seen=1;if(++digits>5000)return -1;}
            if(point)++fractional;
            if(mul_small(&n,(uint32_t)base) || add_small(&n,(uint32_t)d))return -1;
            ++p;continue;
        }
        if(!point && decimal_len && (size_t)(end-p)>=decimal_len && !memcmp(p,decimal,decimal_len)){
            point=1;p+=decimal_len;continue;
        }
        break;
    }
    int exponent=0,exponent_negative=0;
    if(p<end && ((!hex && (*p=='e' || *p=='E')) || (hex && (*p=='p' || *p=='P')))) {
        ++p;if(p<end && (*p=='+' || *p=='-')){exponent_negative=*p=='-';++p;}
        while(p<end && *p>='0' && *p<='9'){if(exponent<100000)exponent=exponent*10+(*p-'0');++p;}
    }
    if(p!=end)return -1;if(exponent_negative)exponent=-exponent;
    if(!n.count){store(result,negative,0,0);return 0;}
    int scale=hex?exponent-4*fractional:exponent-fractional;
    if(!hex){int order=digits+scale-1;
        if(order>4932){extreme(result,negative,rounding,1);*range_error=1;return 0;}
        if(order< -4967){extreme(result,negative,rounding,0);*range_error=1;return 0;}}
    else {int order=bits(&n)+scale-1;
        if(order>16383){extreme(result,negative,rounding,1);*range_error=1;return 0;}
        if(order< -16495){extreme(result,negative,rounding,0);*range_error=1;return 0;}}
    // Decimal N * 10^scale becomes N * 5^scale * 2^scale (or a
    // denominator of 5^-scale). Integer division then supplies the exact
    // binary128 significand and remainder for the requested rounding mode.
    denom.word[0]=1;denom.count=1;int binary_scale=scale;
    if(!hex) {
        int power=scale<0?-scale:scale;Big *target=scale<0?&denom:&n;
        for(int i=0;i<power;++i)if(mul_small(target,5))return -1;
    }
    int diff=bits(&n)-bits(&denom),cmp;
    if(diff>=0){if(shift(&temporary,&denom,(unsigned)diff))return -1;cmp=compare(&n,&temporary);}
    else {if(shift(&temporary,&n,(unsigned)-diff))return -1;cmp=compare(&temporary,&denom);}
    int k=diff-(cmp<0)+binary_scale;
    if(k>16383){extreme(result,negative,rounding,1);*range_error=1;return 0;}
    int subnormal=k< -16382;
    int adjustment=binary_scale+(subnormal?16494:112-k);
    if(adjustment>=0){if(shift(&work,&n,(unsigned)adjustment))return -1;}
    else {work=n;if(shift(&temporary,&denom,(unsigned)-adjustment))return -1;denom=temporary;}
    int highest=bits(&work)-bits(&denom);if(highest>113)return -1;U128 quotient=0;
    for(int i=highest;i>=0;--i){if(shift(&temporary,&denom,(unsigned)i))return -1;
        if(compare(&work,&temporary)>=0){subtract(&work,&temporary);quotient|=(U128)1<<i;}}
    int inexact=work.count!=0,increment=0;
    if(inexact && rounding==0){if(shift(&temporary,&work,1))return -1;int relation=compare(&temporary,&denom);
        increment=relation>0 || (relation==0 && (quotient&1));}
    else if(inexact)increment=is_outward(rounding,negative);
    quotient+=(unsigned)increment;
    if(subnormal) {
        if(inexact)*range_error=1;
        if(quotient>=((U128)1<<112))store(result,negative,1,0);
        else store(result,negative,0,quotient);
    } else {
        if(quotient>=((U128)1<<113)){quotient>>=1;++k;}
        if(k>16383){extreme(result,negative,rounding,1);*range_error=1;}
        else store(result,negative,(unsigned)(k+16383),quotient&(((U128)1<<112)-1));
    }
    return 0;
}
