dev/q3/tcmod_scroll_rotate_turb
{
	cull back
	{
		map textures/dev/grid
		rgbGen identity
		tcMod scroll 0.2 0.0
		tcMod rotate 25
		tcMod turb 0 0.05 0 0.8
	}
}

dev/q3/animmap
{
	surfaceparm trans
	{
		animMap 2 textures/dev/frame0 textures/dev/frame1 textures/dev/frame2
		blendFunc blend
		rgbGen identity
	}
}

dev/q3/clampmap
{
	{
		clampMap textures/dev/clamp_border
		rgbGen identity
	}
}

dev/q3/env
{
	cull disable
	{
		map textures/dev/metal
		tcGen environment
		rgbGen identity
	}
}

dev/q3/trans_blend
{
	surfaceparm trans
	{
		map textures/dev/glass
		blendFunc blend
		rgbGen identity
		alphaGen wave sin 0.5 0.5 0 1
	}
}

dev/q3/fogparms_water
{
	surfaceparm water
	fogparms ( 0.1 0.25 0.35 ) 256
	{
		map textures/dev/water
		blendFunc filter
		rgbGen identity
	}
}
