
typedef unsigned long u64;
static long sc6(long n,long a1,long a2,long a3,long a4,long a5,long a6){
 register long r10 __asm__("r10")=a4;
 register long r8 __asm__("r8")=a5;
 register long r9 __asm__("r9")=a6;
 __asm__ volatile("syscall":"+a"(n):"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
 return n;
}
static long sc3(long n,long a1,long a2,long a3){return sc6(n,a1,a2,a3,0,0,0);}
static void my_puts(const char*s){ u64 l=0; const char *p=s; while(*p){ l++; p++; } sc3(1,1,(long)s,l); }
static void puthex(unsigned v){ char b[9]; for(int i=7;i>=0;i--){ b[i]="0123456789ABCDEF"[v&15]; v>>=4;} b[8]='\n'; sc3(1,1,(long)b,9); }
int main(void){
 my_puts("memfd raw scm start\n");
 long mfd=sc6(319,(long)"wl_shm_test",1,0,0,0,0);
 if(mfd<0){ my_puts("memfd fail\n"); puthex((unsigned)mfd); return 1; }
 my_puts("memfd ok\n"); puthex((unsigned)mfd);
 long tr=sc6(77,mfd,4096,0,0,0,0);
 if(tr<0){ my_puts("ftruncate fail\n"); puthex((unsigned)tr); return 1; }
 my_puts("ftruncate ok\n");
 long p=sc6(9,0,4096,3,1,mfd,0);
 if(p<0 || p==(long)-1){ my_puts("mmap A fail\n"); puthex((unsigned)p); return 1; }
 my_puts("mmap A ok\n"); puthex((unsigned)p);
 *(volatile unsigned*)p=0xFFFF0000u;
 my_puts("A wrote\n");
 int sv[2]; long r=sc6(53,1,1,0,(long)sv,0,0);
 if(r<0){ my_puts("socketpair fail\n"); puthex((unsigned)r); return 1; }
 my_puts("socketpair ok\n");
 long pid=sc6(57,0,0,0,0,0,0);
 if(pid<0){ my_puts("fork fail\n"); puthex((unsigned)pid); return 1; }
 if(pid==0){
   sc6(3,mfd,0,0,0,0,0);
   struct { void *base; u64 len; } iov; char d='x'; iov.base=&d; iov.len=1;
   struct { void *name; unsigned nlen; unsigned pad; void *iov; u64 iovlen; void *ctrl; u64 clen; unsigned flags; unsigned pad2; } msg={0};
   msg.iov=&iov; msg.iovlen=1;
   char cmsgbuf[64]={0};
   msg.ctrl=cmsgbuf; msg.clen=sizeof(cmsgbuf);
   long n=sc6(47,sv[1],(long)&msg,0,0,0,0);
   if(n<=0){ my_puts("B recvmsg fail\n"); puthex((unsigned)n); sc6(60,1,0,0,0,0,0); }
   my_puts("B recv ok clen="); puthex((unsigned)msg.clen);
   struct { u64 cmsg_len; int cmsg_level; int cmsg_type; } *cmsg=(void*)cmsgbuf;
   if(msg.clen<16 || cmsg->cmsg_level!=1 || cmsg->cmsg_type!=1){ my_puts("B no cmsg\n"); sc6(60,1,0,0,0,0,0); }
   int rfd=*(int*)((char*)cmsg+16);
   my_puts("B got fd "); puthex((unsigned)rfd);
   long q=sc6(9,0,4096,3,1,rfd,0);
   if(q<0||q==(long)-1){ my_puts("B mmap fail\n"); puthex((unsigned)q); sc6(60,1,0,0,0,0,0); }
   my_puts("B mmap ok "); puthex((unsigned)q);
   unsigned v=*(volatile unsigned*)q;
   my_puts("B read "); puthex(v);
   if(v!=0xFFFF0000u){ my_puts("B FAIL\n"); sc6(60,1,0,0,0,0,0); }
   *(volatile unsigned*)q=0xFF00FF00u;
   my_puts("B wrote\n");
   sc6(60,0,0,0,0,0,0);
 }
 sc6(3,sv[1],0,0,0,0,0);
 struct { void *base; u64 len; } iov2; char d2='x'; iov2.base=&d2; iov2.len=1;
 struct { void *name; unsigned nlen; unsigned pad; void *iov; u64 iovlen; void *ctrl; u64 clen; unsigned flags; unsigned pad2; } msg2={0};
 msg2.iov=&iov2; msg2.iovlen=1;
 char cmsgbuf2[64]={0};
 struct { u64 cmsg_len; int cmsg_level; int cmsg_type; } *c2=(void*)cmsgbuf2;
 c2->cmsg_len=20; c2->cmsg_level=1; c2->cmsg_type=1;
 *(int*)((char*)c2+16)=(int)mfd;
 msg2.ctrl=cmsgbuf2; msg2.clen=c2->cmsg_len;
 long sn=sc6(46,sv[0],(long)&msg2,0,0,0,0);
 if(sn<0){ my_puts("sendmsg fail\n"); puthex((unsigned)sn); return 1; }
 my_puts("A sendmsg ok\n");
 long st=0; long w=sc6(61,pid,(long)&st,0,0,0,0);
 if(w==0) w=sc6(61,pid,(long)&st,0,0,0,0);
 my_puts("wait "); puthex((unsigned)st);
 unsigned v2=*(volatile unsigned*)p;
 my_puts("A re-read "); puthex(v2);
 if(v2==0xFF00FF00u) my_puts("memfd SCM PASS\n"); else my_puts("memfd SCM FAIL\n");
 return v2==0xFF00FF00u?0:1;
}
__attribute__((noreturn)) void _start(void){ long r=main(); sc6(60,r,0,0,0,0,0); for(;;)__asm__ volatile("hlt"); }
