#include "image.h"
#include "text.h"
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#define IMAGE_PIXELS_MAX (16u * 1024u * 1024u)
static uint32_t u32(const unsigned char *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static unsigned u16(const unsigned char *p) { return p[0]|((unsigned)p[1]<<8); }
void image_free(Image *im)
{ free(im->pixels); free(im->indices); memset(im,0,sizeof(*im)); }
static int decode_bmp(Image *im,const unsigned char *p,size_t n)
{
    uint32_t dib,off,width,height,compression,colors;
    size_t stride,count,i,pos;
    int top,y,x;
    if(n<54 || p[0]!='B' || p[1]!='M') return -1;
    off=u32(p+10); dib=u32(p+14);
    if(dib<40 || dib>n-14 || off>n || off<14+dib || u16(p+26)!=1) return -1;
    width=u32(p+18); height=u32(p+22); top=(height&0x80000000u)!=0;
    if(top) height=0u-height;
    if(!width || width>16384 || !height || height>16384 || width>IMAGE_PIXELS_MAX/height) return -1;
    im->w=(int)width; im->h=(int)height; im->bpp=(int)u16(p+28);
    compression=u32(p+30); colors=u32(p+46);
    if(im->bpp!=1 && im->bpp!=4 && im->bpp!=8 && im->bpp!=24 && im->bpp!=32) return -1;
    if(compression && !(compression==1 && im->bpp==8 && !top)) return -1;
    count=(size_t)width*height;
    im->pixels=calloc(count,sizeof(*im->pixels));
    if(!im->pixels) return -1;
    if(im->bpp<=8) {
        if(!colors) colors=1u<<im->bpp;
        if(colors>(1u<<im->bpp) || (size_t)colors*4>off-(14+dib)) return -1;
        im->palette_size=(int)colors;
        for(i=0;i<colors;++i) {
            const unsigned char *c=p+14+dib+i*4;
            im->palette[i]=((uint32_t)c[2]<<16)|((uint32_t)c[1]<<8)|c[0];
        }
        im->indices=calloc(count,1);
        if(!im->indices) return -1;
    }
    if(compression==1) {
        int ended=0; x=0; y=0; pos=off;
        while(pos+2<=n && !ended) {
            unsigned run=p[pos++],value=p[pos++],j;
            if(run) {
                if(y>=im->h || run>width-(unsigned)x || value>=colors) return -1;
                for(j=0;j<run;++j) im->indices[(size_t)(im->h-1-y)*width+x++]=(unsigned char)value;
            } else if(value==0) { x=0; ++y; if(y>im->h) return -1; }
            else if(value==1) ended=1;
            else if(value==2) {
                if(pos+2>n) return -1;
                x+=p[pos++]; y+=p[pos++];
                if(x>im->w || y>=im->h) return -1;
            } else {
                size_t bytes=value+(value&1u);
                if(y>=im->h || value>width-(unsigned)x || bytes>n-pos) return -1;
                for(j=0;j<value;++j) {
                    unsigned c=p[pos+j]; if(c>=colors) return -1;
                    im->indices[(size_t)(im->h-1-y)*width+x++]=(unsigned char)c;
                }
                pos+=bytes;
            }
        }
        if(!ended) return -1;
    } else {
        stride=(((size_t)width*im->bpp+31)/32)*4;
        if(stride> (n-off)/height) return -1;
        for(y=0;y<im->h;++y) {
            const unsigned char *row=p+off+(size_t)(top?y:im->h-1-y)*stride;
            for(x=0;x<im->w;++x) {
                size_t dst=(size_t)y*width+x;
                if(im->bpp<=8) {
                    unsigned idx=im->bpp==8?row[x]:im->bpp==4?((row[x/2]>>(x%2?0:4))&15):((row[x/8]>>(7-x%8))&1);
                    if(idx>=colors) return -1;
                    im->indices[dst]=(unsigned char)idx;
                } else {
                    const unsigned char *c=row+(size_t)x*(im->bpp/8);
                    im->pixels[dst]=((uint32_t)c[2]<<16)|((uint32_t)c[1]<<8)|c[0];
                }
            }
        }
    }
    if(im->indices) for(i=0;i<count;++i) im->pixels[i]=im->palette[im->indices[i]];
    return 0;
}
int image_decode(Image *image,const void *data,size_t size)
{
    Image im={0}; const unsigned char *p=data;
    if(!data || size<2 || size>INT_MAX) return -1;
    if(p[0]=='B' && p[1]=='M') {
        if(decode_bmp(&im,p,size)) { image_free(&im); return -1; }
    } else {
        int w,h,channels; unsigned char *rgb; size_t i,count;
        if(!stbi_info_from_memory(p,(int)size,&w,&h,&channels) || w<=0 || h<=0 ||
            w>16384 || h>16384 || (unsigned)w>IMAGE_PIXELS_MAX/(unsigned)h) return -1;
        rgb=stbi_load_from_memory(p,(int)size,&w,&h,&channels,3);
        if(!rgb) return -1;
        im.w=w; im.h=h; im.bpp=24; count=(size_t)w*h;
        im.pixels=malloc(count*sizeof(*im.pixels));
        if(!im.pixels) { stbi_image_free(rgb); return -1; }
        for(i=0;i<count;++i) im.pixels[i]=((uint32_t)rgb[i*3]<<16)|((uint32_t)rgb[i*3+1]<<8)|rgb[i*3+2];
        stbi_image_free(rgb);
    }
    image_free(image); *image=im; return 0;
}
int image_load(Image *image,const char *path)
{
    size_t size; char *data=text_read_file(path,&size); int result;
    if(!data) return -1;
    result=image_decode(image,data,size); free(data); return result;
}
