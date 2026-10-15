
typedef unsigned long u64;
static long sc6(long n,long a1,long a2,long a3,long a4,long a5,long a6){
 register long r10 __asm__("r10")=a4;
 register long r8 __asm__("r8")=a5;
 register long r9 __asm__("r9")=a6;
 __asm__ volatile("syscall":"+a"(n):"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
 return n;
}
static long sc3(long n,long a1,long a2,long a3){return sc6(n,a1,a2,a3,0,0,0);}
static long sc1(long n,long a1){return sc6(n,a1,0,0,0,0,0);}
static void prints(const char*s){ long l=0; while(s[l])l++; sc3(1,1,(long)s,l); }
static void puthex(unsigned v){ char b[9]; for(int i=7;i>=0;i--){b[i]="0123456789ABCDEF"[v&15]; v>>=4;} b[8]='\n'; sc3(1,1,(long)b,9); }
__attribute__((naked,noreturn)) static void rest(void){ __asm__ volatile("mov $15,%rax; syscall"); }

#define SYS_memfd_create 319
#define MFD_CLOEXEC 1
int main(void){
 prints("memfd raw ded: start\n");
 long mfd=sc6(SYS_memfd_create,(long)"wl_shm_test",MFD_CLOEXEC,0,0,0,0);
 if(mfd<0){ prints("memfd fail\n"); puthex((unsigned)mfd); return 1;}
 prints("memfd ok fd="); puthex((unsigned)mfd);
 // ftruncate via 77
 long tr=sc6(77,mfd,4096,0,0,0,0);
 if(tr<0){ prints("ftruncate fail\n"); puthex((unsigned)tr); return 1;}
 prints("ftruncate ok\n");
 // mmap 9
 long p=sc6(9,0,4096,3,1,mfd,0); // MAP_SHARED=1
 if(p<0 || p==(long)-1){ prints("mmap A fail\n"); puthex((unsigned)p); return 1;}
 prints("mmap A ok\n");
 *(volatile unsigned*)p=0xFFFF0000u;
 prints("A wrote\n");
 // socketpair 53
 int sv[2]; long r=sc6(53,1,1,0,(long)sv,0,0); // AF_UNIX=1 SOCK_STREAM=1
 if(r<0){ prints("socketpair fail\n"); puthex((unsigned)r); return 1;}
 prints("socketpair ok\n");
 long pid=sc6(57,0,0,0,0,0,0); // fork
 if(pid<0){ prints("fork fail\n"); puthex((unsigned)pid); return 1;}
 if(pid==0){
   sc6(3,mfd,0,0,0,0,0); // close mfd
   // recvmsg 47? In zig 47->92
   struct { void *base; unsigned long len; } iov; char d='x'; iov.base=&d; iov.len=1;
   struct { void *name; unsigned nlen; unsigned pad; void *iov; unsigned long iovlen; void *ctrl; unsigned long clen; unsigned flags; unsigned pad2; } msg={0};
   msg.iov=&iov; msg.iovlen=1;
   char cmsgbuf[64]={0};
   msg.ctrl=cmsgbuf; msg.clen=sizeof(cmsgbuf);
   long n=sc6(47,sv[1],(long)&msg,0,0,0,0); // recvmsg
   if(n<=0){ prints("B recvmsg fail\n"); puthex((unsigned)n); sc6(60,1,0,0,0,0,0); }
   prints("B recv ok controllen="); puthex((unsigned)msg.clen);
   struct { unsigned long cmsg_len; int cmsg_level; int cmsg_type; } *cmsg=(void*)cmsgbuf;
   if(msg.clen<16 || cmsg->cmsg_level!=1 || cmsg->cmsg_type!=1){ prints("B no cmsg\n"); sc6(60,1,0,0,0,0,0); }
   int rfd=*(int*)((char*)cmsg+16);
   prints("B got fd "); puthex((unsigned)rfd);
   long q=sc6(9,0,4096,3,1,rfd,0);
   if(q<0||q==(long)-1){ prints("B mmap fail\n"); puthex((unsigned)q); sc6(60,1,0,0,0,0,0); }
   prints("B mmap ok\n");
   unsigned v=*(volatile unsigned*)q;
   prints("B read "); puthex(v);
   if(v!=0xFFFF0000u){ prints("B FAIL\n"); sc6(60,1,0,0,0,0,0); }
   *(volatile unsigned*)q=0xFF00FF00u;
   prints("B wrote\n");
   sc6(3,rfd,0,0,0,0,0);
   sc6(60,0,0,0,0,0,0);
 }
 // parent
 sc6(3,sv[1],0,0,0,0,0);
 // sendmsg 46
 { struct { void *base; unsigned long len; } iov2; char d2='x'; iov2.base=&d2; iov2.len=1;
   struct { void *name; unsigned nlen; unsigned pad; void *iov; unsigned long iovlen; void *ctrl; unsigned long clen; unsigned flags; unsigned pad2; } msg2={0};
   msg2.iov=&iov2; msg2.iovlen=1;
   char cmsgbuf2[64]={0};
   struct { unsigned long cmsg_len; int cmsg_level; int cmsg_type; } *c2=(void*)cmsgbuf2;
   c2->cmsg_len=20; c2->cmsg_level=1; c2->cmsg_type=1;
   *(int*)((char*)c2+16)=(int)mfd;
   msg2.ctrl=cmsgbuf2; msg2.clen=c2->cmsg_len;
   long sn=sc6(46,sv[0],(long)&msg2,0,0,0,0);
   if(sn<0){ prints("sendmsg fail\n"); puthex((unsigned)sn); return 1;}
   prints("A sendmsg ok\n");
 }
 long st=0; long w=sc6(61,pid,(long)&st,0,0,0,0);
 prints("wait "); puthex((unsigned)st);
 unsigned v2=*(volatile unsigned*)p;
 prints("A re-read "); puthex(v2);
 if(v2==0xFF00FF00u) prints("memfd SCM PASS\n"); else prints("memfd SCM FAIL\n");
 return v2==0xFF00FF00u?0:1;
}
__attribute__((noreturn)) void _start(void){ long r=main(); sc6(60,r,0,0,0,0,0); for(;;)__asm__ volatile("hlt"); }
