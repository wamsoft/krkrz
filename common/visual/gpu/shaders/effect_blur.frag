precision highp float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform vec2 u_dir;
uniform float u_radius;
uniform float u_weights[129];
// 重み付き分離ブラーの 1 方向 (box / gaussian 共通。半径は最大 64)
void main()
{
  vec4 sum = vec4( 0.0 );
  int radius = int( u_radius + 0.5 );
  for( int i = -64; i <= 64; i++ ) {
    if( i < -radius || i > radius ) continue;
    sum += texture2D( s_tex0, v_texCoord + u_dir * float(i) ) * u_weights[i + radius];
  }
  gl_FragColor = sum;
}
