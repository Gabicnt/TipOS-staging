#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdio.h>
#define SYS_memfd_create 319
static int mf(const char*n,unsigned f){return syscall(SYS_memfd_create,n,f);}
int main(void){
    printf("simple memfd test\n");
    int fd=mf("test",0);
    if(fd<0){perror("memfd");return 1;}
    printf("fd=%d\n",fd);
    if(ftruncate(fd,8192)<0){perror("ftruncate");return 1;}
    printf("ftruncate ok\n");
    void *p=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(p==MAP_FAILED){perror("mmap");return 1;}
    printf("mmap %p ok\n",p);
    *(volatile unsigned*)p=0xAABBCCDDu;
    printf("write ok read 0x%x\n", *(volatile unsigned*)p);
    // second mmap same fd
    void *q=mmap(0,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(q==MAP_FAILED){perror("mmap2");return 1;}
    printf("mmap2 %p read 0x%x %s\n",q, *(volatile unsigned*)q, *(volatile unsigned*)q==0xAABBCCDDu?"PASS":"FAIL");
    printf("simple PASS\n");
    return 0;
}
