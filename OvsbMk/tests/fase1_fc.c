#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
static void wlog(const char *s){ write(1,s,strlen(s)); }
int main(void){
  char b[320];
  wlog("[FASE1-FC] mmap /USR/SHARE/FONTS/DEJAVUS.TTF\n");
  int fd=open("/USR/SHARE/FONTS/DEJAVUS.TTF", O_RDONLY);
  if(fd<0) fd=open("/usr/share/fonts/DEJAVUS.TTF", O_RDONLY);
  if(fd<0) fd=open("/USR/SHARE/FONTS/dejavus.ttf", O_RDONLY);
  snprintf(b,sizeof(b),"[FASE1-FC] open fd=%d\n",fd); wlog(b);
  if(fd>=0){
    void *p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
    if(p==MAP_FAILED){ wlog("[FASE1-FC] mmap FAIL\n"); } else {
      unsigned char *u=(unsigned char*)p;
      snprintf(b,sizeof(b),"[FASE1-FC] mmap %02x %02x %02x %02x %s\n",u[0],u[1],u[2],u[3], (u[0]==0&&u[1]==1&&u[2]==0&&u[3]==0)?"PASS":"FAIL"); wlog(b);
      munmap(p,4096);
    }
    close(fd);
  } else wlog("[FASE1-FC] open FAIL\n");
  wlog("[FASE1-FC] FcInit\n");
  int r=FcInit();
  snprintf(b,sizeof(b),"[FASE1-FC] FcInit %d %s\n",r, r?"PASS":"FAIL"); wlog(b);
  if(!r){ wlog("[FASE1-FC] FcInit FAIL done\n"); return 1; }
  FcPattern *pat=FcNameParse((FcChar8*)"monospace");
  FcConfigSubstitute(NULL, pat, FcMatchPattern); FcDefaultSubstitute(pat);
  FcResult res; FcPattern *m=FcFontMatch(NULL, pat, &res);
  if(m){
    FcChar8 *f=NULL;
    if(FcPatternGetString(m, FC_FILE, 0, &f)==FcResultMatch){
      snprintf(b,sizeof(b),"[FASE1-FC] FcFontMatch %s %s\n",(char*)f, (strstr((char*)f,"DejaVu")||strstr((char*)f,"dejavu")||strstr((char*)f,"DEJAVU"))?"PASS":"WARN"); wlog(b);
    } else wlog("[FASE1-FC] FcFontMatch no file FAIL\n");
    FcPatternDestroy(m);
  } else wlog("[FASE1-FC] FcFontMatch null FAIL\n");
  FcPatternDestroy(pat);
  FT_Library lib; FT_Init_FreeType(&lib);
  const char *try_paths[]={"/usr/share/fonts/DEJAVUS.TTF","/USR/SHARE/FONTS/DEJAVUS.TTF","/usr/share/fonts/dejavus.ttf",0};
  FT_Face face; int e=-1;
  for(int i=0;try_paths[i];i++){
    e=FT_New_Face(lib, try_paths[i], 0, &face);
    snprintf(b,sizeof(b),"[FASE1-FC] FT_New_Face %s -> %d %s\n",try_paths[i],e,e==0?"PASS":"retry"); wlog(b);
    if(e==0) break;
  }
  if(e==0){
    snprintf(b,sizeof(b),"[FASE1-FC] face %s glyphs %ld PASS\n", face->family_name?face->family_name:"(null)", face->num_glyphs); wlog(b);
  } else wlog("[FASE1-FC] FT_New_Face FAIL\n");
  int cfd=open("/cache/fontconfig", O_RDONLY|O_DIRECTORY);
  if(cfd<0) cfd=open("/CACHE/FONTCONFIG", O_RDONLY|O_DIRECTORY);
  snprintf(b,sizeof(b),"[FASE1-FC] open /cache/fontconfig fd=%d %s\n",cfd, cfd>=0?"PASS":"FAIL (mem only)"); wlog(b);
  if(cfd>=0) close(cfd);
  // check if cache was written
  cfd=open("/cache/fontconfig", O_RDONLY|O_DIRECTORY);
  if(cfd>=0){
    // try readdir via opendir
    // use getdents via opendir
        DIR *d=opendir("/cache/fontconfig");
    if(d){
      struct dirent *en; int cnt=0;
      while((en=readdir(d))){ if(en->d_name[0]=='.') continue; cnt++; snprintf(b,sizeof(b),"[FASE1-FC] cache entry %s\n",en->d_name); wlog(b); if(cnt>5) break; }
      closedir(d);
      snprintf(b,sizeof(b),"[FASE1-FC] cache cnt=%d %s\n",cnt, cnt>0?"written":"empty (mem)"); wlog(b);
    } else wlog("[FASE1-FC] opendir cache FAIL\n");
    close(cfd);
  }
  wlog("[FASE1-FC] done\n");
  return 0;
}
