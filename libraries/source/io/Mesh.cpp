#include "io/Mesh.h"

#include "core/math/Utilities.h"

namespace sparkle
{
std::shared_ptr<Mesh> Mesh::GetUnitSphere()
{
    static std::shared_ptr<Mesh> mesh;

    if (mesh)
    {
        return mesh;
    }

    mesh = std::make_shared<Mesh>();

    const int base_vertex_count = 32;
    const float refinement = 1.0f;
    auto v_size = std::max(2u, static_cast<unsigned>(base_vertex_count * refinement));
    auto u_size = v_size * 2;

    mesh->vertices.resize((u_size + 1) * (v_size + 1));
    mesh->indices.resize(u_size * v_size * 6);
    mesh->uvs.resize(mesh->vertices.size());
    mesh->normals.resize(mesh->vertices.size());
    mesh->tangents.resize(mesh->vertices.size());

    auto vi = 0u;
    for (auto v = 0u; v <= v_size; v++)
    {
        for (auto u = 0u; u <= u_size; u++)
        {
            auto theta = 2.f * Pi * static_cast<float>(u) / static_cast<float>(u_size) + Pi;
            auto phi = Pi * static_cast<float>(v) / static_cast<float>(v_size);

            auto x = std::cos(theta) * std::sin(phi);
            auto y = -std::cos(phi);
            auto z = std::sin(theta) * std::sin(phi);

            mesh->vertices[vi] = Vector3(x, y, z).normalized();

            vi++;
        }
    }

    auto ti = 0u;
    vi = 0;
    for (auto y = 0u; y < v_size; y++)
    {
        for (auto x = 0u; x < u_size; x++)
        {
            mesh->indices[ti] = vi;
            mesh->indices[ti + 3] = mesh->indices[ti + 2] = vi + 1;
            mesh->indices[ti + 4] = mesh->indices[ti + 1] = vi + u_size + 1;
            mesh->indices[ti + 5] = vi + u_size + 2;

            ti += 6;
            vi++;
        }

        vi++;
    }

    for (auto i = 0u; i < mesh->vertices.size(); i++)
    {
        auto v = i / (u_size + 1);
        auto u = i % (u_size + 1);
        mesh->uvs[i].x() = static_cast<float>(u) / static_cast<float>(u_size);
        mesh->uvs[i].y() = static_cast<float>(v) / static_cast<float>(v_size);
        mesh->normals[i] = mesh->vertices[i];

        const Vector3 major_axis = utilities::GetPossibleMajorAxis(mesh->normals[i]);
        mesh->tangents[i] = utilities::ConcatVector(mesh->normals[i].cross(major_axis).cross(mesh->normals[i]), 1.0f);
    }

    mesh->center = Zeros;
    mesh->extent = Ones;

    return mesh;
}

static void BuildQuad(std::vector<Vector3> &InVertices, std::vector<uint32_t> &InTriangles,
                      std::vector<Vector3> &InNormals, std::vector<Vector4> &InTangents,
                      std::vector<Vector2> &InTexCoords, const Vector3 &BottomLeft, const Vector3 &BottomRight,
                      const Vector3 &TopRight, const Vector3 &TopLeft, uint32_t &VertexOffset, uint32_t &TriangleOffset,
                      const Vector3 &Normal, const Vector4 &Tangent)
{
    const uint32_t index1 = VertexOffset++;
    const uint32_t index2 = VertexOffset++;
    const uint32_t index3 = VertexOffset++;
    const uint32_t index4 = VertexOffset++;

    InVertices[index1] = BottomLeft;
    InVertices[index2] = BottomRight;
    InVertices[index3] = TopRight;
    InVertices[index4] = TopLeft;

    InTexCoords[index1] = Vector2(0.0f, 1.0f);
    InTexCoords[index2] = Vector2(1.0f, 1.0f);
    InTexCoords[index3] = Vector2(1.0f, 0.0f);
    InTexCoords[index4] = Vector2(0.0f, 0.0f);

    // On a cube side, all the vertex normals face the same way
    InNormals[index1] = InNormals[index2] = InNormals[index3] = InNormals[index4] = Normal;
    InTangents[index1] = InTangents[index2] = InTangents[index3] = InTangents[index4] = Tangent;

    InTriangles[TriangleOffset++] = index1;
    InTriangles[TriangleOffset++] = index3;
    InTriangles[TriangleOffset++] = index2;
    InTriangles[TriangleOffset++] = index1;
    InTriangles[TriangleOffset++] = index4;
    InTriangles[TriangleOffset++] = index3;
}

static void GenerateCube(std::vector<Vector3> &InVertices, std::vector<uint32_t> &InTriangles,
                         std::vector<Vector3> &InNormals, std::vector<Vector4> &InTangents,
                         std::vector<Vector2> &InTexCoords)
{
    const float offset_x = 1.f;
    const float offset_y = 1.f;
    const float offset_z = 1.f;

    // Define the 8 corners of the cube
    const Vector3 p0 = Vector3(offset_x, offset_y, -offset_z);
    const Vector3 p1 = Vector3(offset_x, -offset_y, -offset_z);
    const Vector3 p2 = Vector3(offset_x, -offset_y, offset_z);
    const Vector3 p3 = Vector3(offset_x, offset_y, offset_z);
    const Vector3 p4 = Vector3(-offset_x, offset_y, -offset_z);
    const Vector3 p5 = Vector3(-offset_x, -offset_y, -offset_z);
    const Vector3 p6 = Vector3(-offset_x, -offset_y, offset_z);
    const Vector3 p7 = Vector3(-offset_x, offset_y, offset_z);

    // Now we create 6x faces, 4 vertices each
    uint32_t vertex_offset = 0;
    uint32_t triangle_offset = 0;
    Vector3 normal;
    Vector4 tangent;

    // Front (+X) face: 0-1-2-3
    normal = Vector3(1, 0, 0);
    tangent = Vector4(0, 1, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p0, p1, p2, p3, vertex_offset,
              triangle_offset, normal, tangent);

    // Back (-X) face: 5-4-7-6
    normal = Vector3(-1, 0, 0);
    tangent = Vector4(0, -1, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p5, p4, p7, p6, vertex_offset,
              triangle_offset, normal, tangent);

    // Left (-Y) face: 1-5-6-2
    normal = Vector3(0, -1, 0);
    tangent = Vector4(1, 0, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p1, p5, p6, p2, vertex_offset,
              triangle_offset, normal, tangent);

    // Right (+Y) face: 4-0-3-7
    normal = Vector3(0, 1, 0);
    tangent = Vector4(-1, 0, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p4, p0, p3, p7, vertex_offset,
              triangle_offset, normal, tangent);

    // Top (+Z) face: 6-7-3-2
    normal = Vector3(0, 0, 1);
    tangent = Vector4(0, 1, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p6, p7, p3, p2, vertex_offset,
              triangle_offset, normal, tangent);

    // Bottom (-Z) face: 1-0-4-5
    normal = Vector3(0, 0, -1);
    tangent = Vector4(0, -1, 0, 1);
    BuildQuad(InVertices, InTriangles, InNormals, InTangents, InTexCoords, p1, p0, p4, p5, vertex_offset,
              triangle_offset, normal, tangent);
}

std::shared_ptr<Mesh> Mesh::GetUnitCube()
{
    static std::shared_ptr<Mesh> mesh;

    if (mesh)
    {
        return mesh;
    }

    mesh = std::make_shared<Mesh>();

    mesh->vertices.resize(24);
    mesh->indices.resize(36);
    mesh->normals.resize(24);
    mesh->tangents.resize(24);
    mesh->uvs.resize(24);

    GenerateCube(mesh->vertices, mesh->indices, mesh->normals, mesh->tangents, mesh->uvs);

    mesh->center = Zeros;
    mesh->extent = Ones;

    return mesh;
}
} // namespace sparkle
