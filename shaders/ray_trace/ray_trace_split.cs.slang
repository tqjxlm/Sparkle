#include "ray_trace.h.slang"

#include "debug.h.slang"
#include "directional_light.h.slang"
#include "random.h.slang"
#include "reblur_common.h.slang"
#include "sky_light.h.slang"
#include "pbr.h.slang"

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
    float4 hit_dist_params;
    float4x4 view_matrix;
    float4x4 world_to_clip;
    float4x4 world_to_clip_prev;
    uint debug_mode;
};

[[vk::binding(1, 1)]] RWTexture2D<float4> imageData;
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialIdBuffer;
[[vk::binding(3, 1)]] StructuredBuffer<MaterialParameters> materialBuffer;
[[vk::binding(4, 1)]] TextureCube<float4> skyMap;
[[vk::binding(5, 1)]] SamplerState skyMapSampler;
[[vk::binding(6, 1)]] SamplerState materialTextureSampler;

// REBLUR auxiliary output buffers
[[vk::binding(7, 1)]] RWTexture2D<float4> diffuseOutput;
[[vk::binding(8, 1)]] RWTexture2D<float4> specularOutput;
[[vk::binding(9, 1)]] RWTexture2D<float4> normalRoughnessOutput;
[[vk::binding(10, 1)]] RWTexture2D<float> viewZOutput;
[[vk::binding(11, 1)]] RWTexture2D<float2> motionVectorOutput;
[[vk::binding(12, 1)]] RWTexture2D<float4> albedoOutput;
[[vk::binding(13, 1)]] RWTexture2D<float4> ptAccumulation;

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

float3 SampleSurface(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float3 w_i, out float out_pdf)
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
    float3 sample_direction = CosineWeightedHemisphere::Sample();
    float3 w_i = TransformBasisToWorld(sample_direction, intersection.normal, intersection.tangent.xyz);

    RayDesc ray_desc;
    ray_desc.Origin = intersection.position;
    ray_desc.Direction = w_i;
    ray_desc.TMin = Tolerance;
    ray_desc.TMax = 1000.0;
    if (IsRayOccluded(ray_desc))
    {
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

    float3 radiance = CalculateDirectLighting(w_o, w_i, light_sample, surface);
    float cosine_theta = SaturateDot(w_i, intersection.normal);

    return radiance * cosine_theta / (out_pdf + Eps);
}

float3 SampleLights(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float out_pdf)
{
    return SampleSkyLight(intersection, mat_param, w_o, out_pdf);
}

