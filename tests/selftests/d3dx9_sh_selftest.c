#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "win32hle.h"
#include "host.h"
#include "d3dx9_sh.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { {0,0} };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *w,int c){ if(!c){ printf("FAIL %s\n",w); fails++; } }
static uint32_t call(const char *name,int n,const uint32_t *a){ return hle_call_guest(hle_resolve(name),n,a); }
#define A(p) hle_va(p)
static void *G(const void *src,size_t n){ void *p=hle_alloc(n); if(src) memcpy(p,src,n); else memset(p,0,n); return p; }
static uint32_t fb(float f){ uint32_t u; memcpy(&u,&f,4); return u; }
static float ff(uint32_t u){ float f; memcpy(&f,&u,4); return f; }
static int feq(float a,float b){ return fabsf(a-b)<1e-3f; }
static int feq2(float a,float b){ return fabsf(a-b)<1e-5f; }

int main(void){
    recomp_host_init();

    /* EvalDirection order 2, dir=(0,0,1): [c0, 0, c1, 0] */
    float dir[3]={0,0,1};
    float out[36]={0};
    void *gdir=G(dir,12), *gout=G(out,36*4);
    uint32_t r = call("D3DXSHEvalDirection",3,(uint32_t[]){A(gout),2,A(gdir)});
    ok("SHEvalDirection returns pOut", r==A(gout));
    ok("SHEvalDirection c0", feq2(((float*)gout)[0],0.28209479f));
    ok("SHEvalDirection c1", feq2(((float*)gout)[1],0.0f));
    ok("SHEvalDirection c2", feq2(((float*)gout)[2],0.48860251f));
    ok("SHEvalDirection c3", feq2(((float*)gout)[3],0.0f));

    /* Direct C API tests for order 1..6 and edge cases */
    {
        d3dx9_vec3 d = {0,0,1};
        float o1[1];
        ok("sh_eval_dir order1 rejected", d3dx9_sh_eval_direction(o1,1,&d) == NULL);
    }
    {
        d3dx9_vec3 d = {1,0,0};
        float o3[9];
        ok("sh_eval_dir order3 dir=(1,0,0)", d3dx9_sh_eval_direction(o3,3,&d) != NULL);
        /* l=0: 0.28209479; l=1 m=-1:0, m=0:0, m=1: -0.4886025119*1 = -0.4886025; l=2 gives some values */
        ok("sh_eval_dir o3[0]", feq2(o3[0],0.28209479f));
        ok("sh_eval_dir o3[3] (l1 m=1)", feq2(o3[3], -0.48860251f));
    }
    {
        d3dx9_vec3 dzero = {0,0,0};
        float oz[4];
        ok("sh_eval_dir zero vec returns NULL", d3dx9_sh_eval_direction(oz,2,&dzero) == NULL);
    }
    {
        float oz2[4];
        ok("sh_eval_dir null out returns NULL", d3dx9_sh_eval_direction(NULL,2,&(d3dx9_vec3){0,0,1}) == NULL);
    }

    /* Add/Scale/Dot order 2 (via call) */
    float a[4]={1,2,3,4}, b[4]={5,6,7,8}, o[4];
    void *ga=G(a,16), *gb=G(b,16), *go=G(o,16);
    call("D3DXSHAdd",4,(uint32_t[]){A(go),2,A(ga),A(gb)});
    ok("SHAdd", feq(((float*)go)[0],6)&&feq(((float*)go)[1],8)&&feq(((float*)go)[2],10)&&feq(((float*)go)[3],12));
    call("D3DXSHScale",4,(uint32_t[]){A(go),2,A(ga),fb(2.0f)});
    ok("SHScale", feq(((float*)go)[0],2)&&feq(((float*)go)[3],8));
    r = call("D3DXSHDot",3,(uint32_t[]){2,A(ga),A(gb)});
    ok("SHDot", feq(ff(r),70.0f));

    /* RotateZ angle 0 identity */
    call("D3DXSHRotateZ",4,(uint32_t[]){A(go),2,fb(0.0f),A(ga)});
    ok("SHRotateZ identity", feq(((float*)go)[0],1)&&feq(((float*)go)[1],2)&&feq(((float*)go)[2],3)&&feq(((float*)go)[3],4));
    float rz_in[4]={1,1,0,0}; void *grz=G(rz_in,16);
    call("D3DXSHRotateZ",4,(uint32_t[]){A(go),2,fb((float)(M_PI/2.0)),A(grz)});
    ok("SHRotateZ pi/2", feq(((float*)go)[0],1)&&feq(((float*)go)[1],0)&&feq(((float*)go)[2],0)&&feq(((float*)go)[3],-1));
    r = call("D3DXSHAdd",4,(uint32_t[]){A(go),0,A(ga),A(gb)});
    ok("SHAdd order 0 returns NULL", r==0);

    /* Direct tests for directional light */
    {
        d3dx9_vec3 d = {0,0,1};
        float rOut[4], gOut[4], bOut[4];
        ok("EvalDirLight ok", d3dx9_sh_eval_directional_light(rOut,gOut,bOut,2,&d,1.0f,0.5f,0.25f) != NULL);
        ok("EvalDirLight r[0]", feq2(rOut[0],0.28209479f));
        ok("EvalDirLight g[0]", feq2(gOut[0],0.28209479f*0.5f));
    }

    uint32_t esp0=g_esp;
    call("D3DXSHEvalDirection",3,(uint32_t[]){A(gout),2,A(gdir)});
    ok("SHEvalDirection esp", g_esp==esp0);
    esp0=g_esp; call("D3DXSHAdd",4,(uint32_t[]){A(go),2,A(ga),A(gb)}); ok("SHAdd esp", g_esp==esp0);
    esp0=g_esp; call("D3DXSHDot",3,(uint32_t[]){2,A(ga),A(gb)}); ok("SHDot esp", g_esp==esp0);

    printf(fails==0?"d3dx9_sh_selftest: all checks passed\n":"d3dx9_sh_selftest: FAILED (%d)\n",fails);
    return fails!=0;
}
