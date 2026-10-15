#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#define SYS_memfd_create 319
static int mf(const char*n,unsigned f){return syscall(SYS_memfd_create,n,f);}
int main(void){
    printf("fork memfd test start\n");
    int fd=mf("test",0);
    printf("fd=%d\n",fd);
    if(ftruncate(fd,4096)<0){perror("ftruncate");return 1;}
    void *p=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    printf("p=%p\n",p);
    *(int*)p=0x12345678;
    printf("wrote\n");
    pid_t pid=fork();
    if(pid<0){perror("fork");return 1;}
    if(pid==0){
        printf("child pid=%d ppid=%d\n", getpid(), getppid());
        printf("child read 0x%x\n", *(int*)p);
        *(int*)p=0x87654321;
        printf("child wrote\n");
        _exit(0);
    }
    int st=0; waitpid(pid,&st,0);
    printf("parent read 0x%x after child %s\n", *(int*)p, *(int*)p==0x87654321?"PASS shared":"FAIL private");
    return 0;
}
