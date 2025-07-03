#include "ray_trace.h.slang"

#include "debug.h.slang"
#include "directional_light.h.slang"
#include "random.h.slang"
#include "sky_light.h.slang"

struct Camera
{
    float3 position;
    uint mode;
    float3 lowerLeft;
    uint max_bounce;
    float3 max_u;
    float3 max_v;
    float lensRadius;
    uint2 resolution;
};

// set 0: tlas, shared by the whole scene
[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;

// set 1: global resources and parameters, shared by the whole scene
[[vk::binding(0, 1)]] cbuffer ubo
{
    Camera camera;
    SkyLight sky_light;
    DirectionalLight dir_light;
    uint time_seed;
    float output_limit;
    uint total_sample_count;
    uint spp;
};

[[vk::binding(1, 1)]] RWTexture2D<float4> imageData;
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialIdBuffer;
[[vk::binding(3, 1)]] StructuredBuffer<MaterialParameters> materialBuffer;
[[vk::binding(4, 1)]] TextureCube<float4> skyMap;
[[vk::binding(5, 1)]] SamplerState skyMapSampler;
[[vk::binding(6, 1)]] SamplerState materialTextureSampler;

// set 2: bindless resources, shared by the whole scene
[[vk::binding(0, 2)]] Texture2D textures[];
[[vk::binding(1, 2)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(2, 2)]] StructuredBuffer<float> vertexBuffers[];
[[vk::binding(3, 2)]] StructuredBuffer<VertexAttribute> vertexAttributeBuffers[];

float3 SampleNormal(uint texture_id, float2 tex_coord)
{
    return textures[texture_id].SampleLevel(materialTextureSampler, tex_coord, 0).xyz * 2 - 1;
}

float3 SampleSkyLight(float3 direction)
{
    if (sky_light.has_sky_map != 0)
    {
        float3 light = skyMap.SampleLevel(skyMapSampler, direction, 0).rgb;

        return light;
    }

    return 0;
}

float3 SampleEmissive(inout Intersection intersection, MaterialParameters mat_param)
{
    float2 tex_coord = intersection.texCoord;

    float3 emissive_color_sampled =
        mat_param.emissiveTextureId >= 0
            ? textures[mat_param.emissiveTextureId].SampleLevel(materialTextureSampler, tex_coord, 0).rgb
            : float3(1, 1, 1);
    return emissive_color_sampled * mat_param.emissiveColor;
}

float3 SampleSurface(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float3 w_i)
{
    if (mat_param.eta > 0)
    {
        return BxdfDieletric(w_o, w_i, mat_param, intersection);
    }
    else
    {
        return BxdfMetal(w_o, w_i, mat_param, intersection);
    }
}

MaterialParameters GetMaterial(inout Intersection intersection)
{
    uint material_id = intersection.materialId;
    float3 geometry_normal = intersection.normal;
    float4 tangent = intersection.tangent;
    float2 tex_coord = intersection.texCoord;

    MaterialParameters mat_param = materialBuffer[material_id];

    // Normal mapping
    float3 surface_normal;
    if (mat_param.normalTextureId >= 0)
    {
        float3 tangent_normal = SampleNormal(mat_param.normalTextureId, tex_coord);
        float3 bi_tangent = cross(geometry_normal, tangent.xyz) * tangent.w;
        surface_normal = normalize(TransformBasisToWorld(tangent_normal, tangent.xyz, bi_tangent, geometry_normal));
    }
    else
    {
        surface_normal = geometry_normal;
    }

    intersection.normal = TransformDirection(surface_normal, (float3x3)intersection.inv_transform);
    intersection.tangent.xyz = TransformDirection(tangent.xyz, (float3x3)intersection.inv_transform);

    float4 metallic_roughness_sampled =
        (mat_param.metallicRoughnessTextureId >= 0)
            ? textures[mat_param.metallicRoughnessTextureId].SampleLevel(materialTextureSampler, tex_coord, 0)
            : float4(1, 1, 1, 1);

    mat_param.roughness = metallic_roughness_sampled.y * mat_param.roughness;
    mat_param.metallic = metallic_roughness_sampled.z * mat_param.metallic;

    float3 base_color_sampled =
        (mat_param.baseColorTextureId >= 0)
            ? textures[mat_param.baseColorTextureId].SampleLevel(materialTextureSampler, tex_coord, 0).rgb
            : float3(1, 1, 1);

    mat_param.baseColor = mat_param.baseColor * base_color_sampled;

    return mat_param;
}

#define BaryLerp(v0, v1, v2, uv) (v0 + (v1 - v0) * uv.x + (v2 - v0) * uv.y)

#define AccessFaceAttribute(triangle_id, buf, vertex_idx) (buf[triangle_id * 3 + vertex_idx])

VertexAttribute GetVertexAttribute(uint instance_id, uint index)
{
    return vertexAttributeBuffers[instance_id][index];
}

float3 GetVertex(uint instance_id, uint index)
{
    return float3(vertexBuffers[instance_id][index * 3], vertexBuffers[instance_id][index * 3 + 1],
                  vertexBuffers[instance_id][index * 3 + 2]);
}

void GetIntersection(RayDesc ray, RayQuery<RAY_FLAG_NONE> ray_query, out Intersection intersection)
{
    const float2 intersection_uv = ray_query.CommittedTriangleBarycentrics();
    const uint instance_id = ray_query.CommittedInstanceIndex();
    const uint triangle_id = ray_query.CommittedPrimitiveIndex();
    const float4x3 transform = ray_query.CommittedObjectToWorld4x3();
    const float4x3 inv_transform = ray_query.CommittedWorldToObject4x3();

    uint index0 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 0);
    uint index1 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 1);
    uint index2 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 2);

    const VertexAttribute va0 = GetVertexAttribute(instance_id, index0);
    const VertexAttribute va1 = GetVertexAttribute(instance_id, index1);
    const VertexAttribute va2 = GetVertexAttribute(instance_id, index2);

    const float3 v0 = TransformPoint(GetVertex(instance_id, index0), transform);
    const float3 v1 = TransformPoint(GetVertex(instance_id, index1), transform);
    const float3 v2 = TransformPoint(GetVertex(instance_id, index2), transform);

    intersection.position = BaryLerp(v0, v1, v2, intersection_uv);
    intersection.inv_transform = inv_transform;
    intersection.normal = BaryLerp(va0.normal, va1.normal, va2.normal, intersection_uv);
    intersection.tangent = BaryLerp(va0.tangent, va1.tangent, va2.tangent, intersection_uv);
    intersection.texCoord = BaryLerp(va0.texCoord, va1.texCoord, va2.texCoord, intersection_uv);
    intersection.materialId = materialIdBuffer[instance_id];
    intersection.primitiveId = instance_id;
}

