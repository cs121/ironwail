#include "Quake/renderer/r_iwshader.h"

#include <assert.h>
#include <math.h>
#include <string.h>

static void expect_float(float actual, float expected)
{
    assert(fabsf(actual - expected) < 1e-5f);
}

int main(void)
{
    static const char *shader_text =
        "shader tests/dp_material\n"
        "{\n"
        "    q3map_sun 1 0.5 0.25 100 45 30\n"
        "    deformVertexes wave sin 1 0 0 1\n"
        "    tessSize 32\n"
        "    portal\n"
        "    fog textures/fogs/testfog\n"
        "    fogparms ( 0.2 0.3 0.4 ) 128\n"
        "    skyparms env/test 512 env/test_near\n"
        "\n"
        "    stage {\n"
        "        clampmap textures/dp/diffuse.tga\n"
        "        tcgen vector ( 1 0 0 ) ( 0 1 0 )\n"
        "        depthfunc equal\n"
        "        alphafunc gequal\n"
        "        normalmap textures/dp/normal.tga\n"
        "        glossmap textures/dp/gloss.tga\n"
        "        specular 0.75\n"
        "        if dp_extensions\n"
        "        fogparms ( 0.1 0.2 0.3 ) 512 0.5\n"
        "    }\n"
        "\n"
        "    stage {\n"
        "        map $white\n"
        "    }\n"
        "\n"
        "    stage {\n"
        "        cubeMap env/dp/sky\n"
        "    }\n"
        "}\n";

    iwMaterial_t material;
    assert(IW_ParseShaderTextForTesting(shader_text, &material));

    assert(material.numQ3MapDirectives == 1);
    assert(strcmp(material.q3mapDirectives[0], "q3map_sun 1 0.5 0.25 100 45 30") == 0);

    assert(material.numDeformVertexes == 1);
    assert(strcmp(material.deformVertexes[0], "deformVertexes wave sin 1 0 0 1") == 0);

    assert(material.hasTessSize);
    expect_float(material.tessSize, 32.0f);

    assert(material.portal == 1);
    assert(material.hasFog);
    assert(strcmp(material.fogShader, "textures/fogs/testfog") == 0);

    assert(material.hasFogParms);
    assert(material.fogParms.hasColor);
    expect_float(material.fogParms.color[0], 0.2f);
    expect_float(material.fogParms.color[1], 0.3f);
    expect_float(material.fogParms.color[2], 0.4f);
    assert(material.fogParms.hasDepthForOpaque);
    expect_float(material.fogParms.depthForOpaque, 128.0f);

    assert(material.hasSkyParms);
    assert(strcmp(material.skyParms[0], "env/test") == 0);
    assert(strcmp(material.skyParms[1], "512") == 0);
    assert(strcmp(material.skyParms[2], "env/test_near") == 0);

    assert(material.numStages == 3);

    const iwStage_t *stage0 = &material.stages[0];
    assert(stage0->mapType == IW_MAP_CLAMP2D);
    assert(stage0->isClamp);
    assert(strcmp(stage0->mapPath, "textures/dp/diffuse.tga") == 0);
    assert(strcmp(stage0->tcGen, "vector ( 1 0 0 ) ( 0 1 0 )") == 0);
    assert(stage0->tcGenMode == IW_TCGEN_VECTOR);
    expect_float(stage0->tcGenVectors[0][0], 1.0f);
    expect_float(stage0->tcGenVectors[0][1], 0.0f);
    expect_float(stage0->tcGenVectors[0][2], 0.0f);
    expect_float(stage0->tcGenVectors[1][0], 0.0f);
    expect_float(stage0->tcGenVectors[1][1], 1.0f);
    expect_float(stage0->tcGenVectors[1][2], 0.0f);
    assert(strcmp(stage0->depthFunc, "equal") == 0);
    assert(strcmp(stage0->alphaFunc, "gequal") == 0);
    assert(strcmp(stage0->normalMap, "textures/dp/normal.tga") == 0);
    assert(strcmp(stage0->glossMap, "textures/dp/gloss.tga") == 0);
    expect_float(stage0->specularScale, 0.75f);
    assert(stage0->stageCondition[0] != '\0');
    assert(strcmp(stage0->stageCondition, "dp_extensions") == 0);
    assert(stage0->hasFogParms);
    expect_float(stage0->fogParms.color[0], 0.1f);
    expect_float(stage0->fogParms.color[1], 0.2f);
    expect_float(stage0->fogParms.color[2], 0.3f);
    assert(stage0->fogParms.hasDepthForOpaque);
    expect_float(stage0->fogParms.depthForOpaque, 512.0f);
    assert(stage0->fogParms.hasDensity);
    expect_float(stage0->fogParms.density, 0.5f);

    const iwStage_t *stage1 = &material.stages[1];
    assert(stage1->mapType == IW_MAP_PROCEDURAL_WHITE);
    assert(strcmp(stage1->mapPath, "$white") == 0);

    const iwStage_t *stage2 = &material.stages[2];
    assert(stage2->mapType == IW_MAP_CUBEMAP);
    assert(strcmp(stage2->mapPath, "env/dp/sky") == 0);

    return 0;
}
