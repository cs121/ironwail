#ifndef KTX2_WRITE_H
#define KTX2_WRITE_H

#include "q_stdinc.h"

qboolean KTX2_WriteRGBA8File(const char *path, int width, int height, int mip_count,
                             const uint8_t **mip_data, const size_t *mip_sizes);

#endif /* KTX2_WRITE_H */
