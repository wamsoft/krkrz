precision mediump float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform float a_opacity;
void main()
{
  vec4 color = texture2D( s_tex0, v_texCoord );
  color.a *= a_opacity;
  gl_FragColor = color;
}
