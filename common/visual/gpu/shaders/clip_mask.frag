precision mediump float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform sampler2D s_tex1;
uniform vec2 a_res;
uniform vec4 a_maskRect;
uniform vec2 a_maskUV;
uniform float a_useMask;
void main()
{
  vec4 color = texture2D( s_tex0, v_texCoord );
  if( a_useMask != 0.0 ) {
    vec2 m = ( v_texCoord * a_res - a_maskRect.xy ) / a_maskRect.zw;
    float a = 0.0;
    if( m.x >= 0.0 && m.x <= 1.0 && m.y >= 0.0 && m.y <= 1.0 ) {
      a = texture2D( s_tex1, m * a_maskUV ).a;
    }
    color.a *= a;
  }
  gl_FragColor = color;
}
