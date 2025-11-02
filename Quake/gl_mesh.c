/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// gl_mesh.c: triangle model functions

#include "quakedef.h"
#include <assert.h>
#include <string.h> /* memset, memcpy */
#include <stdlib.h> /* malloc, free */

/*
=================================================================

ALIAS MODEL DISPLAY LIST / VBO GENERATION

=================================================================
*/

/*
 * Kleinere Hilfsfunktion für saubere Alignment-Berechnungen.
 * Erwartet, dass 'alignment' eine Zweierpotenz ist (Engine-Ann.: ssbo_align).
 */
static size_t align_up (size_t value, size_t alignment)
{
	if (alignment == 0) return value;
	const size_t mask = alignment - 1;
	/* Voraussetzung: alignment ist Power-of-Two */
	assert ((alignment & mask) == 0);
	return (value + mask) & ~mask;
}

/*
================
GL_MakeAliasModelDisplayLists

Saves data needed to build the VBO for this model on the hunk. Afterwards this
is copied to Mod_Extradata.

Original code by MH from RMQEngine

Härtung:
- pheader -> paliashdr Bugfix
- Plausibilitätsprüfungen (numposes/numverts/numtris)
- NULL-Checks und sauberer LowMark-Restore
================
*/
void GL_MakeAliasModelDisplayLists (qmodel_t* aliasmodel, aliashdr_t* paliashdr)
{
	int i, j;
	int mark;
	trivertx_t* verts = NULL;
	unsigned short* indexes = NULL;
	unsigned short* remap = NULL;
	aliasmesh_t* desc = NULL;

	/* Basisprüfungen */
	if (!aliasmodel || !paliashdr)
		return;

	if (paliashdr->numposes <= 0 || paliashdr->numverts <= 0 || paliashdr->numtris < 0)
		return;

	/* first, copy the verts onto the hunk */
	{
		const size_t num_verts_total = (size_t)paliashdr->numposes * (size_t)paliashdr->numverts;
		const size_t bytes = num_verts_total * sizeof (trivertx_t);

		if (num_verts_total == 0 || bytes / sizeof (trivertx_t) != num_verts_total)
			return; /* Overflow-Schutz */

		verts = (trivertx_t*)Hunk_AllocNoFill ((int)bytes);
		if (!verts)
			return;

		paliashdr->vertexes = (byte*)verts - (byte*)paliashdr;

		for (i = 0; i < paliashdr->numposes; i++)
		{
			for (j = 0; j < paliashdr->numverts; j++)
			{
				/* poseverts ist Engine-Global (wie im Original) */
				verts[i * paliashdr->numverts + j] = poseverts[i][j];
			}
		}
	}

	/* es kann nicht mehr als 2 * numverts (Seam-Duplikate) geben */
	desc = (aliasmesh_t*)Hunk_Alloc ((int)(sizeof (aliasmesh_t) * (size_t)paliashdr->numverts * 2u));
	if (!desc)
		return;

	/* immer genau 3 Indices pro Tri */
	if (paliashdr->numtris > 0)
	{
		indexes = (unsigned short*)Hunk_Alloc ((int)(sizeof (unsigned short) * (size_t)paliashdr->numtris * 3u));
		if (!indexes)
			return;
	}
	else
	{
		indexes = NULL; /* kein Tri -> kein Indexbuffer */
	}

	paliashdr->indexes = (intptr_t)indexes - (intptr_t)paliashdr;
	paliashdr->meshdesc = (intptr_t)desc - (intptr_t)paliashdr;
	paliashdr->numindexes = 0;
	paliashdr->numverts_vbo = 0;

	mark = Hunk_LowMark ();

	/* Remap-Tabelle: pro Quellvertex 2 Slots (front/back seam) */
	{
		const size_t remap_count = (size_t)paliashdr->numverts * 2u;
		const size_t remap_bytes = remap_count * sizeof (remap[0]);
		if (remap_bytes / sizeof (remap[0]) != remap_count)
		{
			Hunk_FreeToLowMark (mark);
			return;
		}
		remap = (unsigned short*)Hunk_Alloc ((int)remap_bytes);
		if (!remap)
		{
			Hunk_FreeToLowMark (mark);
			return;
		}
		memset (remap, 0, remap_bytes);
	}

	/* triangles, stverts sind Engine-Globals (wie im Original) */
	for (i = 0; i < paliashdr->numtris; i++)
	{
		for (j = 0; j < 3; j++)
		{
			/* index into hdr->vertexes */
			const unsigned short vertindex = triangles[i].vertindex[j];

			if (vertindex >= (unsigned short)paliashdr->numverts)
			{
				/* Defensiv: ungültiger Index im Model */
				continue;
			}

			/* index into remap table */
			int v = (int)vertindex * 2;

			/* check for back side */
			if (!triangles[i].facesfront && stverts[vertindex].onseam)
				v++;

			/* emit new vertex if it doesn't already exist */
			if (!remap[v])
			{
				/* basic s/t coords */
				int s = stverts[vertindex].s;
				int t = stverts[vertindex].t;

				/* check for back side and adjust texcoord s */
				if (v & 1)
					s += paliashdr->skinwidth / 2;

				/* Sicherheitscheck gegen Bufferüberlauf (desc wurde für 2*numverts alloziert) */
				if ((unsigned int)paliashdr->numverts_vbo >= (unsigned int)paliashdr->numverts * 2u)
				{
					/* Sollte nie passieren, Abbruch um Korruption zu vermeiden */
					Hunk_FreeToLowMark (mark);
					return;
				}

				desc[paliashdr->numverts_vbo].vertindex = vertindex;
				desc[paliashdr->numverts_vbo].st[0] = s;
				desc[paliashdr->numverts_vbo].st[1] = t;

				remap[v] = (unsigned short)(++paliashdr->numverts_vbo);
			}

			/* emit index */
			if (indexes) /* nur wenn Triangles existieren */
			{
				if ((unsigned int)paliashdr->numindexes >= (unsigned int)paliashdr->numtris * 3u)
				{
					/* Schutz gegen Überschreiben */
					Hunk_FreeToLowMark (mark);
					return;
				}
				indexes[paliashdr->numindexes++] = (unsigned short)(remap[v] - 1);
			}
		}
	}

	/* free temporary data */
	Hunk_FreeToLowMark (mark);

	/* upload immediately */
	GLMesh_LoadVertexBuffer (aliasmodel, paliashdr);
}

