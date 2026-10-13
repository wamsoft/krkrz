precision highp float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform sampler2D s_tex1;   // 256x1 のチャンネルごとの表
float idx( float v ) { return ( v*255.0 + 0.5 ) / 256.0; }
void main()
{
  vec4 c = texture2D( s_tex0, v_texCoord );
  float r = texture2D( s_tex1, vec2( idx(c.r), 0.5 ) ).r;
  float g = texture2D( s_tex1, vec2( idx(c.g), 0.5 ) ).g;
  float b = texture2D( s_tex1, vec2( idx(c.b), 0.5 ) ).b;
  gl_FragColor = vec4( r, g, b, c.a );
}
