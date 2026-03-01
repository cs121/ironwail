#lava1
{
    qer_editorimage enter01

    // 1) Base
    {
        map textures/#lava1
        tcmod scale .2 .2
	tcmod scroll .04 .03
        tcMod turb 0 .1 0 .01
        blendFunc GL_ONE GL_ZERO
	rgbGen wave sin 0.90 0.10 0 1.2
    }		
}