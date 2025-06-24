#pragma once

#include "core/math/Utilities.h"

namespace sparkle
{
struct Mesh
{
    explicit Mesh() = default;

    static std::shared_ptr<Mesh> GetUnitSphere();
    static std::shared_ptr<Mesh> GetUnitCube();

    std::string name;

    // stride * num_vertices
    std::vector<Vector3> vertices;
    // [xyz] * 3(triangle) * num_faces
    std::vector<Vector3> normals;
    // [xyzw] * 3(triangle) * num_faces
    std::vector<Vector4> tangents;
    // [xyz] * 3(triangle) * num_faces
    // std::vector<T> facevarying_binormals;
    // [xy]  * 3(triangle) * num_faces
    // TODO(tqjxlm): support multi-texcoords
    std::vector<Vector2> uvs;
    // [xyz] * 3(triangle) * num_faces
    // std::vector<T> facevarying_vertex_colors;
    // triangle x num_faces
    std::vector<unsigned int> indices;
    // index x num_faces
    // std::vector<unsigned int> material_ids;

    Vector3 center;
    Vector3 extent;

    [[nodiscard]] uint32_t GetNumIndices() const
    {
        return static_cast<uint32_t>(indices.size());
    }

    [[nodiscard]] size_t GetNumFaces() const
    {
        return indices.size() / 3;
    }

    [[nodiscard]] uint32_t GetNumVertices() const
    {
        return static_cast<uint32_t>(vertices.size());
    }

    void GetTriangle(uint32_t face_idx, Vector3 &v0, Vector3 &v1, Vector3 &v2) const
    {
        auto idx_offset = face_idx * 3;

        v0 = vertices[indices[idx_offset + 0]];
        v1 = vertices[indices[idx_offset + 1]];
        v2 = vertices[indices[idx_offset + 2]];
    }

    void GetTriangleMinMax(uint32_t face_idx, Vector3 &min, Vector3 &max) const
    {
        min = Ones * std::numeric_limits<Scalar>::max();
        max = Ones * -std::numeric_limits<Scalar>::max();

        auto idx_offset = face_idx * 3;

        // a face consists of 3 vertices
        for (unsigned i = 0; i < 3; i++)
        {
            auto index = indices[idx_offset + i];

            const Vector3 v = vertices[index];
            min = min.cwiseMin(v);
            max = max.cwiseMax(v);
        }
    }

    ///
    /// Get the geometric normal and the shading normal at `face_idx' th face.
    ///
    [[nodiscard]] Vector3 GetGeometryNormal(uint32_t face_idx) const
    {
        Vector3 v0;
        Vector3 v1;
        Vector3 v2;
        GetTriangle(face_idx, v0, v1, v2);

        return utilities::CalculateNormal(v0, v1, v2);
    }

    [[nodiscard]] Vector3 GetShadingNormal(uint32_t face_idx, const Scalar u, const Scalar v,
                                           const Vector3 &geometric_normal) const
    {
        if (normals.empty())
        {
            // Just use geometric normal
            return geometric_normal;
        }

        auto idx_offset = face_idx * 3;

        const auto &n0 = normals[indices[idx_offset + 0]];
        const auto &n1 = normals[indices[idx_offset + 1]];
        const auto &n2 = normals[indices[idx_offset + 2]];

        return utilities::Lerp(n0, n1, n2, u, v);
    }

    [[nodiscard]] Vector4 GetTangent(uint32_t face_idx, const Scalar u, const Scalar v) const
    {
        if (tangents.empty())
        {
            return utilities::ConcatVector(Right, 1.f);
        }

        auto idx_offset = face_idx * 3;

        const auto &t0 = tangents[indices[idx_offset + 0]];
        const auto &t1 = tangents[indices[idx_offset + 1]];
        const auto &t2 = tangents[indices[idx_offset + 2]];

        return utilities::Lerp(t0, t1, t2, u, v);
    }

    [[nodiscard]] Vector2 GetTexCoord(uint32_t face_idx, const Scalar u, const Scalar v) const
    {
        if (uvs.empty())
        {
            return Vector2::Zero();
        }

        auto idx_offset = face_idx * 3;

        const auto &t0 = uvs[indices[idx_offset + 0]];
        const auto &t1 = uvs[indices[idx_offset + 1]];
        const auto &t2 = uvs[indices[idx_offset + 2]];

        return utilities::Lerp(t0, t1, t2, u, v);
    }

    [[nodiscard]] bool Validate() const
    {
        return !vertices.empty() && !indices.empty() && vertices.size() == normals.size() &&
               vertices.size() == tangents.size() && vertices.size() == uvs.size();
    }
};
} // namespace sparkle
