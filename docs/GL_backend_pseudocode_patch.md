# Ironwail Rendering Backend Migration (Pseudocode Patch)

Dieser Patch beschreibt in Pseudocode, wie die Renderer-Initialisierung und die Ersetzung direkter OpenGL-Aufrufe auf das neue Backend-Interface angepasst werden. Die Beispiele zeigen nur die ersten typischen Stellen in den genannten Dateien.

## 1. Renderer-Init anpassen

```c
// bisher: Renderer-Backend wird nicht aktiv abgefragt
void R_Init(void)
{
    ...
    // TODO: Backend setzen
}

// neu: Backend-Handle bei der Initialisierung holen
void R_Init(void)
{
    ...
    rb = GL_GetBackend();
    // rb speichert Funktionszeiger, z. B. rb->BindTexture(), rb->DrawSurface()
}
```

## 2. OpenGL-Aufrufe durch Backend-Wrapper ersetzen
Direkte GL-Aufrufe in r_*.c bleiben logisch erhalten, werden aber über das Backend-Interface aufgerufen.

### Beispiele (erste 5 typische Stellen)

#### r_surf.c
```c
// 1. glBindTexture(GL_TEXTURE_2D, tex->gl_id);
rb->BindTexture(tex);

// 2. glTexCoord2f(s, t);
rb->TexCoord2f(s, t);

// 3. glVertex3fv(v);
rb->Vertex3fv(v);

// 4. glBegin(GL_QUADS);
rb->Begin(GL_QUADS);

// 5. glEnd();
rb->End();
```

#### r_alias.c
```c
// 1. glBindTexture(GL_TEXTURE_2D, skin->gl_id);
rb->BindTexture(skin);

// 2. glDisable(GL_ALPHA_TEST);
rb->Disable(GL_ALPHA_TEST);

// 3. glEnable(GL_BLEND);
rb->Enable(GL_BLEND);

// 4. glDepthMask(GL_FALSE);
rb->DepthMask(false);

// 5. glDrawArrays(GL_TRIANGLES, 0, count);
rb->DrawArrays(GL_TRIANGLES, 0, count);
```

#### gl_model.c
```c
// 1. glGenTextures(1, &tex->gl_id);
rb->GenTextures(1, &tex->gl_id);

// 2. glBindTexture(GL_TEXTURE_2D, tex->gl_id);
rb->BindTexture(tex);

// 3. glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
rb->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

// 4. glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
rb->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

// 5. glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
rb->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
```
