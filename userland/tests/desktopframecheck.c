/* Poll a REAL grim PPM capture for a rendered wallpaper/panel. Independent host
 * PNG acceptance is still mandatory; black frames never become a passing test. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
static int number(FILE *f){int c;do{c=fgetc(f);if(c=='#'){while(c!='\n'&&c!=EOF)c=fgetc(f);}}while(c!=EOF&&isspace(c));if(c<'0'||c>'9')return -1;int n=0;do{n=n*10+c-'0';if(n>100000)return -1;c=fgetc(f);}while(c>='0'&&c<='9');return isspace(c)?n:-1;}
static unsigned palette(const unsigned char *pixels,int w,int y0,int y1){unsigned char *seen=calloc(1,1<<21);if(!seen)return 0;unsigned distinct=0;for(int y=y0;y<y1;y++)for(int x=0;x<w;x++){const unsigned char *p=pixels+((size_t)y*w+x)*3;unsigned rgb=((unsigned)p[0]<<16)|((unsigned)p[1]<<8)|p[2];unsigned char bit=1u<<(rgb&7);if(!(seen[rgb>>3]&bit)){seen[rgb>>3]|=bit;distinct++;}}free(seen);return distinct;}
int main(int argc,char **argv){if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;if(fgetc(f)!='P'||fgetc(f)!='6'){fclose(f);return 2;}int w=number(f),h=number(f),max=number(f);if(w<128||h<96||w>4096||h>4096||max!=255){fclose(f);return 2;}size_t n=(size_t)w*h*3;unsigned char *p=malloc(n);if(!p){fclose(f);return 2;}if(fread(p,1,n,f)!=n||fgetc(f)!=EOF){free(p);fclose(f);return 2;}fclose(f);unsigned bg=palette(p,w,0,h*4/5),panel=palette(p,w,h*11/12,h);free(p);printf("desktopframecheck: %dx%d wallpaper colors=%u panel colors=%u\n",w,h,bg,panel);return bg>=1000&&panel>=32?0:1;}
