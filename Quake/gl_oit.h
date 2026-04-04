#ifndef GL_OIT_H
#define GL_OIT_H

#include "quakedef.h"

typedef struct framesetup_s
{
	GLuint		scene_fbo;
	GLuint		oit_fbo;
	qboolean	composite_ready;
} framesetup_t;

extern framesetup_t framesetup;

void R_BeginTranslucency (void);
void R_EndTranslucency (void);

#endif
