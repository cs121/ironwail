# Beispiele für den Wechsel von GL-Zeichnung auf `R_SubmitDrawSurface`

Die folgenden Pseudocode-Patches zeigen, wie die bisherigen unmittelbaren OpenGL-Aufrufe
in `r_surf.c` und `r_brush.c` durch das neue Draw-Surface-Submission-Interface ersetzt
werden. Jede Stelle ersetzt die Kombination

```
GL_BindTexture(tex);
GL_DrawSurfaceChain(surf);
```

mit einem `draw_surf_t`, der an `R_SubmitDrawSurface` übergeben wird. Die Beispiele
verwenden Platzhalter (`material`, `mesh_handle`, `model_matrix` usw.) und zeigen nur das
Schema des Umbaus.

## r_surf.c

### 1. Opaque-Chain im Weltpass
```diff
-    GL_BindTexture(tex);
-    GL_DrawSurfaceChain(surf);
+    draw_surf_t ds;
+    ds.material = material;
+    ds.mesh = mesh_handle;
+    memcpy(ds.model_matrix, model_matrix, sizeof(ds.model_matrix));
+    ds.first_index = chain->first_index;
+    ds.index_count = chain->index_count;
+    R_SubmitDrawSurface(&ds);
```

### 2. Lightmapped Geometry
```diff
-    GL_BindTexture(lightmap_tex);
-    GL_DrawSurfaceChain(lightmap_chain);
+    draw_surf_t ds;
+    ds.material = lightmap_material;
+    ds.mesh = lightmap_mesh;
+    Matrix4x4_LoadIdentity(ds.model_matrix);
+    ds.first_index = lightmap_chain->first_index;
+    ds.index_count = lightmap_chain->index_count;
+    R_SubmitDrawSurface(&ds);
```

### 3. Wasserflächen
```diff
-    GL_BindTexture(water_tex);
-    GL_DrawSurfaceChain(watersurf);
+    draw_surf_t ds;
+    ds.material = water_material;
+    ds.mesh = water_mesh;
+    Matrix4x4_ConcatTransforms(ds.model_matrix, base_matrix, scroll_matrix);
+    ds.first_index = watersurf->first_index;
+    ds.index_count = watersurf->index_count;
+    R_SubmitDrawSurface(&ds);
```

## r_brush.c

### 4. Mark-Surfaces für dynamische Lichter
```diff
-    GL_BindTexture(tex);
-    GL_DrawSurfaceChain(chain);
+    draw_surf_t ds;
+    ds.material = mark_material;
+    ds.mesh = mark_mesh;
+    Matrix4x4_Copy(ds.model_matrix, model_matrix);
+    ds.first_index = chain->first_index;
+    ds.index_count = chain->index_count;
+    R_SubmitDrawSurface(&ds);
```

### 5. Transparente Brush-Geometrie
```diff
-    GL_BindTexture(tex);
-    GL_DrawSurfaceChain(trans_chain);
+    draw_surf_t ds;
+    ds.material = trans_material;
+    ds.mesh = trans_mesh;
+    Matrix4x4_Copy(ds.model_matrix, model_matrix);
+    ds.first_index = trans_chain->first_index;
+    ds.index_count = trans_chain->index_count;
+    R_SubmitDrawSurface(&ds);
```