/*
================
GLMesh_LoadVertexBuffer

Upload the given alias model's mesh to a VBO

Original code by MH from RMQEngine

Härtung:
- Sichere Größenberechnung mit size_t
- Ausrichtung via align_up
- malloc/NULL-Check, geordnete Fehlerpfade, frühe Abbrüche
- Plausibilitätsprüfungen der Header-Kette
================
*/
void GLMesh_LoadVertexBuffer (qmodel_t* m, aliashdr_t* mainhdr)
{
	size_t totalvbosize = 0;
	size_t animsize = 0;
	const aliasmesh_t* desc;
	const trivertx_t* trivertexes;
	byte* ebodata = NULL;
	byte* vbodata = NULL;
	int f;
	aliashdr_t* hdr;
	unsigned int numindexes = 0, numverts = 0;
	size_t stofs;
	size_t vertofs;
	size_t poseofs;

	if (!m || !mainhdr)
		return;

	if (isDedicated)
		return;

	/* count how much space we're going to need. */
	for (hdr = mainhdr, numverts = 0, numindexes = 0; ; )
	{
		if (!hdr)
			return;

		if (hdr->numverts_vbo < 0 || hdr->numindexes < 0 || hdr->numposes < 0)
			return;

		switch (hdr->poseverttype)
		{
		case PV_QUAKE1:
			totalvbosize += (size_t)hdr->numposes * (size_t)hdr->numverts_vbo * sizeof (meshxyz_t); /* ericw: nummeshframes -> numposes */
			break;
		case PV_IQM:
			totalvbosize += (size_t)hdr->numposes * (size_t)hdr->numverts_vbo * sizeof (iqmvert_t);
			animsize += (size_t)hdr->numboneposes * (size_t)hdr->numbones * sizeof (bonepose_t);
			break;
		default:
			Sys_Error ("Bad vert type %i for %s", hdr->poseverttype, m->name);
			return;
		}

		numverts += (unsigned int)hdr->numverts_vbo;
		numindexes += (unsigned int)hdr->numindexes;

		if (hdr->nextsurface)
			hdr = (aliashdr_t*)((byte*)hdr + hdr->nextsurface);
		else
			break;
	}
	hdr = NULL;

	vertofs = 0;
	totalvbosize = align_up (totalvbosize, (size_t)ssbo_align);

	stofs = totalvbosize;
	if (mainhdr->poseverttype == PV_QUAKE1)
	{
		totalvbosize += (size_t)numverts * sizeof (meshst_t);
	}
	totalvbosize = align_up (totalvbosize, (size_t)ssbo_align);

	poseofs = totalvbosize;
	totalvbosize += animsize;
	totalvbosize = align_up (totalvbosize, (size_t)ssbo_align);

	if (totalvbosize == 0 || numindexes == 0)
		return;

	/* create an elements buffer */
	{
		const size_t ebo_bytes = (size_t)numindexes * sizeof (unsigned short);
		ebodata = (byte*)malloc (ebo_bytes);
		if (!ebodata)
			return; /* fatal */
	}

	/* create the vertex buffer (empty) */
	vbodata = (byte*)malloc (totalvbosize);
	if (!vbodata)
	{
		free (ebodata);
		return; /* fatal */
	}
	memset (vbodata, 0, totalvbosize);

	/* reset for Füllung */
	numindexes = 0;

	for (hdr = mainhdr, numverts = 0, numindexes = 0; ; )
	{
		if (!hdr) break;

		/* grab the pointers to data in the extradata */
		desc = (const aliasmesh_t*)((const byte*)hdr + hdr->meshdesc);
		trivertexes = (const trivertx_t*)((const byte*)hdr + hdr->vertexes);

		/* defensiv */
		if (!desc || !trivertexes)
		{
			hdr = (hdr->nextsurface) ? (aliashdr_t*)((byte*)hdr + hdr->nextsurface) : NULL;
			continue;
		}

		/* submit the index data */
		hdr->eboofs = (unsigned int)((size_t)numindexes * sizeof (unsigned short));
		{
			const size_t copy_bytes = (size_t)hdr->numindexes * sizeof (unsigned short);
			if (copy_bytes)
			{
				const void* src = (const void*)((const byte*)hdr + hdr->indexes);
				if (src)
					memcpy (ebodata + hdr->eboofs, src, copy_bytes);
			}
		}
		numindexes += (unsigned int)hdr->numindexes;

		hdr->vbovertofs = (unsigned int)vertofs;

		/* fill in the vertices at the start of the buffer */
		switch (hdr->poseverttype)
		{
		case PV_QUAKE1:
			for (f = 0; f < hdr->numposes; f++) /* ericw: nummeshframes -> numposes */
			{
				int v;
				meshxyz_t* xyz = (meshxyz_t*)(vbodata + vertofs);
				const trivertx_t* tv = (const trivertx_t*)trivertexes + ((size_t)hdr->numverts * (size_t)f);
				vertofs += (size_t)hdr->numverts_vbo * sizeof (*xyz);

				for (v = 0; v < hdr->numverts_vbo; v++)
				{
					const trivertx_t trivert = tv[desc[v].vertindex];

					xyz[v].xyz[0] = trivert.v[0];
					xyz[v].xyz[1] = trivert.v[1];
					xyz[v].xyz[2] = trivert.v[2];
					xyz[v].xyz[3] = 1;	/* need w 1 for 4 byte vertex compression */

					/* map the normal coordinates in [-1..1] to [-127..127] und als unsigned char speichern */
					xyz[v].normal[0] = (signed char)(127 * r_avertexnormals[trivert.lightnormalindex][0]);
					xyz[v].normal[1] = (signed char)(127 * r_avertexnormals[trivert.lightnormalindex][1]);
					xyz[v].normal[2] = (signed char)(127 * r_avertexnormals[trivert.lightnormalindex][2]);
					xyz[v].normal[3] = 0;	/* unused; for 4-byte alignment */
				}
			}
			break;

		case PV_IQM:
			for (f = 0; f < hdr->numposes; f++) /* ericw: nummeshframes -> numposes */
			{
				int v;
				iqmvert_t* xyz = (iqmvert_t*)(vbodata + vertofs);
				const iqmvert_t* tv = (const iqmvert_t*)trivertexes + ((size_t)hdr->numverts_vbo * (size_t)f);
				vertofs += (size_t)hdr->numverts_vbo * sizeof (*xyz);

				for (v = 0; v < hdr->numverts_vbo; v++, tv++)
					xyz[v] = *tv;
			}

			/* copy bone poses */
			hdr->vboposeofs = (unsigned int)poseofs;
			{
				const size_t pose_bytes = (size_t)hdr->numboneposes * (size_t)hdr->numbones * sizeof (bonepose_t);
				if (pose_bytes)
				{
					const void* src = (const void*)((const byte*)hdr + hdr->boneposedata);
					if (src)
						memcpy (vbodata + hdr->vboposeofs, src, pose_bytes);
					poseofs += pose_bytes;
				}
			}
			break;

		default:
			/* bereits weiter oben abgefangen */
			break;
		}

		/* fill in the ST coords at the end of the buffer (nur Quake1) */
		if (hdr->poseverttype == PV_QUAKE1)
		{
			meshst_t* st;
			float hscale, vscale;

			/* johnfitz -- padded skins */
			hscale = 1.0f / (float)TexMgr_PadConditional (hdr->skinwidth);
			vscale = 1.0f / (float)TexMgr_PadConditional (hdr->skinheight);
			/* johnfitz */

			hdr->vbostofs = (unsigned int)stofs;
			st = (meshst_t*)(vbodata + stofs);
			stofs += (size_t)hdr->numverts_vbo * sizeof (*st);

			for (f = 0; f < hdr->numverts_vbo; f++)
			{
				st[f].st[0] = hscale * ((float)desc[f].st[0] + 0.5f);
				st[f].st[1] = vscale * ((float)desc[f].st[1] + 0.5f);
			}
		}

		if (hdr->nextsurface)
			hdr = (aliashdr_t*)((byte*)hdr + hdr->nextsurface);
		else
			break;
	}
	hdr = NULL;

	/* upload index buffer */
	{
		const size_t ebo_bytes = (size_t)numindexes * sizeof (unsigned short);
		GL_DeleteBuffer (m->meshindexesvbo);
		m->meshindexesvbo = GL_CreateBuffer (GL_ELEMENT_ARRAY_BUFFER, GL_STATIC_DRAW, va ("%s indices", m->name), (int)ebo_bytes, ebodata);
	}

	/* upload vertex buffer */
	{
		GL_DeleteBuffer (m->meshvbo);
		m->meshvbo = GL_CreateBuffer (GL_ARRAY_BUFFER, GL_STATIC_DRAW, va ("%s vertices", m->name), (int)totalvbosize, vbodata);
	}

	free (vbodata);
	free (ebodata);
}

