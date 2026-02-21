#ifndef R_MAPTEX_EXPORT_H
#define R_MAPTEX_EXPORT_H

void R_MapTex_ExportInit(void);
void R_MapTex_ExportFromBsp(qmodel_t *mod, const byte *bsp_base, const lump_t *tex_lump);

#endif /* R_MAPTEX_EXPORT_H */
