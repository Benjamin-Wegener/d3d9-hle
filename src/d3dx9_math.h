/*
 * d3dx9_math.h - the D3DX9 math d3dx9.c's shims call: matrices, vectors,
 * quaternions and planes.
 *
 * D3DX's types are single precision and row-major: a matrix is float m[4][4]
 * applied to *row* vectors (v' = v * M), so a translation's components sit in
 * the last column and a LookAt matrix's axes are its rows. Everything is
 * computed in float, as D3DX does, and every function that takes an out
 * pointer also accepts one of its inputs as the out pointer (the result is
 * built locally first).
 */
#ifndef WIN32HLE_D3DX9_MATH_H
#define WIN32HLE_D3DX9_MATH_H

typedef struct { float m[4][4]; } d3dx9_matrix;
typedef struct { float x, y; } d3dx9_vec2;
typedef struct { float x, y, z; } d3dx9_vec3;
typedef struct { float x, y, z, w; } d3dx9_vec4;
typedef struct { float x, y, z, w; } d3dx9_quat;
typedef struct { float a, b, c, d; } d3dx9_plane;
typedef struct { float r, g, b, a; } d3dx9_color;

/* matrices */
void d3dx9_matrix_identity(d3dx9_matrix *m);
void d3dx9_matrix_multiply(d3dx9_matrix *out, const d3dx9_matrix *a, const d3dx9_matrix *b);
int  d3dx9_matrix_inverse(d3dx9_matrix *out, const d3dx9_matrix *m);  /* 0: singular */
void d3dx9_matrix_transpose(d3dx9_matrix *out, const d3dx9_matrix *m);
float d3dx9_matrix_determinant(const d3dx9_matrix *m);
void d3dx9_matrix_scaling(d3dx9_matrix *m, float x, float y, float z);
void d3dx9_matrix_translation(d3dx9_matrix *m, float x, float y, float z);
void d3dx9_matrix_rotation_x(d3dx9_matrix *m, float angle);
void d3dx9_matrix_rotation_y(d3dx9_matrix *m, float angle);
void d3dx9_matrix_rotation_z(d3dx9_matrix *m, float angle);
void d3dx9_matrix_rotation_axis(d3dx9_matrix *m, const d3dx9_vec3 *axis, float angle);
void d3dx9_matrix_rotation_yaw_pitch_roll(d3dx9_matrix *m, float yaw, float pitch, float roll);
void d3dx9_matrix_rotation_quaternion(d3dx9_matrix *m, const d3dx9_quat *q);
void d3dx9_matrix_reflect(d3dx9_matrix *m, const d3dx9_plane *p);
void d3dx9_matrix_shadow(d3dx9_matrix *m, const d3dx9_vec4 *light, const d3dx9_plane *plane);

/* 2D vectors */
void d3dx9_vec2_normalize(d3dx9_vec2 *out, const d3dx9_vec2 *v);
float d3dx9_vec2_length(const d3dx9_vec2 *v);
float d3dx9_vec2_length_sq(const d3dx9_vec2 *v);
float d3dx9_vec2_dot(const d3dx9_vec2 *a, const d3dx9_vec2 *b);
float d3dx9_vec2_ccw(const d3dx9_vec2 *a, const d3dx9_vec2 *b);
void d3dx9_vec2_add(d3dx9_vec2 *out, const d3dx9_vec2 *a, const d3dx9_vec2 *b);
void d3dx9_vec2_subtract(d3dx9_vec2 *out, const d3dx9_vec2 *a, const d3dx9_vec2 *b);
void d3dx9_vec2_scale(d3dx9_vec2 *out, const d3dx9_vec2 *v, float s);
void d3dx9_vec2_lerp(d3dx9_vec2 *out, const d3dx9_vec2 *a, const d3dx9_vec2 *b, float s);
void d3dx9_vec2_minimize(d3dx9_vec2 *out, const d3dx9_vec2 *a, const d3dx9_vec2 *b);
void d3dx9_vec2_maximize(d3dx9_vec2 *out, const d3dx9_vec2 *a, const d3dx9_vec2 *b);
void d3dx9_vec2_transform(d3dx9_vec4 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m);
void d3dx9_vec2_transform_coord(d3dx9_vec2 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m);
void d3dx9_vec2_transform_normal(d3dx9_vec2 *out, const d3dx9_vec2 *v, const d3dx9_matrix *m);
void d3dx9_vec2_transform_coord_array(d3dx9_vec2 *out, unsigned out_stride,
                                      const d3dx9_vec2 *in, unsigned in_stride,
                                      const d3dx9_matrix *m, unsigned n);
