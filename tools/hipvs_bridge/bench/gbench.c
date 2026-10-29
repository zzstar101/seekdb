/* gbench: graph-build component timing for 250K-scale CAGRA.
 * usage: gbench <mode> base.f32 n dim query.f32 nq gt.i32(nq x 100) [k=v ...]
 * modes: cagra | nnd | bf | an
 * keys: algo(0..3) deg inter niter itopk width k batch nclus overlap */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/nn_descent.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/all_neighbors.h>
#include <dlpack/dlpack.h>
#include <cuvs/cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../sk_cagra_graph.h"

static double now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static long kv(int argc,char**argv,const char*k,long d){size_t l=strlen(k);for(int i=8;i<argc;i++) if(!strncmp(argv[i],k,l)&&argv[i][l]=='=') return atol(argv[i]+l+1);return d;}
static void mk(DLManagedTensor*t,void*p,int dev,int code,int bits,long*sh){memset(t,0,sizeof(*t));t->dl_tensor.data=p;t->dl_tensor.device.device_type=dev?kDLCUDA:kDLCPU;t->dl_tensor.ndim=2;t->dl_tensor.dtype.code=code;t->dl_tensor.dtype.bits=bits;t->dl_tensor.dtype.lanes=1;t->dl_tensor.shape=sh;}
#define CK(x) do{ if((x)!=CUVS_SUCCESS){printf("FAIL %s: %s\n",#x,cuvsGetLastErrorText());exit(1);} }while(0)

static float* rd(const char*p,long cnt){float*b=malloc(sizeof(float)*cnt);FILE*f=fopen(p,"rb");if(!f||fread(b,4,cnt,f)!=(size_t)cnt){printf("read %s fail\n",p);exit(1);}fclose(f);return b;}

static void eval_cagra(cuvsResources_t res,cuvsCagraIndex_t idx,const float*qs,long nq,long dim,const int*gt,int argc,char**argv){
  long topks[2]={10,100};
  float*qd; unsigned*nd; float*dd;
  CK(cuvsRMMAlloc(res,(void**)&qd,4*nq*dim)); CK(cuvsRMMAlloc(res,(void**)&nd,4*nq*100)); CK(cuvsRMMAlloc(res,(void**)&dd,4*nq*100));
  cudaMemcpy(qd,qs,4*nq*dim,cudaMemcpyDefault);
  unsigned*ids=malloc(4*nq*100);
  for(int t=0;t<2;t++){
    long topk=topks[t];
    cuvsCagraSearchParams_t sp; cuvsCagraSearchParamsCreate(&sp);
    long it=kv(argc,argv,"itopk",0); if(!it){it=topk*4; if(it<512) it=512;}
    sp->itopk_size=it; sp->search_width=kv(argc,argv,"width",4); sp->algo=kv(argc,argv,"salgo",3);
    double t1=now_ms(); double t1q=0;
    for(long i=0;i<nq;i+=256){
      long m=nq-i<256?nq-i:256; long qsh[2]={m,dim},nsh[2]={m,topk};
      DLManagedTensor qt,nt,dt; mk(&qt,qd+i*dim,1,kDLFloat,32,qsh); mk(&nt,nd+i*topk,1,kDLUInt,32,nsh); mk(&dt,dd+i*topk,1,kDLFloat,32,nsh);
      cuvsFilter fl={0,NO_FILTER};
      CK(cuvsCagraSearch(res,sp,idx,&qt,&nt,&dt,fl));
    }
    cudaDeviceSynchronize(); double bt=now_ms()-t1;
    cudaMemcpy(ids,nd,4*nq*topk,cudaMemcpyDefault);
    /* serial nq=1 latency on 512 queries */
    t1=now_ms();
    for(long i=0;i<512;i++){
      long qsh[2]={1,dim},nsh[2]={1,topk};
      DLManagedTensor qt,nt,dt; mk(&qt,qd+i*dim,1,kDLFloat,32,qsh); mk(&nt,nd,1,kDLUInt,32,nsh); mk(&dt,dd,1,kDLFloat,32,nsh);
      cuvsFilter fl={0,NO_FILTER};
      CK(cuvsCagraSearch(res,sp,idx,&qt,&nt,&dt,fl)); cudaDeviceSynchronize();
    }
    t1q=(now_ms()-t1)/512;
    long hit=0;
    for(long q=0;q<nq;q++){ for(long a=0;a<topk;a++){ unsigned g=gt[q*100+a]; for(long b=0;b<topk;b++) if(ids[q*topk+b]==g){hit++;break;} } }
    printf("  search top%ld itopk=%ld recall=%.4f batch_ms_per_q=%.4f nq1_ms=%.3f\n",topk,it,(double)hit/(nq*topk),bt/nq,t1q);
    cuvsCagraSearchParamsDestroy(sp);
  }
  free(ids);
}

