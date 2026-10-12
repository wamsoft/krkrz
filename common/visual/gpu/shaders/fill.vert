attribute vec2 a_pos;
attribute vec4 a_color;
uniform mat4 a_modelMat4;
uniform vec2 a_size;
varying vec4 v_color;
void main()
{
  mat4 ortho = mat4(
    vec4( 2.0 / a_size.x, 0.0, 0.0, 0.0 ),
    vec4( 0.0, -2.0 / a_size.y, 0.0, 0.0 ),
    vec4( 0.0, 0.0, -1.0, 0.0 ),
    vec4( -1.0, 1.0, 0.0, 1.0 ) );
  gl_Position = ortho * a_modelMat4 * vec4( a_pos, 0.0, 1.0 );
  v_color = a_color;
}
