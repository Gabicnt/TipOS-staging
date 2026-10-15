
typedef unsigned long u64;
static long sc6(long n,long a1,long a2,long a3,long a4,long a5,long a6){
 register long r10 __asm__("r10")=a4;
 register long r8 __asm__("r8")=a5;
 register long r9 __asm__("r9")=a6;
 __asm__ volatile("syscall":"+a"(n):"D"(a1),"S"(a2),"d"(a3),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
 return n;
}
static long sc3(long n,long a1,long a2,long a3){return sc6(n,a1,a2,a3,0,0,0);}
static void my_puts(const char*s){ u64 l=0; const volatile char *p=s; while(*p){ l++; p++; } sc3(1,1,(long)s,l); }
int main(void){
 my_puts("raw fork start\n");
 long pid=sc6(57,0,0,0,0,0,0);
 if(pid<0){ my_puts("fork fail\n"); return 1;}
 if(pid==0){ my_puts("child\n"); sc6(60,0,0,0,0,0,0); }
 my_puts("parent\n");
 long st=0; long w=sc6(61,pid,(long)&st,0,0,0,0);
 if(w==0) w=sc6(61,pid,(long)&st,0,0,0,0);
 if(w!=pid){ my_puts("wait fail\n"); return 2;}
 my_puts("raw fork PASS\n");
 return 0;
}
__attribute__((noreturn)) void _start(void){ long r=main(); sc6(60,r,0,0,0,0,0); for(;;)__asm__ volatile("hlt"); }
