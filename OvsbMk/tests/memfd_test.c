#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/syscall.h>
#ifndef SYS_memfd_create
#define SYS_memfd_create 319
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001
#endif
static int my_memfd_create(const char *name, unsigned int flags) {
    return syscall(SYS_memfd_create, name, flags);
}
int main(void){
    printf("memfd ded test: memfd_create -> ftruncate -> mmap -> SCM_RIGHTS\n");
    int mfd = my_memfd_create("wl_shm_test", MFD_CLOEXEC);
    if(mfd<0){ perror("memfd_create"); return 1; }
    printf("memfd_create fd=%d OK\n", mfd);
    if(ftruncate(mfd, 4096)<0){ perror("ftruncate"); return 1; }
    printf("ftruncate 4096 OK\n");
    void *a = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mfd, 0);
    if(a==MAP_FAILED){ perror("mmap A"); return 1; }
    printf("mmap A at %p OK\n", a);
    *(volatile unsigned int*)a = 0xFFFF0000u;
    printf("A wrote 0xFFFF0000\n");
    int sv[2];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, sv)<0){ perror("socketpair"); return 1; }
    printf("socketpair %d<->%d OK\n", sv[0], sv[1]);
    pid_t pid = fork();
    if(pid<0){ perror("fork"); return 1; }
    if(pid==0){
        close(mfd);
        struct msghdr msg={0};
        struct iovec iov;
        char dummy='x';
        iov.iov_base=&dummy; iov.iov_len=1;
        msg.msg_iov=&iov; msg.msg_iovlen=1;
        char cmsgbuf[64]={0};
        msg.msg_control=cmsgbuf; msg.msg_controllen=sizeof(cmsgbuf);
        ssize_t n = recvmsg(sv[1], &msg, 0);
        if(n<=0){ perror("child recvmsg"); _exit(1); }
        printf("B recvmsg n=%zd controllen=%zu\n", n, msg.msg_controllen);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if(!cmsg || cmsg->cmsg_level!=SOL_SOCKET || cmsg->cmsg_type!=SCM_RIGHTS){
            fprintf(stderr,"B no SCM_RIGHTS\n");
            _exit(1);
        }
        int *fdarr = (int*)CMSG_DATA(cmsg);
        int rfd = fdarr[0];
        printf("B got fd %d via SCM_RIGHTS\n", rfd);
        char procfd[32]; snprintf(procfd,sizeof(procfd),"/proc/self/fd/%d", rfd);
        int dupfd = open(procfd, O_RDWR);
        printf("B open %s -> %d %s\n", procfd, dupfd, dupfd>=0?"OK":"FAIL");
        if(dupfd>=0) close(dupfd);
        void *b = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, rfd, 0);
        if(b==MAP_FAILED){ perror("child mmap"); _exit(1); }
        printf("B mmap at %p OK\n", b);
        unsigned int v = *(volatile unsigned int*)b;
        printf("B read 0x%08x %s\n", v, v==0xFFFF0000u?"PASS":"FAIL");
        if(v!=0xFFFF0000u) _exit(1);
        *(volatile unsigned int*)b = 0xFF00FF00u;
        printf("B wrote 0xFF00FF00\n");
        munmap(b,4096);
        close(rfd);
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    struct msghdr msg2={0};
    struct iovec iov2;
    char dummy2='x';
    iov2.iov_base=&dummy2; iov2.iov_len=1;
    msg2.msg_iov=&iov2; msg2.msg_iovlen=1;
    char cmsgbuf2[64]={0};
    struct cmsghdr *cmsg2 = (struct cmsghdr*)cmsgbuf2;
    cmsg2->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg2->cmsg_level = SOL_SOCKET;
    cmsg2->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg2), &mfd, sizeof(int));
    msg2.msg_control=cmsgbuf2; msg2.msg_controllen=cmsg2->cmsg_len;
    ssize_t sn = sendmsg(sv[0], &msg2, 0);
    if(sn<0){ perror("sendmsg"); return 1; }
    printf("A sendmsg n=%zd OK\n", sn);
    int st=0;
    waitpid(pid,&st,0);
    printf("child exit status %d (%s)\n", WEXITSTATUS(st), WEXITSTATUS(st)==0?"PASS":"FAIL");
    unsigned int v2 = *(volatile unsigned int*)a;
    printf("A re-read 0x%08x %s\n", v2, v2==0xFF00FF00u?"PASS shared":"FAIL");
    printf("memfd SCM test %s\n", (WEXITSTATUS(st)==0 && v2==0xFF00FF00u)?"PASS":"FAIL");
    munmap(a,4096);
    close(mfd);
    close(sv[0]);
    return (WEXITSTATUS(st)==0 && v2==0xFF00FF00u)?0:1;
}