float3 SamplePixel(RayDesc ray)
{
    RayQuery<RAY_FLAG_NONE> ray_query;
    Intersection intersection;

    float3 pixel_color = float3(0, 0, 0);
    float3 throughput = float3(1, 1, 1);
    int bounce;

    [loop] for (bounce = 0; bounce <= camera.max_bounce; bounce++)
    {
        // initialize ray query
        ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);

        // wait for ray query completion
        while (ray_query.Proceed())
        {
        }

        // terminal condition: hit sky
        if (ray_query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            float3 sky_light = SampleSkyLight(ray.Direction);
            pixel_color += sky_light * throughput;
            break;
        }

        // get intersection details
        GetIntersection(ray, ray_query, intersection);
        MaterialParameters mat_param = GetMaterial(intersection);

        // terminal condition: emissive
        float3 emissive_color = SampleEmissive(intersection, mat_param);
        if (length(emissive_color) > Eps)
        {
            pixel_color += emissive_color * throughput;
            break;
        }

        // core function: surface sampling
        float3 next_direction;
        float3 this_throughput = SampleSurface(intersection, mat_param, -ray.Direction, next_direction);

        // terminal condition: debug output
        switch (camera.mode)
        {
        case 3:
            return VisualizeVector(intersection.normal);
        case 4:
            return VisualizeVector(next_direction);
        case 7:
            return ((float3)1) * mat_param.metallic;
        case 8:
            return ((float3)1) * mat_param.roughness;
        case 9:
            return mat_param.baseColor;
        case 10:
            return emissive_color;
        case 11: {
            float3 front = normalize(cross(camera.max_v, camera.max_u));
            return ((float3)1) * dot(intersection.position - camera.position, front) / ray.TMax;
        }
        default:
            break;
        }

        throughput *= this_throughput;

        // terminal condition: early out
        if (length(throughput) < Eps)
        {
            break;
        }

        // Russian roulette
        if (bounce >= 3)
        {
            float p = max(throughput.r, max(throughput.g, throughput.b));

            // Avoid too low probability. It will introduce a small bias.
            p = clamp(p, 0.05, 1.0);

            if (RandomUnit() > p)
            {
                break;
            }

            // Compensate for survival probability
            throughput /= p;
        }

        // next ray
        ray.Origin = intersection.position;
        ray.Direction = next_direction;
    }

    // debug output
    switch (camera.mode)
    {
    case 1:
        pixel_color = debug_color.xyz;
        break;
    case 2:
        pixel_color = PickColorPalette(bounce);
        break;
    case 5:
        if (bounce <= 1)
        {
            pixel_color = (float3)0;
        }
        break;
    case 6:
        if (bounce > 1)
        {
            pixel_color = (float3)0;
        }
        break;
    default:
        break;
    }

    return pixel_color;
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = uint2(DTid.xy);

    // bound check
    if (pixel.x >= camera.resolution.x || pixel.y >= camera.resolution.y)
    {
        return;
    }

    float3 all_samples = imageData[pixel].rgb;

    float3 this_samples = (float3)0;

    InitRandomBase(pixel.x, pixel.y, time_seed);

    float2 pixel_size = 1.f / (float2(camera.resolution) - 1);

    for (uint i = 0; i < spp; i++)
    {
        // anti-aliasing
        float2 antialiasing_noise = float2(RandomUnit(), RandomUnit());
        float2 uv = (float2(pixel.x, pixel.y) + antialiasing_noise) * pixel_size;

        // invert Y
        uv.y = 1.0f - uv.y;

        // depth of field
        float2 lens_noise = UnitDisk() * camera.lensRadius;
        float3 lens_offset = lens_noise.x * normalize(camera.max_u) + lens_noise.y * normalize(camera.max_v);

        // setup the initial ray
        RayDesc ray;
        ray.Origin = camera.position + lens_offset;
        ray.Direction = normalize(camera.lowerLeft + uv.x * camera.max_u + uv.y * camera.max_v - ray.Origin);
        ray.TMin = Tolerance;
        ray.TMax = 1000.0f;

        float3 this_pixel = SamplePixel(ray);

        // a hard limit to remove noise. will introduce a small bias, though.
        this_pixel = min(this_pixel, output_limit);

        this_samples += this_pixel;
    }

    float inv_next_sample_count = 1.f / (total_sample_count + spp);
    all_samples = total_sample_count * inv_next_sample_count * all_samples + inv_next_sample_count * this_samples;

    // output
    imageData[pixel] = float4(all_samples, 1.f);
}