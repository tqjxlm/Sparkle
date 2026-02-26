#include "ray_trace.h.slang"

#include "debug.h.slang"
#include "directional_light.h.slang"
#include "pbr.h.slang"
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
    uint enable_nee;
    float4x4 current_view_projection;
    float4x4 previous_view_projection;
    uint write_reblur_inputs;
    uint has_history;
    uint2 reblur_padding;
};

[[vk::binding(1, 1)]] RWTexture2D<float4> imageData;
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialIdBuffer;
[[vk::binding(3, 1)]] StructuredBuffer<MaterialParameters> materialBuffer;
[[vk::binding(4, 1)]] TextureCube<float4> skyMap;
[[vk::binding(5, 1)]] SamplerState skyMapSampler;
[[vk::binding(6, 1)]] SamplerState materialTextureSampler;
[[vk::binding(7, 1)]] RWTexture2D<float4> reblur_normal_roughness_output;
[[vk::binding(8, 1)]] RWTexture2D<float> reblur_view_z_output;
[[vk::binding(9, 1)]] RWTexture2D<float4> reblur_motion_vector_output;
[[vk::binding(10, 1)]] RWTexture2D<float4> reblur_diff_radiance_hitdist_output;
[[vk::binding(11, 1)]] RWTexture2D<float4> reblur_spec_radiance_hitdist_output;

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
        mat_param.emissive_texture >= 0
            ? textures[mat_param.emissive_texture].SampleLevel(materialTextureSampler, tex_coord, 0).rgb
            : float3(1, 1, 1);
    return emissive_color_sampled * mat_param.emissive_color;
}

float3 SampleSurface(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float3 w_i,
                     out float out_pdf)
{
    if (mat_param.eta > 0)
    {
        return BxdfDieletric(w_o, w_i, out_pdf, mat_param, intersection);
    }
    else
    {
        return BxdfMetal(w_o, w_i, out_pdf, mat_param, intersection);
    }
}

bool IsRayOccluded(RayDesc ray_desc)
{
    RayQuery<RAY_FLAG_NONE> ray_query;
    ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray_desc);
    while (ray_query.Proceed())
    {
    }

    return ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 SampleSkyLight(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float out_pdf)
{
    // TODO(tqjxlm): sample direction according to luminance distribution
    float3 sample_direction = CosineWeightedHemisphere::Sample();

    float3 w_i = TransformBasisToWorld(sample_direction, intersection.normal, intersection.tangent.xyz);

    // shadow ray
    RayDesc ray_desc;
    ray_desc.Origin = intersection.position;
    ray_desc.Direction = w_i;
    ray_desc.TMin = Tolerance;
    ray_desc.TMax = 1000.0;
    if (IsRayOccluded(ray_desc))
    {
        // do not sample
        out_pdf = 0;
        return 0;
    }

    out_pdf = CosineWeightedHemisphere::Pdf(sample_direction);

    float3 light_sample = skyMap.SampleLevel(skyMapSampler, w_i, 0).rgb;

    SurfaceAttributes surface;
    surface.normal = intersection.normal;
    surface.albedo = mat_param.base_color;
    surface.roughness = mat_param.roughness;
    surface.metallic = mat_param.metallic;
    surface.emissive = mat_param.emissive_color;

    // evaluate brdf
    float3 radiance = CalculateDirectLighting(w_o, w_i, light_sample, surface);

    float cosine_theta = SaturateDot(w_i, intersection.normal);

    return radiance * cosine_theta / (out_pdf + Eps);
}

float3 SampleLights(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float out_pdf)
{
    // only consider sky light for now
    return SampleSkyLight(intersection, mat_param, w_o, out_pdf);
}

float GetLightPdf(float3 normal, float3 w_i)
{
    // only consider sky light for now
    return CosineWeightedHemisphere::Pdf(normal, w_i);
}

float GetMISWeight(float current_pdf, float compensation_pdf)
{
    if (current_pdf < Eps || compensation_pdf < Eps)
    {
        return 1.f;
    }

    float current_pdf_sqr = current_pdf * current_pdf;
    float compensation_pdf_sqr = compensation_pdf * compensation_pdf;
    return current_pdf_sqr / (compensation_pdf_sqr + current_pdf_sqr + Eps);
}

