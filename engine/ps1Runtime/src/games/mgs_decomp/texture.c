/* Adapted from FoxdieTeam/mgs_reversing, source/libdg/text.c,
 * commit 217509eae471629b1a53633b816c61adfef9ed5c.
 * See docs/mgs-decomp-native-core.md for provenance and the USA contract.
 * Adaptations: explicit context, fixed-width data types, returned record index
 * instead of a host pointer, allocation/USA assertions in the ABI adapter.
 * This file is compiled as C, not produced by the static recompiler. */
#include "texture.h"
#define DG_MAX_TEXTURES 512

int mgs_dc_find_texture(const void *records, int id, uint32_t *found)
{
    int start, index, record_id;
    const uint8_t *record;
    start = id % DG_MAX_TEXTURES;
    record = (const uint8_t *)records + start * sizeof(MgsDcTexture);
    index = start;
    do {
        /* Guest RAM has byte storage, not host C struct object lifetime. */
        record_id = record[0] | ((int)record[1] << 8);
        if (record_id == 0) {
            *found = (uint32_t)index;
            return 0;
        }
        if (record_id == id) {
            *found = (uint32_t)index;
            return 1;
        }
        index++;
        record += sizeof(MgsDcTexture);
        if (index == DG_MAX_TEXTURES) {
            record = records;
            index = 0;
        }
    } while (index != start);
    *found = UINT32_MAX;
    return 0;
}

void mgs_dc_init_textures(MgsDcTexture *records)
{
    MgsDcTexture *tex = records;
    int i;
    for (i = DG_MAX_TEXTURES; i > 0; i--) {
        tex->id = 0;
        tex->used = 0;
        tex++;
    }
}

void mgs_dc_set_texture(MgsDcTexture *tex, int id, int tp, int abr,
                        const MgsDcRect *img, const MgsDcRect *pal, int col)
{
    int x, y, w, h, cx, cy, tpage;
    tex->id = (uint16_t)id;
    tex->col = (uint8_t)col;
    tex->used = 0;
    x = img->x;
    y = img->y;
    cx = pal->x;
    cy = pal->y;
    tpage = ((x / 64) + ((y / 256) << 4)) | ((tp << 7) | (abr << 5));
    tex->tpage = (uint16_t)tpage;
    tex->clut = (uint16_t)(cy << 6 | cx >> 4);
    x %= 64;
    w = img->w;
    h = img->h;
    if (tp == 0) {
        x *= 4;
        w *= 4;
    } else {
        x *= 2;
        w *= 2;
    }
    tex->off_x = (uint8_t)x;
    tex->off_y = (uint8_t)(y % 256);
    tex->w = (uint8_t)(w - 1);
    tex->h = (uint8_t)(h - 1);
}
