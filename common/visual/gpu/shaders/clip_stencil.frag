precision mediump float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform float a_threshold;
void main()
{
  if( texture2D( s_tex0, v_texCoord ).a < a_threshold ) discard;
  gl_FragColor = vec4( 0.0 );
}