static const float4 ReblurHitDistanceParams = float4(3.0f, 0.1f, 20.0f, -25.0f);

struct PathTraceResult
{
    float3 color;
    uint has_primary_hit;
    float3 primary_world_position;
    float primary_view_z;
    float3 primary_normal;
    float primary_roughness;
    float primary_metallic;
    float primary_hit_distance;
};

float3 ReblurLinearToYCoCg(float3 color)
{
    float y = dot(color, float3(0.25f, 0.5f, 0.25f));
    float co = dot(color, float3(0.5f, 0.0f, -0.5f));
    float cg = dot(color, float3(-0.25f, 0.5f, -0.25f));

    return float3(y, co, cg);
}

float GetReblurHitDistanceNormalization(float view_z, float roughness)
{
    float roughness_factor = saturate(exp2(ReblurHitDistanceParams.w * roughness * roughness));
    float mixed_scale = lerp(1.0f, ReblurHitDistanceParams.z, roughness_factor);
    return (ReblurHitDistanceParams.x + abs(view_z) * ReblurHitDistanceParams.y) * mixed_scale;
}

float4 ReblurPackNormalAndRoughness(float3 normal, float roughness)
{
    float normal_length = length(normal);
    float3 safe_normal = normal_length > Eps ? normal / normal_length : float3(0.0f, 0.0f, 1.0f);
    safe_normal /= max(abs(safe_normal.x), max(abs(safe_normal.y), abs(safe_normal.z)));
    return float4(safe_normal * 0.5f + 0.5f, saturate(roughness));
}

float ReblurGetNormHitDist(float hit_distance, float view_z, float roughness)
{
    if (hit_distance <= 0.0f || abs(view_z) <= Eps)
    {
        return 0.0f;
    }

    float normalization = GetReblurHitDistanceNormalization(view_z, roughness);
    return saturate(hit_distance / max(normalization, Eps));
}

float4 ReblurPackRadianceAndNormHitDist(float3 radiance, float norm_hit_distance)
{
    float3 sanitized_radiance =
        any(isnan(radiance)) || any(isinf(radiance)) ? float3(0.0f, 0.0f, 0.0f) : max(radiance, 0.0f);
    float sanitized_norm_hit_distance =
        (isnan(norm_hit_distance) || isinf(norm_hit_distance)) ? 0.0f : saturate(norm_hit_distance);
    return float4(ReblurLinearToYCoCg(sanitized_radiance), sanitized_norm_hit_distance);
}

