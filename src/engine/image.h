#ifndef WOS_IMAGE_H
#define WOS_IMAGE_H
#include <stddef.h>
#include <stdint.h>
typedef struct Image {
    int w, h;
    uint32_t *pixels; /* 0x00RRGGBB, top row first */
    unsigned char *indices; /* palette indices for 1/4/8-bit BMP; NULL otherwise */
    uint32_t palette[256];
    int palette_size, bpp;
} Image;
/* Initialize with {0}; loaders replace only on success; image_free releases ownership. */
int image_load(Image *image, const char *path);
int image_decode(Image *image, const void *data, size_t size);
void image_free(Image *image);
#endif