void d3dx9_vec2_transform_normal_array(d3dx9_vec2 *out, unsigned out_stride,
                                       const d3dx9_vec2 *in, unsigned in_stride,
                                       const d3dx9_matrix *m, unsigned n);

/* 3D vectors */
void d3dx9_vec3_normalize(d3dx9_vec3 *out, const d3dx9_vec3 *v);
float d3dx9_vec3_length(const d3dx9_vec3 *v);
float d3dx9_vec3_length_sq(const d3dx9_vec3 *v);
float d3dx9_vec3_dot(const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_cross(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_add(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_subtract(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_scale(d3dx9_vec3 *out, const d3dx9_vec3 *v, float s);
void d3dx9_vec3_lerp(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b, float s);
void d3dx9_vec3_minimize(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_maximize(d3dx9_vec3 *out, const d3dx9_vec3 *a, const d3dx9_vec3 *b);
void d3dx9_vec3_transform(d3dx9_vec4 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m);
void d3dx9_vec3_transform_coord(d3dx9_vec3 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m);
void d3dx9_vec3_transform_normal(d3dx9_vec3 *out, const d3dx9_vec3 *v, const d3dx9_matrix *m);
void d3dx9_vec3_transform_coord_array(d3dx9_vec3 *out, unsigned out_stride,
                                      const d3dx9_vec3 *in, unsigned in_stride,
                                      const d3dx9_matrix *m, unsigned n);
void d3dx9_vec3_transform_normal_array(d3dx9_vec3 *out, unsigned out_stride,
                                       const d3dx9_vec3 *in, unsigned in_stride,
                                       const d3dx9_matrix *m, unsigned n);

/* 4D vectors */
void d3dx9_vec4_normalize(d3dx9_vec4 *out, const d3dx9_vec4 *v);
float d3dx9_vec4_length(const d3dx9_vec4 *v);
float d3dx9_vec4_length_sq(const d3dx9_vec4 *v);
float d3dx9_vec4_dot(const d3dx9_vec4 *a, const d3dx9_vec4 *b);
void d3dx9_vec4_add(d3dx9_vec4 *out, const d3dx9_vec4 *a, const d3dx9_vec4 *b);
void d3dx9_vec4_subtract(d3dx9_vec4 *out, const d3dx9_vec4 *a, const d3dx9_vec4 *b);
void d3dx9_vec4_scale(d3dx9_vec4 *out, const d3dx9_vec4 *v, float s);
void d3dx9_vec4_lerp(d3dx9_vec4 *out, const d3dx9_vec4 *a, const d3dx9_vec4 *b, float s);
void d3dx9_vec4_minimize(d3dx9_vec4 *out, const d3dx9_vec4 *a, const d3dx9_vec4 *b);
void d3dx9_vec4_maximize(d3dx9_vec4 *out, const d3dx9_vec4 *a, const d3dx9_vec4 *b);
void d3dx9_vec4_transform(d3dx9_vec4 *out, const d3dx9_vec4 *v, const d3dx9_matrix *m);
void d3dx9_vec4_cross(d3dx9_vec4 *out, const d3dx9_vec4 *v1, const d3dx9_vec4 *v2, const d3dx9_vec4 *v3);
void d3dx9_vec4_transform_array(d3dx9_vec4 *out, unsigned out_stride,
                                const d3dx9_vec4 *in, unsigned in_stride,
                                const d3dx9_matrix *m, unsigned n);

/* View and projection matrices (public D3DX9 docs; row vectors). */
void d3dx9_matrix_look_at_lh(d3dx9_matrix *m, const d3dx9_vec3 *eye, const d3dx9_vec3 *at, const d3dx9_vec3 *up);
void d3dx9_matrix_look_at_rh(d3dx9_matrix *m, const d3dx9_vec3 *eye, const d3dx9_vec3 *at, const d3dx9_vec3 *up);
void d3dx9_matrix_perspective_lh(d3dx9_matrix *m, float w, float h, float zn, float zf);
void d3dx9_matrix_perspective_rh(d3dx9_matrix *m, float w, float h, float zn, float zf);
void d3dx9_matrix_perspective_fov_lh(d3dx9_matrix *m, float fovy, float aspect, float zn, float zf);
void d3dx9_matrix_perspective_fov_rh(d3dx9_matrix *m, float fovy, float aspect, float zn, float zf);
void d3dx9_matrix_perspective_off_center_lh(d3dx9_matrix *m, float l, float r, float b, float t, float zn, float zf);
void d3dx9_matrix_perspective_off_center_rh(d3dx9_matrix *m, float l, float r, float b, float t, float zn, float zf);
void d3dx9_matrix_ortho_lh(d3dx9_matrix *m, float w, float h, float zn, float zf);
void d3dx9_matrix_ortho_rh(d3dx9_matrix *m, float w, float h, float zn, float zf);
void d3dx9_matrix_ortho_off_center_lh(d3dx9_matrix *m, float l, float r, float b, float t, float zn, float zf);
void d3dx9_matrix_ortho_off_center_rh(d3dx9_matrix *m, float l, float r, float b, float t, float zn, float zf);

/* Quaternions */
void d3dx9_quat_identity(d3dx9_quat *q);
void d3dx9_quat_multiply(d3dx9_quat *out, const d3dx9_quat *q1, const d3dx9_quat *q2);
void d3dx9_quat_normalize(d3dx9_quat *out, const d3dx9_quat *q);
void d3dx9_quat_conjugate(d3dx9_quat *out, const d3dx9_quat *q);
d3dx9_quat *d3dx9_quat_inverse(d3dx9_quat *out, const d3dx9_quat *q); /* NULL on singular */
float d3dx9_quat_length(const d3dx9_quat *q);
float d3dx9_quat_length_sq(const d3dx9_quat *q);
float d3dx9_quat_dot(const d3dx9_quat *a, const d3dx9_quat *b);
void d3dx9_quat_rotation_axis(d3dx9_quat *out, const d3dx9_vec3 *axis, float angle);
void d3dx9_quat_rotation_matrix(d3dx9_quat *out, const d3dx9_matrix *m);
void d3dx9_quat_rotation_yaw_pitch_roll(d3dx9_quat *out, float yaw, float pitch, float roll);
void d3dx9_quat_to_axis_angle(const d3dx9_quat *q, d3dx9_vec3 *axis, float *angle);
void d3dx9_quat_slerp(d3dx9_quat *out, const d3dx9_quat *q1, const d3dx9_quat *q2, float t);
void d3dx9_quat_ln(d3dx9_quat *out, const d3dx9_quat *q);
void d3dx9_quat_exp(d3dx9_quat *out, const d3dx9_quat *q);

/* Planes */
void d3dx9_plane_from_point_normal(d3dx9_plane *p, const d3dx9_vec3 *point, const d3dx9_vec3 *normal);
void d3dx9_plane_from_points(d3dx9_plane *p, const d3dx9_vec3 *v1, const d3dx9_vec3 *v2, const d3dx9_vec3 *v3);
void d3dx9_plane_normalize(d3dx9_plane *out, const d3dx9_plane *p);
float d3dx9_plane_dot(const d3dx9_plane *p, const d3dx9_vec4 *v);
float d3dx9_plane_dot_coord(const d3dx9_plane *p, const d3dx9_vec3 *v);
float d3dx9_plane_dot_normal(const d3dx9_plane *p, const d3dx9_vec3 *v);
void d3dx9_plane_scale(d3dx9_plane *out, const d3dx9_plane *p, float s);
d3dx9_plane *d3dx9_plane_transform(d3dx9_plane *out, const d3dx9_plane *p, const d3dx9_matrix *m); /* NULL on singular */

/* Colours (public D3DX9 docs; alpha is copied or combined as noted) */
void d3dx9_color_add(d3dx9_color *out, const d3dx9_color *c1, const d3dx9_color *c2);
void d3dx9_color_subtract(d3dx9_color *out, const d3dx9_color *c1, const d3dx9_color *c2);
void d3dx9_color_modulate(d3dx9_color *out, const d3dx9_color *c1, const d3dx9_color *c2);
void d3dx9_color_scale(d3dx9_color *out, const d3dx9_color *c, float s);
void d3dx9_color_negative(d3dx9_color *out, const d3dx9_color *c);
void d3dx9_color_lerp(d3dx9_color *out, const d3dx9_color *c1, const d3dx9_color *c2, float s);
void d3dx9_color_adjust_contrast(d3dx9_color *out, const d3dx9_color *c, float contrast);
void d3dx9_color_adjust_saturation(d3dx9_color *out, const d3dx9_color *c, float saturation);

#endif /* WIN32HLE_D3DX9_MATH_H */