float2 ReblurComputeMotionVector(float3 world_position)
{
    if (has_history == 0)
    {
        return float2(0.0f, 0.0f);
    }

    float4 current_clip = mul(current_view_projection, float4(world_position, 1.0f));
    float4 prev_clip = mul(previous_view_projection, float4(world_position, 1.0f));
    if (abs(current_clip.w) <= Eps || abs(prev_clip.w) <= Eps)
    {
        return float2(0.0f, 0.0f);
    }

    float2 current_ndc = current_clip.xy / current_clip.w;
    float2 prev_ndc = prev_clip.xy / prev_clip.w;

    float2 current_uv = current_ndc * 0.5f + 0.5f;
    float2 prev_uv = prev_ndc * 0.5f + 0.5f;
    current_uv.y = 1.0f - current_uv.y;
    prev_uv.y = 1.0f - prev_uv.y;

    float2 current_pixel = current_uv * float2(camera.resolution);
    float2 prev_pixel = prev_uv * float2(camera.resolution);
    return prev_pixel - current_pixel;
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
    if (mat_param.normal_texture >= 0)
    {
        float3 tangent_normal = SampleNormal(mat_param.normal_texture, tex_coord);
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
        (mat_param.metallic_roughness_texture >= 0)
            ? textures[mat_param.metallic_roughness_texture].SampleLevel(materialTextureSampler, tex_coord, 0)
            : float4(1, 1, 1, 1);

    mat_param.roughness = metallic_roughness_sampled.y * mat_param.roughness;
    mat_param.metallic = metallic_roughness_sampled.z * mat_param.metallic;

    float3 base_color_sampled =
        (mat_param.base_color_texture >= 0)
            ? textures[mat_param.base_color_texture].SampleLevel(materialTextureSampler, tex_coord, 0).rgb
            : float3(1, 1, 1);

    mat_param.base_color = mat_param.base_color * base_color_sampled;

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

void GetIntersection(RayDesc ray_desc, RayQuery<RAY_FLAG_NONE> ray_query, out Intersection intersection)
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

PathTraceResult SamplePixel(RayDesc ray_desc)
{
    RayQuery<RAY_FLAG_NONE> ray_query;
    Intersection intersection;

    PathTraceResult result = (PathTraceResult)0;
    result.primary_normal = float3(0.0f, 0.0f, 1.0f);
    result.primary_roughness = 1.0f;

    float3 pixel_color = float3(0, 0, 0);
    float3 throughput = float3(1, 1, 1);
    int bounce;

    // When we introduce NEE, non-NEE paths should be weighted down
    float bsdf_mis_weight = 1.f;

    [loop] for (bounce = 0; bounce <= camera.max_bounce; bounce++)
    {
        // initialize ray query
        ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray_desc);

        // wait for ray query completion
        while (ray_query.Proceed())
        {
        }

        // terminal condition: hit sky
        if (ray_query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            float3 sky_light = SampleSkyLight(ray_desc.Direction);
            pixel_color += sky_light * throughput * bsdf_mis_weight;
            break;
        }

        // get intersection details
        GetIntersection(ray_desc, ray_query, intersection);
        MaterialParameters mat_param = GetMaterial(intersection);

        if (result.has_primary_hit == 0)
        {
            float3 front = normalize(cross(camera.max_v, camera.max_u));
            result.has_primary_hit = 1;
            result.primary_world_position = intersection.position;
            result.primary_view_z = max(dot(intersection.position - camera.position, front), 0.0f);
            result.primary_normal = normalize(intersection.normal);
            result.primary_roughness = saturate(mat_param.roughness);
            result.primary_metallic = saturate(mat_param.metallic);
            result.primary_hit_distance = max(length(intersection.position - ray_desc.Origin), 0.0f);
        }

        // terminal condition: emissive
        float3 emissive_color = SampleEmissive(intersection, mat_param);
        if (length(emissive_color) > Eps)
        {
            pixel_color += emissive_color * throughput * bsdf_mis_weight;
            break;
        }

        // core function: surface sampling
        float3 next_direction;
        float bsdf_pdf = 1.f;
        float3 this_throughput = SampleSurface(intersection, mat_param, -ray_desc.Direction, next_direction, bsdf_pdf);

        if (enable_nee != 0)
        {
            // next event estimation: explicitly sample light sources every bounce
            float light_pdf = 0.f;
            float3 nee_radiance = SampleLights(intersection, mat_param, -ray_desc.Direction, light_pdf);
            float light_mis_weight = GetMISWeight(light_pdf, bsdf_pdf);
            pixel_color += nee_radiance * throughput * light_mis_weight;

            // update mis weight for next bounce
            bsdf_mis_weight = GetMISWeight(bsdf_pdf, GetLightPdf(intersection.normal, next_direction));
        }

        // terminal condition: debug output
        switch (camera.mode)
        {
        case 3:
            result.color = VisualizeVector(intersection.normal);
            return result;
        case 4:
            result.color = VisualizeVector(next_direction);
            return result;
        case 7:
            result.color = ((float3)1) * mat_param.metallic;
            return result;
        case 8:
            result.color = ((float3)1) * mat_param.roughness;
            return result;
        case 9:
            result.color = mat_param.base_color;
            return result;
        case 10:
            result.color = emissive_color;
            return result;
        case 11: {
            float3 front = normalize(cross(camera.max_v, camera.max_u));
            result.color = ((float3)1) * dot(intersection.position - camera.position, front) / ray_desc.TMax;
            return result;
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
        ray_desc.Origin = intersection.position;
        ray_desc.Direction = next_direction;
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

    result.color = pixel_color;
    return result;
}

[shader("compute")][numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = uint2(DTid.xy);

    // bound check
    if (pixel.x >= camera.resolution.x || pixel.y >= camera.resolution.y)
    {
        return;
    }

    float3 all_samples = imageData[pixel].rgb;

    float3 this_samples = (float3)0;
    float3 primary_world_position_sum = (float3)0;
    float3 primary_normal_sum = (float3)0;
    float primary_roughness_sum = 0.0f;
    float primary_metallic_sum = 0.0f;
    float primary_view_z_sum = 0.0f;
    float primary_hit_distance_sum = 0.0f;
    uint primary_hit_count = 0;

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
        RayDesc ray_desc;
        ray_desc.Origin = camera.position + lens_offset;
        ray_desc.Direction = normalize(camera.lowerLeft + uv.x * camera.max_u + uv.y * camera.max_v - ray_desc.Origin);
        ray_desc.TMin = Tolerance;
        ray_desc.TMax = 1000.0f;

        PathTraceResult trace_result = SamplePixel(ray_desc);
        float3 this_pixel = trace_result.color;

        // firefly removal
        this_pixel = min(this_pixel, output_limit);

        this_samples += this_pixel;

        if (trace_result.has_primary_hit != 0)
        {
            primary_hit_count += 1;
            primary_world_position_sum += trace_result.primary_world_position;
            primary_normal_sum += trace_result.primary_normal;
            primary_roughness_sum += trace_result.primary_roughness;
            primary_metallic_sum += trace_result.primary_metallic;
            primary_view_z_sum += trace_result.primary_view_z;
            primary_hit_distance_sum += trace_result.primary_hit_distance;
        }
    }

    float3 frame_radiance = this_samples / float(spp);
    float moving_average = (float)total_sample_count / (total_sample_count + spp);
    all_samples = lerp(frame_radiance, all_samples, moving_average);

    // output
    imageData[pixel] = float4(all_samples, 1.f);

    if (write_reblur_inputs != 0)
    {
        float3 primary_world_position = float3(0.0f, 0.0f, 0.0f);
        float3 primary_normal = float3(0.0f, 0.0f, 1.0f);
        float primary_roughness = 1.0f;
        float primary_metallic = 0.0f;
        float primary_view_z = 0.0f;
        float primary_hit_distance = 0.0f;

        if (primary_hit_count > 0)
        {
            float hit_count = float(primary_hit_count);
            primary_world_position = primary_world_position_sum / hit_count;
            float3 averaged_normal = primary_normal_sum / hit_count;
            float normal_length = length(averaged_normal);
            primary_normal = normal_length > Eps ? averaged_normal / normal_length : float3(0.0f, 0.0f, 1.0f);
            primary_roughness = saturate(primary_roughness_sum / hit_count);
            primary_metallic = saturate(primary_metallic_sum / hit_count);
            primary_view_z = max(primary_view_z_sum / hit_count, 0.0f);
            primary_hit_distance = max(primary_hit_distance_sum / hit_count, 0.0f);
        }

        float norm_hit_distance = ReblurGetNormHitDist(primary_hit_distance, primary_view_z, primary_roughness);
        float3 diffuse_radiance =
            primary_hit_count > 0 ? frame_radiance * (1.0f - primary_metallic) : float3(0.0f, 0.0f, 0.0f);
        float3 specular_radiance = primary_hit_count > 0 ? frame_radiance - diffuse_radiance : float3(0.0f, 0.0f, 0.0f);
        float2 motion_vector =
            primary_hit_count > 0 ? ReblurComputeMotionVector(primary_world_position) : float2(0.0f, 0.0f);

        reblur_normal_roughness_output[pixel] = ReblurPackNormalAndRoughness(primary_normal, primary_roughness);
        reblur_view_z_output[pixel] = primary_view_z;
        reblur_motion_vector_output[pixel] = float4(motion_vector, 0.0f, 0.0f);
        reblur_diff_radiance_hitdist_output[pixel] =
            ReblurPackRadianceAndNormHitDist(diffuse_radiance, norm_hit_distance);
        reblur_spec_radiance_hitdist_output[pixel] =
            ReblurPackRadianceAndNormHitDist(specular_radiance, norm_hit_distance);
    }
}