float GetLightPdf(float3 normal, float3 w_i)
{
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

MaterialParameters GetMaterial(inout Intersection intersection)
{
    uint material_id = intersection.materialId;
    float3 geometry_normal = intersection.normal;
    float4 tangent = intersection.tangent;
    float2 tex_coord = intersection.texCoord;

    MaterialParameters mat_param = materialBuffer[material_id];

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

struct SplitPathOutput
{
    float3 diffuse_radiance;
    float diffuse_hit_dist;
    float3 specular_radiance;
    float specular_hit_dist;
    float3 world_normal;
    float roughness;
    float view_z;
    float3 albedo;
    float metallic;
    float3 total_radiance;  // combined radiance mirroring vanilla's pixel_color
    float3 primary_world_pos;
    bool is_hit;
};

SplitPathOutput SamplePixelSplit(RayDesc ray_desc)
{
    RayQuery<RAY_FLAG_NONE> ray_query;
    Intersection intersection;

    SplitPathOutput result;
    result.diffuse_radiance = float3(0, 0, 0);
    result.diffuse_hit_dist = 0;
    result.specular_radiance = float3(0, 0, 0);
    result.specular_hit_dist = 0;
    result.world_normal = float3(0, 0, 1);
    result.roughness = 1.0;
    result.view_z = 1e6;
    result.albedo = float3(0, 0, 0);
    result.metallic = 0;
    result.total_radiance = float3(0, 0, 0);
    result.primary_world_pos = float3(0, 0, 0);
    result.is_hit = false;

    float3 throughput = float3(1, 1, 1);
    float bsdf_mis_weight = 1.f;
    bool is_diffuse_path = false;
    float primary_hit_dist = 0;

    [loop] for (int bounce = 0; bounce <= camera.max_bounce; bounce++)
    {
        ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray_desc);
        while (ray_query.Proceed())
        {
        }

        if (ray_query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            float3 sky_contrib = SampleSkyLight(ray_desc.Direction) * throughput * bsdf_mis_weight;

            if (bounce == 0)
            {
                // Sky pixel: no geometry hit
                result.total_radiance += sky_contrib;
                return result;
            }

            if (bounce == 1)
            {
                // Secondary ray hit sky: record hit distance as TMax
                primary_hit_dist = ray_desc.TMax;
            }

            // Accumulate into the appropriate channel
            result.total_radiance += sky_contrib;
            if (is_diffuse_path)
            {
                result.diffuse_radiance += sky_contrib;
            }
            else
            {
                result.specular_radiance += sky_contrib;
            }
            break;
        }

        GetIntersection(ray_desc, ray_query, intersection);
        MaterialParameters mat_param = GetMaterial(intersection);

        // Store primary hit data
        if (bounce == 0)
        {
            result.is_hit = true;
            result.primary_world_pos = intersection.position;
            result.world_normal = intersection.normal;
            result.roughness = mat_param.roughness;
            result.metallic = mat_param.metallic;
            result.albedo = mat_param.base_color;

            // View-space Z: transform hit position by view matrix and take Z
            float4 view_pos = mul(view_matrix, float4(intersection.position, 1.0));
            result.view_z = view_pos.z;
        }

        if (bounce == 1)
        {
            // Distance from primary hit to secondary hit
            primary_hit_dist = length(intersection.position - ray_desc.Origin);
        }

        // Emissive
        float3 emissive_color = SampleEmissive(intersection, mat_param);
        if (length(emissive_color) > Eps)
        {
            float3 emissive_contrib = emissive_color * throughput * bsdf_mis_weight;
            if (bounce == 0)
            {
                // Emissive primary hit: put in diffuse channel.
                // Set albedo to 1 so the composite pass-through is identity
                // (emissive light is independent of surface albedo).
                result.total_radiance += emissive_contrib;
                result.diffuse_radiance += emissive_contrib;
                result.albedo = float3(1, 1, 1);
                return result;
            }

            result.total_radiance += emissive_contrib;
            if (is_diffuse_path)
            {
                result.diffuse_radiance += emissive_contrib;
            }
            else
            {
                result.specular_radiance += emissive_contrib;
            }
            break;
        }

        // Surface sampling
        float3 next_direction;
        float bsdf_pdf = 1.f;
        float3 this_throughput = SampleSurface(intersection, mat_param, -ray_desc.Direction, next_direction, bsdf_pdf);

        if (enable_nee != 0)
        {
            float light_pdf = 0.f;
            float3 nee_radiance = SampleLights(intersection, mat_param, -ray_desc.Direction, light_pdf);
            float light_mis_weight = GetMISWeight(light_pdf, bsdf_pdf);
            float3 nee_contrib = nee_radiance * throughput * light_mis_weight;
            result.total_radiance += nee_contrib;

            if (bounce == 0)
            {
                // NEE at primary hit: split based on lobe type
                // For simplicity, attribute NEE to diffuse channel
                result.diffuse_radiance += nee_contrib;
            }
            else if (is_diffuse_path)
            {
                result.diffuse_radiance += nee_contrib;
            }
            else
            {
                result.specular_radiance += nee_contrib;
            }

            bsdf_mis_weight = GetMISWeight(bsdf_pdf, GetLightPdf(intersection.normal, next_direction));
        }

        // At bounce 0, determine if this is a diffuse or specular path
        if (bounce == 0)
        {
            // Heuristic: if the sampled direction is close to the reflection direction,
            // treat it as specular; otherwise diffuse.
            float3 w_o = -ray_desc.Direction;
            float3 reflect_dir = reflect(-w_o, intersection.normal);
            float cos_angle = dot(next_direction, reflect_dir);

            // For metals, everything is specular
            // For dielectrics with low roughness and high cos_angle, treat as specular
            if (mat_param.metallic > 0.5 || (cos_angle > 0.5 && mat_param.roughness < 0.5))
            {
                is_diffuse_path = false;
            }
            else
            {
                is_diffuse_path = true;
            }
        }

        throughput *= this_throughput;

        if (length(throughput) < Eps)
        {
            break;
        }

        // Russian roulette
        if (bounce >= 3)
        {
            float p = max(throughput.r, max(throughput.g, throughput.b));
            p = clamp(p, 0.05, 1.0);
            if (RandomUnit() > p)
            {
                break;
            }
            throughput /= p;
        }

        ray_desc.Origin = intersection.position;
        ray_desc.Direction = next_direction;
    }

    // Clamp diffuse+specular to output_limit before demodulation.
    // This matches the vanilla PT's energy budget (which clamps total_radiance
    // to output_limit) while preventing Float16 overflow after demodulation.
    // Without this clamp, bright radiance on dark surfaces produces demodulated
    // values exceeding Float16 max (65504), causing systematic energy loss
    // when the denoiser textures (RGBAFloat16) clip the values.
    if (result.is_hit)
    {
        float3 sum = result.diffuse_radiance + result.specular_radiance;
        float max_component = max(sum.r, max(sum.g, sum.b));
        if (max_component > output_limit)
        {
            float scale = output_limit / max_component;
            result.diffuse_radiance *= scale;
            result.specular_radiance *= scale;
        }
    }

    // Demodulate: divide out albedo from diffuse
    if (result.is_hit)
    {
        float3 albedo = result.albedo;
        result.diffuse_radiance /= max(albedo, float3(1e-6, 1e-6, 1e-6));

        // Normalize hit distance
        HitDistanceParams hdp;
        hdp.A = hit_dist_params.x;
        hdp.B = hit_dist_params.y;
        hdp.C = hit_dist_params.z;
        hdp.D = hit_dist_params.w;

        result.diffuse_hit_dist = NormalizeHitDistance(primary_hit_dist, result.view_z, 1.0, hdp);
        result.specular_hit_dist = NormalizeHitDistance(primary_hit_dist, result.view_z, result.roughness, hdp);
    }

    return result;
}

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid
                                  : SV_DispatchThreadID) {
    uint2 pixel = uint2(DTid.xy);

    if (pixel.x >= camera.resolution.x || pixel.y >= camera.resolution.y)
    {
        return;
    }

    InitRandomBase(pixel.x, pixel.y, time_seed);

    float2 pixel_size = 1.f / (float2(camera.resolution) - 1);

    float4 diff_accum = float4(0, 0, 0, 0);
    float4 spec_accum = float4(0, 0, 0, 0);
    float3 this_combined = float3(0, 0, 0);

    // G-buffer from last sample (single sample per pixel typically for denoiser input)
    SplitPathOutput last_output;
    last_output.is_hit = false;

    for (uint i = 0; i < spp; i++)
    {
        float2 antialiasing_noise = float2(RandomUnit(), RandomUnit());
        float2 uv = (float2(pixel.x, pixel.y) + antialiasing_noise) * pixel_size;
        uv.y = 1.0f - uv.y;

        float2 lens_noise = UnitDisk() * camera.lensRadius;
        float3 lens_offset = lens_noise.x * normalize(camera.max_u) + lens_noise.y * normalize(camera.max_v);

        RayDesc ray_desc;
        ray_desc.Origin = camera.position + lens_offset;
        ray_desc.Direction = normalize(camera.lowerLeft + uv.x * camera.max_u + uv.y * camera.max_v - ray_desc.Origin);
        ray_desc.TMin = Tolerance;
        ray_desc.TMax = 1000.0f;

        SplitPathOutput output = SamplePixelSplit(ray_desc);

        // Accumulate combined radiance (identical to vanilla's per-sample clamping)
        float3 combined_clamped = min(output.total_radiance, output_limit);
        this_combined += combined_clamped;

        // Diffuse/specular clamped to output_limit pre-demodulation (matching vanilla PT)
        diff_accum += float4(output.diffuse_radiance, output.diffuse_hit_dist);
        spec_accum += float4(output.specular_radiance, output.specular_hit_dist);

        last_output = output;
    }

    // Average over SPP
    diff_accum /= float(spp);
    spec_accum /= float(spp);

    // For the denoiser, we write the current frame's data directly (no temporal moving average).
    // The denoiser handles temporal accumulation internally.
    diffuseOutput[pixel] = diff_accum;
    specularOutput[pixel] = spec_accum;

    // Write G-buffer data from the last sample
    if (last_output.is_hit)
    {
        float2 encoded_normal = EncodeNormalOct(last_output.world_normal);
        normalRoughnessOutput[pixel] = float4(encoded_normal, last_output.roughness, 0);
        viewZOutput[pixel] = last_output.view_z;
        albedoOutput[pixel] = float4(last_output.albedo, last_output.metallic);
    }
    else
    {
        // Sky pixel: mark with large viewZ
        normalRoughnessOutput[pixel] = float4(0.5, 0.5, 0, 0);
        viewZOutput[pixel] = 1e6;
        albedoOutput[pixel] = float4(0, 0, 0, 0);
    }

    // Motion vectors: compute from world position projected through current and previous matrices.
    // Convention: MV = prevUV - currentUV (where did this pixel come from in the previous frame?)
    if (last_output.is_hit)
    {
        float3 worldPos = last_output.primary_world_pos;

        float4 clipCurr = mul(world_to_clip, float4(worldPos, 1.0));
        float2 uvCurr = clipCurr.xy / clipCurr.w * 0.5 + 0.5;

        float4 clipPrev = mul(world_to_clip_prev, float4(worldPos, 1.0));
        float2 uvPrev = clipPrev.xy / clipPrev.w * 0.5 + 0.5;

        motionVectorOutput[pixel] = uvPrev - uvCurr;
    }
    else
    {
        // Sky pixel: project far point along view direction
        float2 pixel_center = (float2(pixel) + 0.5) / (float2(camera.resolution) - 1);
        pixel_center.y = 1.0 - pixel_center.y;
        float3 skyDir = normalize(camera.lowerLeft + pixel_center.x * camera.max_u + pixel_center.y * camera.max_v - camera.position);
        float3 farPoint = camera.position + skyDir * 10000.0;

        float4 clipCurr = mul(world_to_clip, float4(farPoint, 1.0));
        float2 uvCurr = clipCurr.xy / clipCurr.w * 0.5 + 0.5;

        float4 clipPrev = mul(world_to_clip_prev, float4(farPoint, 1.0));
        float2 uvPrev = clipPrev.xy / clipPrev.w * 0.5 + 0.5;

        motionVectorOutput[pixel] = uvPrev - uvCurr;
    }

    // Motion vector debug visualization (DebugMode::MotionVectors = 12)
    if (debug_mode == 12)
    {
        float2 mv = motionVectorOutput[pixel];
        float magnitude = length(mv * float2(camera.resolution));
        float t = saturate(magnitude / 20.0);
        float3 color = lerp(float3(0, 0, 1), float3(1, 0, 0), t);
        imageData[pixel] = float4(color, 1.0);
        return;
    }

    // Write combined output to separate PT accumulation buffer.
    // The running average reads from ptAccumulation (not imageData) to avoid
    // contamination from the composite pass which overwrites imageData every frame.
    float moving_average = (float)total_sample_count / (total_sample_count + spp);
    float3 all_samples = lerp(this_combined / float(spp), ptAccumulation[pixel].rgb, moving_average);
    ptAccumulation[pixel] = float4(all_samples, 1.f);
    // Copy to imageData for passthrough/debug modes and tone mapping fallback
    imageData[pixel] = float4(all_samples, 1.f);
}
