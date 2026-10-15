#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
static void wlog(const char *s){ write(1,s,strlen(s)); }
static void tolow(char *d,const char *s){ int i=0; while(s[i]){ d[i]=s[i]>='A'&&s[i]<='Z'?s[i]+32:s[i]; i++; } d[i]=0; }
int main(void){
    wlog("[FASE1-STAT] stat /USR/SHARE/FONTS\n");
    struct stat st; 
    if(stat("/USR/SHARE/FONTS",&st)==0){ char tmp[128]; snprintf(tmp,sizeof(tmp),"[FASE1-STAT] stat ok mode=%o size=%ld\n",(unsigned)st.st_mode,(long)st.st_size); wlog(tmp); } else { perror("stat"); }
    const char *dirs[]={"/USR/SHARE/FONTS","/usr/share/fonts","/USR/SHARE/FONTS/","/usr/share/fonts/",0};
    for(int di=0;dirs[di];di++){
        wlog("[FASE1-STAT] opendir "); wlog(dirs[di]); wlog("\n");
        DIR *d=opendir(dirs[di]);
        if(!d){ perror("opendir"); continue; }
        wlog("[FASE1-STAT] getdents:\n");
        struct dirent *e; int cnt=0, found=0;
        while((e=readdir(d))){
            if(e->d_name[0]=='.') continue;
            char tmp[256]; snprintf(tmp,sizeof(tmp),"  [%d] name='%s' len=%d type=%d\n",cnt,e->d_name,(int)strlen(e->d_name),(int)e->d_type); wlog(tmp);
            cnt++;
            char low[256]; tolow(low,e->d_name);
            if(strstr(low,"dejavu")||strstr(low,"dejavus")) found=1;
            if(cnt>20) break;
        }
        closedir(d);
        char tmp[64]; snprintf(tmp,sizeof(tmp),"[FASE1-STAT] cnt=%d found=%d dir=%s\n",cnt,found,dirs[di]); wlog(tmp);
        if(found){ wlog("[FASE1-STAT] PASS found ttf\n"); }
    }
    wlog("[FASE1-STAT] opendir /USR/SHARE/X11/XKB/RULES\n");
    DIR *d=opendir("/USR/SHARE/X11/XKB/RULES");
    if(!d) d=opendir("/usr/share/X11/xkb/rules");
    if(!d){ perror("opendir rules"); return 1; }
    wlog("[FASE1-STAT] rules:\n");
    int cnt=0; struct dirent *e;
    while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char t2[256]; snprintf(t2,sizeof(t2),"  %s\n",e->d_name); wlog(t2); cnt++; if(cnt>14) break; }
    closedir(d);
    wlog("[FASE1-STAT] done\n");
    return 0;
}
