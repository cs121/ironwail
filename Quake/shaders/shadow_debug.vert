layout(location=0) out vec2 v_uv;

void main()
{
	vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
	v_uv = pos * 0.5;
	gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
