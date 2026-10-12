precision mediump float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform sampler2D s_tex1;
uniform float a_phase;
uniform float a_opacity;
void main()
{
  vec4 s1 = texture2D( s_tex0, v_texCoord );
  vec4 s2 = texture2D( s_tex1, v_texCoord );
  float opa = clamp( a_phase, 0.0, 1.0 );
  float a1 = s1.a;
  float a2 = s2.a;
  float a = a1 * (1.0 - opa);
  float alpha = 1.0;
  if( a != 0.0 ) {
    float b = a2 * opa;
    alpha = b / a;
    alpha = alpha / (1.0 - b + alpha);
    alpha = clamp( alpha, 0.0, 1.0 );
  }
  float ar = a1 + (a2 - a1) * opa;
  gl_FragColor = vec4( mix( s1.rgb, s2.rgb, alpha ), ar * a_opacity );
}
