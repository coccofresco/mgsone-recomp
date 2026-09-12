#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Fixed data layout shared with the USA guest; all host pointers stay in the
 * call interface. No native pointer is stored in guest memory or savestates. */
typedef struct MgsDcTexture {
    uint16_t id;
    uint8_t used, col;
    uint16_t tpage, clut;
    uint8_t off_x, off_y, w, h;
} MgsDcTexture;
typedef struct MgsDcRect { int16_t x, y, w, h; } MgsDcRect;

int mgs_dc_find_texture(const void *records, int id, uint32_t *found);
void mgs_dc_init_textures(MgsDcTexture *records);
void mgs_dc_set_texture(MgsDcTexture *tex, int id, int tp, int abr,
                        const MgsDcRect *img, const MgsDcRect *pal, int col);

#ifdef __cplusplus
}
static_assert(sizeof(MgsDcTexture) == 12);
static_assert(sizeof(MgsDcRect) == 8);
#else
_Static_assert(sizeof(MgsDcTexture) == 12, "DG_TEX guest layout");
_Static_assert(sizeof(MgsDcRect) == 8, "RECT guest layout");
#endif
