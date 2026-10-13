precision highp float;
varying vec2 v_texCoord;
uniform sampler2D s_tex0;
uniform float u_count;
uniform float u_ops[32];
uniform vec4 u_params[32];
uniform float u_seed;
uniform vec2 u_res;

float hash12( vec2 p )
{
  vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
  p3 += dot( p3, p3.yzx + 33.33 );
  return fract( ( p3.x + p3.y ) * p3.z );
}

float hue2rgb( float m1, float m2, float h )
{
  if( h < 0.0 ) h += 1.0;
  if( h > 1.0 ) h -= 1.0;
  if( h < 1.0/6.0 ) return m1 + (m2-m1)*6.0*h;
  if( h < 1.0/2.0 ) return m2;
  if( h < 2.0/3.0 ) return m1 + (m2-m1)*(2.0/3.0-h)*6.0;
  return m1;
}
vec3 hslToRgb( float h, float s, float l )
{
  if( s <= 0.0 ) return vec3( l );
  float m2 = ( l <= 0.5 ) ? l*(1.0+s) : l+s-l*s;
  float m1 = 2.0*l - m2;
  return vec3( hue2rgb(m1,m2,h+1.0/3.0), hue2rgb(m1,m2,h), hue2rgb(m1,m2,h-1.0/3.0) );
}
vec3 rgbToHsl( vec3 c )
{
  float cMax = max( max(c.r,c.g), c.b );
  float cMin = min( min(c.r,c.g), c.b );
  float delta = cMax - cMin;
  float add = cMax + cMin;
  float l = add*0.5; float h = 0.0; float sat = 0.0;
  if( delta > 0.0 ) {
    sat = ( l < 0.5 ) ? delta/add : delta/(2.0-add);
    if( c.r == cMax ) h = (c.g-c.b)/delta;
    else if( c.g == cMax ) h = 2.0 + (c.b-c.r)/delta;
    else h = 4.0 + (c.r-c.g)/delta;
    h /= 6.0; if( h < 0.0 ) h += 1.0;
  }
  return vec3( h, sat, l );
}

vec3 bScreen( vec3 d, vec3 s ) { return vec3(1.0) - (vec3(1.0)-d)*(vec3(1.0)-s); }
float bOverlay1( float a, float b ) { return ( a < 0.5 ) ? a*b*2.0 : 1.0 - (1.0-a)*(1.0-b)*2.0; }
vec3 bOverlay( vec3 d, vec3 s ) { return vec3( bOverlay1(d.r,s.r), bOverlay1(d.g,s.g), bOverlay1(d.b,s.b) ); }
float bHard1( float a, float b ) { return ( b < 0.5 ) ? a*b*2.0 : 1.0 - (1.0-a)*(1.0-b)*2.0; }
vec3 bHard( vec3 d, vec3 s ) { return vec3( bHard1(d.r,s.r), bHard1(d.g,s.g), bHard1(d.b,s.b) ); }
float bSoft1( float a, float b ) { return ( b < 0.5 ) ? (2.0*a*b + a*a*(1.0-2.0*b)) : (sqrt(a)*(2.0*b-1.0) + 2.0*a*(1.0-b)); }
vec3 bSoft( vec3 d, vec3 s ) { return vec3( bSoft1(d.r,s.r), bSoft1(d.g,s.g), bSoft1(d.b,s.b) ); }

// dest に定数色 s を alpha a・合成モード mode (tTVPLayerType の値) で重ねる
vec3 applyOver( vec3 d, vec3 s, float a, int mode )
{
  if( mode == 1 ) return s * a;
  else if( mode == 3 ) return min( vec3(1.0), d + s );
  else if( mode == 4 ) return max( vec3(0.0), d + s - vec3(1.0) );
  else if( mode == 5 ) return d * s;
  else if( mode == 8 ) return min( vec3(1.0), d / (vec3(1.0) - s) );
  else if( mode == 9 ) return min( d, s );
  else if( mode == 10 ) return max( d, s );
  else if( mode == 11 ) return bScreen( d, s );
  else if( mode == 12 ) return min( vec3(1.0), d*(1.0-a) + s );
  else if( mode == 14 ) return mix( d, min(vec3(1.0), d+s), a );
  else if( mode == 15 ) return mix( d, max(vec3(0.0), d+s-vec3(1.0)), a );
  else if( mode == 16 ) return mix( d, d*s, a );
  else if( mode == 17 ) return mix( d, bScreen(d,s), a );
  else if( mode == 18 ) return mix( d, bOverlay(d,s), a );
  else if( mode == 19 ) return mix( d, bHard(d,s), a );
  else if( mode == 20 ) return mix( d, bSoft(d,s), a );
  else if( mode == 21 ) return mix( d, min(vec3(1.0), d/(vec3(1.0)-s)), a );
  else if( mode == 22 ) return min( vec3(1.0), d / (vec3(1.0) - s*a) );
  else if( mode == 23 ) return mix( d, max(vec3(0.0), vec3(1.0)-(vec3(1.0)-d)/s), a );
  else if( mode == 24 ) return mix( d, max(d,s), a );
  else if( mode == 25 ) return mix( d, min(d,s), a );
  else if( mode == 26 ) return mix( d, abs(d-s), a );
  else if( mode == 27 ) return abs( d - s*a );
  else if( mode == 28 ) return mix( d, d + s - 2.0*s*d, a );
  return mix( d, s, a );
}

// 混合系の点処理 (GL 版 GLEffect.cpp の buildPointwiseFs と同じ演算)。op の値は tTVPEffectOp
void main()
{
  vec4 c = texture2D( s_tex0, v_texCoord );
  vec2 px = v_texCoord * u_res;
  for( int i = 0; i < 32; i++ ) {
    if( float(i) >= u_count ) break;
    int op = int( u_ops[i] + 0.5 );
    vec4 p = u_params[i];
    if( op >= 100 ) {
      c.rgb = applyOver( c.rgb, p.rgb, p.a, op - 100 );
    } else if( op == 1 ) {
      vec3 s255 = c.rgb * 255.0;
      float y = floor( (s255.b*19.0 + s255.g*183.0 + s255.r*54.0) / 256.0 );
      c.rgb = vec3( y / 255.0 );
    } else if( op == 2 ) {
      vec3 hsl = rgbToHsl( c.rgb );
      vec3 rgb = hslToRgb( p.x, p.y, hsl.z );
      c.rgb = mix( c.rgb, rgb, p.z );
    } else if( op == 3 ) {
      vec3 hsl = rgbToHsl( c.rgb );
      float h = fract( hsl.x + p.x );
      float ss = hsl.y; float ll = hsl.z;
      ss += ( p.y > 0.0 ) ? (1.0-ss)*p.y : ss*p.y;
      ll += ( p.z > 0.0 ) ? (1.0-ll)*p.z : ll*p.z;
      c.rgb = hslToRgb( h, ss, ll );
    } else if( op == 4 ) {
      float lv = p.x / 255.0;
      float n0 = ( hash12( px + vec2(u_seed, 1.0) ) - 0.5 ) * lv;
      float n1 = ( hash12( px + vec2(7.0, u_seed) ) - 0.5 ) * lv;
      float n2 = ( hash12( px + vec2(u_seed, u_seed+3.0) ) - 0.5 ) * lv;
      c.rgb = clamp( c.rgb + vec3(n0,n1,n2), 0.0, 1.0 );
    } else if( op == 5 ) {
      float nn = floor( hash12( px + vec2(u_seed, u_seed) ) * 255.0 ) / 255.0;
      c.rgb = vec3( nn );
    }
  }
  gl_FragColor = c;
}