/*
================
GLMesh_LoadVertexBuffers

Loop over all precached alias models, and upload each one to a VBO.

Härtung:
- NULL-/Typ-Checks, defensives Ende der Schleife
================
*/
void GLMesh_LoadVertexBuffers (void)
{
	int j;
	qmodel_t* m;
	aliashdr_t* hdr;

	if (isDedicated)
		return;

	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m) break;
		if (m->type != mod_alias) continue;

		hdr = (aliashdr_t*)Mod_Extradata (m);
		if (!hdr) continue;

		GLMesh_LoadVertexBuffer (m, hdr);
	}
}

/*
================
GLMesh_DeleteVertexBuffers

Delete VBOs for all loaded alias models

Härtung:
- Guards auf Dedicated
- Buffer-Bindings konsistent aufräumen
================
*/
void GLMesh_DeleteVertexBuffers (void)
{
	int j;
	qmodel_t* m;

	if (isDedicated)
		return;

	for (j = 1; j < MAX_MODELS; j++)
	{
		m = cl.model_precache[j];
		if (!m) break;
		if (m->type != mod_alias) continue;

		if (m->meshvbo)
		{
			GL_DeleteBuffersFunc (1, &m->meshvbo);
			m->meshvbo = 0;
		}

		if (m->meshindexesvbo)
		{
			GL_DeleteBuffersFunc (1, &m->meshindexesvbo);
			m->meshindexesvbo = 0;
		}
	}

	GL_ClearBufferBindings ();
}
