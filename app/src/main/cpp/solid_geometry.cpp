#include "solid_geometry.h"

#include <algorithm>
#include <cmath>

namespace
{

const float PHI     = (1.0f + sqrtf(5.0f)) * 0.5f;
const float INV_PHI = 1.0f / PHI;
const float EPS     = 1.0e-4f;

struct Vec3
{
    float x, y, z;
};

Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

float Length(Vec3 v) { return sqrtf(Dot(v, v)); }

Vec3 Normalize(Vec3 v)
{
    const float l = Length(v);
    return l > 0.0f ? v * (1.0f / l) : Vec3{ 0.0f, 0.0f, 0.0f };
}

void HsvToRgb(float h, float s, float v, float* out)
{
    h -= floorf(h);
    const int   i = (int)(h * 6.0f);
    const float f = h * 6.0f - (float)i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (i % 6)
    {
        case 0: out[0] = v; out[1] = t; out[2] = p; break;
        case 1: out[0] = q; out[1] = v; out[2] = p; break;
        case 2: out[0] = p; out[1] = v; out[2] = t; break;
        case 3: out[0] = p; out[1] = q; out[2] = v; break;
        case 4: out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

// --------------------------------------------------------------------------
// Vertices
// --------------------------------------------------------------------------
std::vector<Vec3> BaseVertices(SolidType type)
{
    switch (type)
    {
    case SolidType::Tetrahedron:
        return { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };

    case SolidType::Cube:
        return { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
                 { -1, -1,  1 }, { 1, -1,  1 }, { 1, 1,  1 }, { -1, 1,  1 } };

    case SolidType::Octahedron:
        return { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

    case SolidType::Dodecahedron:
    {
        std::vector<Vec3> v;
        for (int a = -1; a <= 1; a += 2)
            for (int b = -1; b <= 1; b += 2)
                for (int c = -1; c <= 1; c += 2)
                    v.push_back({ (float)a, (float)b, (float)c });
        for (int a = -1; a <= 1; a += 2)
            for (int b = -1; b <= 1; b += 2)
            {
                v.push_back({ 0.0f, (float)a * INV_PHI, (float)b * PHI });
                v.push_back({ (float)a * INV_PHI, (float)b * PHI, 0.0f });
                v.push_back({ (float)a * PHI, 0.0f, (float)b * INV_PHI });
            }
        return v;
    }

    case SolidType::Icosahedron:
    {
        std::vector<Vec3> v;
        for (int a = -1; a <= 1; a += 2)
            for (int b = -1; b <= 1; b += 2)
            {
                v.push_back({ 0.0f, (float)a, (float)b * PHI });
                v.push_back({ (float)a, (float)b * PHI, 0.0f });
                v.push_back({ (float)a * PHI, 0.0f, (float)b });
            }
        return v;
    }

    default:
        return {};
    }
}

// --------------------------------------------------------------------------
// Convex hull
// --------------------------------------------------------------------------
struct Face
{
    Vec3              normal;
    float             d = 0.0f;
    std::vector<Vec3> poly;
};

/**
 * Brute-force convex hull: a plane through three vertices is a face when every
 * other vertex lies on one side of it. Works for any of the five solids and
 * avoids hand-written (error prone) face lists - in particular the 12
 * pentagons of the dodecahedron.
 */
std::vector<Face> BuildFaces(const std::vector<Vec3>& verts)
{
    std::vector<Face> faces;
    const int n = (int)verts.size();

    for (int i = 0; i < n - 2; i++)
        for (int j = i + 1; j < n - 1; j++)
            for (int k = j + 1; k < n; k++)
            {
                const Vec3 nrm = Normalize(Cross(verts[j] - verts[i], verts[k] - verts[i]));
                if (Length(nrm) < 0.5f)
                    continue; // degenerate triple

                const float d0 = Dot(nrm, verts[i]);
                float mn = d0;
                float mx = d0;
                for (const Vec3& p : verts)
                {
                    const float t = Dot(nrm, p);
                    mn = std::min(mn, t);
                    mx = std::max(mx, t);
                }

                Vec3  outward;
                float d;
                if (fabsf(mx - d0) < EPS)
                {
                    outward = nrm;
                    d = mx;
                }
                else if (fabsf(d0 - mn) < EPS)
                {
                    outward = nrm * -1.0f;
                    d = -mn;
                }
                else
                {
                    continue; // plane cuts through the solid
                }

                bool duplicate = false;
                for (const Face& f : faces)
                {
                    if (Dot(f.normal, outward) > 1.0f - 1.0e-5f && fabsf(f.d - d) < 1.0e-5f)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                Face face;
                face.normal = outward;
                face.d = d;
                for (const Vec3& p : verts)
                {
                    if (fabsf(Dot(outward, p) - d) < EPS)
                        face.poly.push_back(p);
                }
                if (face.poly.size() < 3)
                    continue;

                faces.push_back(face);
            }
    return faces;
}

/** Sorts a coplanar polygon counter-clockwise as seen from outside. */
void OrderPolygon(const Vec3& n, std::vector<Vec3>& poly)
{
    Vec3 c{ 0.0f, 0.0f, 0.0f };
    for (const Vec3& p : poly)
        c = c + p;
    c = c * (1.0f / (float)poly.size());

    const Vec3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    int   best = 0;
    float bestAbs = fabsf(n.x);
    if (fabsf(n.y) < bestAbs) { best = 1; bestAbs = fabsf(n.y); }
    if (fabsf(n.z) < bestAbs) { best = 2; }

    const Vec3 t = Normalize(Cross(n, axes[best]));
    const Vec3 b = Cross(n, t);   // (t, b, n) is right handed -> t->b is CCW

    std::sort(poly.begin(), poly.end(), [&](const Vec3& p, const Vec3& q) {
        const Vec3 dp = p - c;
        const Vec3 dq = q - c;
        return atan2f(Dot(dp, b), Dot(dp, t)) < atan2f(Dot(dq, b), Dot(dq, t));
    });
}

} // namespace

// --------------------------------------------------------------------------
// Public geometry API
// --------------------------------------------------------------------------
const char* SolidDisplayName(SolidType type)
{
    switch (type)
    {
    case SolidType::Tetrahedron:  return u8"正四面体 (4 面)";
    case SolidType::Cube:         return u8"正六面体 · 立方体 (6 面)";
    case SolidType::Octahedron:   return u8"正八面体 (8 面)";
    case SolidType::Dodecahedron: return u8"正十二面体 (12 面)";
    case SolidType::Icosahedron:  return u8"正二十面体 (20 面)";
    default:                      return "?";
    }
}

bool BuildSolidMesh(SolidType type, std::vector<float>& outVertices, int& outFaceCount)
{
    std::vector<Vec3> verts = BaseVertices(type);
    if (verts.empty())
        return false;

    // Normalize to circumradius 1 so every solid gets a comparable size.
    float maxLen = 0.0f;
    for (const Vec3& v : verts)
        maxLen = std::max(maxLen, Length(v));
    if (maxLen <= 0.0f)
        return false;
    for (Vec3& v : verts)
        v = v * (1.0f / maxLen);

    std::vector<Face> faces = BuildFaces(verts);
    if (faces.empty())
        return false;

    outFaceCount = (int)faces.size();
    outVertices.clear();

    for (int f = 0; f < (int)faces.size(); f++)
    {
        Face& face = faces[f];
        OrderPolygon(face.normal, face.poly);

        const int   n = (int)face.poly.size();
        const float hueBase = (float)f / (float)faces.size();

        // The rim sweeps 2/3 of the hue wheel, the fan centre takes the
        // remaining third: every rasterized triangle ends up with three
        // different corner colours which fade into each other.
        std::vector<Vec3> rim(n);
        for (int i = 0; i < n; i++)
        {
            float rgb[3];
            HsvToRgb(hueBase + ((float)i / (float)(std::max)(1, n - 1)) * (2.0f / 3.0f), 0.90f, 1.0f, rgb);
            rim[i] = { rgb[0], rgb[1], rgb[2] };
        }

        Vec3 centerPos{ 0.0f, 0.0f, 0.0f };
        for (const Vec3& p : face.poly)
            centerPos = centerPos + p;
        centerPos = centerPos * (1.0f / (float)n);

        float centerRgb[3];
        HsvToRgb(hueBase + 5.0f / 6.0f, 0.90f, 1.0f, centerRgb);
        const Vec3 centerColor{ centerRgb[0], centerRgb[1], centerRgb[2] };

        const auto push = [&](const Vec3& p, const Vec3& col) {
            outVertices.push_back(p.x);
            outVertices.push_back(p.y);
            outVertices.push_back(p.z);
            outVertices.push_back(face.normal.x);
            outVertices.push_back(face.normal.y);
            outVertices.push_back(face.normal.z);
            outVertices.push_back(col.x);
            outVertices.push_back(col.y);
            outVertices.push_back(col.z);
        };

        if (n == 3)
        {
            push(face.poly[0], rim[0]);
            push(face.poly[1], rim[1]);
            push(face.poly[2], rim[2]);
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                const int j = (i + 1) % n;
                push(centerPos, centerColor);
                push(face.poly[i], rim[i]);
                push(face.poly[j], rim[j]);
            }
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// Matrices
// --------------------------------------------------------------------------
Mat4 Mat4Identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mat4Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = sum;
        }
    return r;
}

Mat4 Mat4Translate(float x, float y, float z)
{
    Mat4 r = Mat4Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 Mat4RotateX(float a)
{
    const float c = cosf(a);
    const float s = sinf(a);
    Mat4 r = Mat4Identity();
    r.m[5] = c;  r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

Mat4 Mat4RotateY(float a)
{
    const float c = cosf(a);
    const float s = sinf(a);
    Mat4 r = Mat4Identity();
    r.m[0] = c; r.m[2] = -s;
    r.m[8] = s; r.m[10] = c;
    return r;
}

Mat4 Mat4RotateZ(float a)
{
    const float c = cosf(a);
    const float s = sinf(a);
    Mat4 r = Mat4Identity();
    r.m[0] = c; r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

Mat4 Mat4Perspective(float fovYRadians, float aspect, float nearZ, float farZ)
{
    const float f = 1.0f / tanf(fovYRadians * 0.5f);
    Mat4 r{};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (farZ + nearZ) / (nearZ - farZ);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    return r;
}

Mat4 SolidTumbleMatrix(float timeSeconds)
{
    return Mat4Multiply(Mat4RotateY(timeSeconds * 0.75f),
                        Mat4Multiply(Mat4RotateX(timeSeconds * 0.53f),
                                     Mat4RotateZ(timeSeconds * 0.31f)));
}

Mat4 SolidViewMatrix()
{
    return Mat4Translate(0.0f, 0.0f, -3.2f);
}

Mat4 SolidProjection()
{
    return Mat4Perspective(0.7854f /* 45 deg */, 1.0f, 0.1f, 100.0f);
}

void Mat4TransformPoint(const Mat4& m, float x, float y, float z, float out[4])
{
    out[0] = m.m[0] * x + m.m[4] * y + m.m[8]  * z + m.m[12];
    out[1] = m.m[1] * x + m.m[5] * y + m.m[9]  * z + m.m[13];
    out[2] = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
    out[3] = m.m[3] * x + m.m[7] * y + m.m[11] * z + m.m[15];
}

void Mat4TransformDirection(const Mat4& m, float x, float y, float z, float out[3])
{
    out[0] = m.m[0] * x + m.m[4] * y + m.m[8]  * z;
    out[1] = m.m[1] * x + m.m[5] * y + m.m[9]  * z;
    out[2] = m.m[2] * x + m.m[6] * y + m.m[10] * z;
}
