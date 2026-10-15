#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>

int main(void){
    printf("[EPOLL_TEST] start\n");
    int epfd = epoll_create1(0);
    printf("[EPOLL_TEST] epoll_create1 -> %d errno=%d (%s)\n", epfd, errno, strerror(errno));
    if(epfd < 0){ printf("[EPOLL_TEST] FAIL create\n"); return 1; }

    int fds[2];
    if(pipe(fds) < 0){ printf("[EPOLL_TEST] pipe FAIL %d %s\n", errno, strerror(errno)); return 1; }
    printf("[EPOLL_TEST] pipe -> %d %d\n", fds[0], fds[1]);

    int fl = fcntl(fds[0], F_SETFL, O_NONBLOCK);
    printf("[EPOLL_TEST] fcntl F_SETFL O_NONBLOCK -> %d errno=%d\n", fl, errno);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = fds[0];
    int r = epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev);
    printf("[EPOLL_TEST] epoll_ctl ADD fd=%d -> %d errno=%d (%s)\n", fds[0], r, errno, strerror(errno));
    if(r < 0){ printf("[EPOLL_TEST] FAIL epoll_ctl ADD\n"); return 1; }

    // also test dup: EPOLL_CTL_ADD same fd should return EEXIST
    int r2 = epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev);
    printf("[EPOLL_TEST] epoll_ctl ADD dup -> %d errno=%d (%s) expect EEXIST=%d\n", r2, errno, strerror(errno), EEXIST);

    const char c='x';
    ssize_t w = write(fds[1], &c, 1);
    printf("[EPOLL_TEST] write -> %zd errno=%d\n", w, errno);

    struct epoll_event evs[4];
    memset(evs, 0, sizeof(evs));
    int n = epoll_wait(epfd, evs, 4, 1000);
    printf("[EPOLL_TEST] epoll_wait -> %d errno=%d (%s)\n", n, errno, strerror(errno));
    if(n > 0){
        for(int i=0;i<n;i++) printf("[EPOLL_TEST] ev[%d] events=0x%x data.fd=%d\n", i, evs[i].events, evs[i].data.fd);
    }
    if(n == 1 && (evs[0].events & EPOLLIN) && evs[0].data.fd == fds[0]){
        printf("[EPOLL_TEST] PASS\n");
        close(fds[0]); close(fds[1]); close(epfd);
        return 0;
    } else {
        printf("[EPOLL_TEST] FAIL expected n=1 EPOLLIN fd=%d got n=%d events=0x%x fd=%d\n",
            fds[0], n, n>0?evs[0].events:0, n>0?evs[0].data.fd:-1);
        close(fds[0]); close(fds[1]); close(epfd);
        return 1;
    }
}
