
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
int main(void){ printf("pre fork\n"); pid_t p=fork(); if(p==0){ printf("child %d\n", getpid()); _exit(0);} else if(p>0){ printf("parent child %d\n", p); int st; waitpid(p,&st,0); printf("wait %d\n", WEXITSTATUS(st)); } else perror("fork"); printf("done\n"); return 0; }