/* recall of a knn graph (n x k, int64 or uint32) for sample rows vs exact bf */
int main(int argc,char**argv){
  if(argc<8){printf("usage\n");return 1;}
  const char*mode=argv[1]; long n=atol(argv[3]),dim=atol(argv[4]); long nq=atol(argv[6]);
  float*base=rd(argv[2],n*dim); float*qs=rd(argv[5],nq*dim);
  int*gt=(int*)rd(argv[7],nq*100);
  cuvsResources_t res; CK(cuvsResourcesCreate(&res));
  float*dbase; CK(cuvsRMMAlloc(res,(void**)&dbase,4*n*dim)); cudaMemcpy(dbase,base,4*n*dim,cudaMemcpyDefault); cudaDeviceSynchronize();
  long dsh[2]={n,dim}; DLManagedTensor hds,dds; mk(&hds,base,0,kDLFloat,32,dsh); mk(&dds,dbase,1,kDLFloat,32,dsh);
  int ondev=kv(argc,argv,"dev",0);
  if(!strcmp(mode,"cagra")){
    cuvsCagraIndexParams_t ip; CK(cuvsCagraIndexParamsCreate(&ip));
    ip->build_algo=kv(argc,argv,"algo",2); ip->graph_degree=kv(argc,argv,"deg",128); ip->intermediate_graph_degree=kv(argc,argv,"inter",256);
    ip->nn_descent_niter=kv(argc,argv,"niter",20);
    cuvsCagraIndex_t idx; CK(cuvsCagraIndexCreate(&idx));
    double t0=now_ms(); CK(cuvsCagraBuild(res,ip,ondev?&dds:&hds,idx)); cudaDeviceSynchronize();
    printf("cagra algo=%ld deg=%ld inter=%ld niter=%ld dev=%d build_ms=%.1f\n",(long)ip->build_algo,(long)ip->graph_degree,(long)ip->intermediate_graph_degree,(long)ip->nn_descent_niter,ondev,now_ms()-t0);
    eval_cagra(res,idx,qs,nq,dim,gt,argc,argv);
  } else if(!strcmp(mode,"nnd")){
    cuvsNNDescentIndexParams_t p; CK(cuvsNNDescentIndexParamsCreate(&p));
    p->graph_degree=kv(argc,argv,"deg",256); p->intermediate_graph_degree=kv(argc,argv,"inter",p->graph_degree*3/2);
    p->max_iterations=kv(argc,argv,"niter",20); p->termination_threshold=kv(argc,argv,"term1e4",1)/1e4f; p->return_distances=kv(argc,argv,"rd",0);
    cuvsNNDescentIndex_t ix; CK(cuvsNNDescentIndexCreate(&ix));
    double t0=now_ms(); CK(cuvsNNDescentBuild(res,p,ondev?&dds:&hds,NULL,ix)); cudaDeviceSynchronize();
    printf("nnd deg=%ld inter=%ld niter=%ld build_ms=%.1f\n",(long)p->graph_degree,(long)p->intermediate_graph_degree,(long)p->max_iterations,now_ms()-t0);
  } else if(!strcmp(mode,"bf")){
    long k=kv(argc,argv,"k",64), bs=kv(argc,argv,"batch",8192);
    cuvsBruteForceIndex_t bf; CK(cuvsBruteForceIndexCreate(&bf));
    double t0=now_ms(); CK(cuvsBruteForceBuild(res,&dds,L2Expanded,0.f,bf)); cudaDeviceSynchronize();
    double tb=now_ms()-t0;
    int64_t*nn; float*dd; CK(cuvsRMMAlloc(res,(void**)&nn,8*bs*k)); CK(cuvsRMMAlloc(res,(void**)&dd,4*bs*k));
    t0=now_ms(); long lim=kv(argc,argv,"rows",n);
    for(long i=0;i<lim;i+=bs){ long m=lim-i<bs?lim-i:bs; long qsh[2]={m,dim},nsh[2]={m,k};
      DLManagedTensor qt,nt,dt; mk(&qt,dbase+i*dim,1,kDLFloat,32,qsh); mk(&nt,nn,1,kDLInt,64,nsh); mk(&dt,dd,1,kDLFloat,32,nsh);
      cuvsFilter fl={0,NO_FILTER}; CK(cuvsBruteForceSearch(res,bf,&qt,&nt,&dt,fl)); }
    cudaDeviceSynchronize();
    printf("bf k=%ld batch=%ld rows=%ld build_ms=%.1f search_ms=%.1f\n",k,bs,lim,tb,now_ms()-t0);
  } else if(!strcmp(mode,"custom")){
    int k=kv(argc,argv,"k",128), deg=kv(argc,argv,"deg",64); long bs=kv(argc,argv,"batch",4096);
    uint32_t*dknn,*dg; CK(cuvsRMMAlloc(res,(void**)&dknn,4*n*k)); CK(cuvsRMMAlloc(res,(void**)&dg,4*n*deg));
    double t0=now_ms();
    (void)bs;
    if(sk_cagra_knn_build(res,base,n,dim,k,dknn)){printf("knn fail\n");return 1;}
    double t1=now_ms();
    if(sk_cagra_optimize(res,dknn,n,k,deg,dg)){printf("opt fail\n");return 1;}
    double t2=now_ms();
    long gsh[2]={n,deg}; DLManagedTensor gt_; mk(&gt_,dg,1,kDLUInt,32,gsh);
    cuvsCagraIndex_t idx; CK(cuvsCagraIndexCreate(&idx));
    CK(cuvsCagraIndexFromArgs(res,L2Expanded,&gt_,&dds,idx)); cudaDeviceSynchronize();
    double t3=now_ms();
    printf("custom k=%d deg=%d knn_ms=%.1f opt_ms=%.1f idx_ms=%.1f total_ms=%.1f\n",k,deg,t1-t0,t2-t1,t3-t2,t3-t0);
    if(kv(argc,argv,"dump",0)){ uint32_t*h=malloc(4*n*deg); cudaMemcpy(h,dg,4*n*deg,cudaMemcpyDefault); FILE*f=fopen("graph_custom.u32","wb"); fwrite(h,4,n*deg,f); fclose(f);}
    eval_cagra(res,idx,qs,nq,dim,gt,argc,argv);
  } else if(!strcmp(mode,"an")){
    printf("an: see header\n");
  }
  return 0;
}
