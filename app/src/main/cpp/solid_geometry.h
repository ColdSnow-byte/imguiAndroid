#pragma once

#include <vector>

/** The five Platonic solids. */
enum class SolidType
{
    Tetrahedron = 0,   //  4 triangular faces
    Cube,              //  6 square faces
    Octahedron,        //  8 triangular faces
    Dodecahedron,      // 12 pentagonal faces
    Icosahedron,       // 20 triangular faces
    Count
};

const char* SolidDisplayName(SolidType type);

/**
 * Builds the render geometry of a solid.
 *
 * Output is a non-indexed triangle list, 9 floats per vertex:
 * position(3) + normal(3) + color(3).
 *
 * Colour rule (identical for every solid): every rasterized triangle carries
 * three clearly different colours on its three corners - hues 120 degrees
 * apart on the wheel - so each corner fades into the other two. Polygonal
 * faces (square / pentagon) are fanned from the face centre, which takes the
 * third hue. Faces are flat shaded (each vertex owns the face normal).
 */
bool BuildSolidMesh(SolidType type, std::vector<float>& outVertices, int& outFaceCount);

// ---------------------------------------------------------------------------
// Column-major 4x4 matrices (shared by the GPU and the CPU renderer)
// ---------------------------------------------------------------------------
struct Mat4
{
    float m[16];
};

Mat4 Mat4Identity();
Mat4 Mat4Multiply(const Mat4& a, const Mat4& b);
Mat4 Mat4Translate(float x, float y, float z);
Mat4 Mat4RotateX(float a);
Mat4 Mat4RotateY(float a);
Mat4 Mat4RotateZ(float a);
Mat4 Mat4Perspective(float fovYRadians, float aspect, float nearZ, float farZ);

/** Tumbling rotation: three different angular speeds around X / Y / Z. */
Mat4 SolidTumbleMatrix(float timeSeconds);

/** Shared camera setup: eye at (0, 0, 3.2) looking down -Z, 45 deg FOV. */
Mat4 SolidViewMatrix();
Mat4 SolidProjection();

/** Transforms a point (w is written) with a column-major matrix. */
void Mat4TransformPoint(const Mat4& m, float x, float y, float z, float out[4]);

/** Transforms a direction (ignores translation). */
void Mat4TransformDirection(const Mat4& m, float x, float y, float z, float out[3]);
