#include "ray_trace.h.slang"

#include "cubemap.h.slang"
#include "debug.h.slang"
#include "directional_light.h.slang"
#include "pbr.h.slang"
#include "random.h.slang"
#include "sky_light.h.slang"

// FORCE_OPAQUE is valid only because the pipeline has no alpha testing / any-hit shader.
#define CLOSEST_HIT_FLAGS RAY_FLAG_FORCE_OPAQUE

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
    float4x4 viewProjection;
    float4x4 prevViewProjection;
    uint time_seed;
    float output_limit;
    uint total_sample_count;
    uint spp;
    uint enable_nee;
    uint write_gbuffer;
    uint sky_sampling_resolution;
    uint emissive_triangle_count;
};

[[vk::binding(1, 1)]] RWTexture2D<float4> imageData;
[[vk::binding(2, 1)]] StructuredBuffer<uint> materialIdBuffer;
[[vk::binding(3, 1)]] StructuredBuffer<MaterialParameters> materialBuffer;
[[vk::binding(4, 1)]] TextureCube<float4> skyMap;
[[vk::binding(5, 1)]] SamplerState skyMapSampler;
[[vk::binding(6, 1)]] SamplerState materialTextureSampler;

// Denoiser G-buffer outputs (written only when write_gbuffer != 0). The path tracer carries no denoiser
// math here: it just emits generic primary-hit attributes + the current frame's raw radiance.
[[vk::binding(7, 1)]] RWTexture2D<float4> gRadiance;          // rgb: this-frame mean radiance
[[vk::binding(8, 1)]] RWTexture2D<float4> gNormalDepth;       // xyz: world normal, w: view-space linear z
[[vk::binding(9, 1)]] RWTexture2D<float4> gAlbedoObj;         // rgb: base color, w: object id
[[vk::binding(10, 1)]] RWTexture2D<float4> gMotion;           // xy: screen motion vector, z: is-hit, w: metallic
[[vk::binding(11, 1)]] RWTexture2D<float4> gRadianceSpecular; // rgb: this-frame specular-lobe radiance
[[vk::binding(12, 1)]] RWTexture2D<float4> gSpecAlbedo;       // rgb: pre-integrated specular BRDF (demod factor)

// must match EmissiveTriangles::Triangle
struct EmissiveTriangle
{
    float4 vertices[3]; // world space. w: unused
    float pdf_area;     // the probability of sampling this triangle over its area
    uint instance_id;
    uint triangle_id;
};

// next event estimation: an alias table over the sky's cube map texels (sky_sampling_resolution^2 per face), and one
// over the scene's emissive triangles, with each instance's first triangle in it (UINT_MAX if it emits nothing)
[[vk::binding(13, 1)]] StructuredBuffer<AliasTableEntry> skySamplingTable;
[[vk::binding(14, 1)]] StructuredBuffer<EmissiveTriangle> emissiveTriangles;
[[vk::binding(15, 1)]] StructuredBuffer<AliasTableEntry> emissiveSamplingTable;
[[vk::binding(16, 1)]] StructuredBuffer<uint> emissiveTriangleOffsets;

// set 2: bindless resources, shared by the whole scene
[[vk::binding(0, 2)]] Texture2D textures[];
[[vk::binding(1, 2)]] StructuredBuffer<uint> indexBuffers[];
[[vk::binding(2, 2)]] StructuredBuffer<VertexAttribute> vertexAttributeBuffers[];

float3 SampleNormal(uint texture_id, float2 tex_coord)
{
    return textures[texture_id].SampleLevel(materialTextureSampler, tex_coord, 0).xyz * 2 - 1;
}

float3 SampleSkyLight(float3 direction)
{
    if (sky_light.has_sky_map != 0)
    {
        float3 light = skyMap.SampleLevel(skyMapSampler, direction, 0).rgb;

        return clamp(light, 0.f, output_limit);
    }

    return 0;
}

float3 SampleEmissive(float2 tex_coord, MaterialParameters mat_param)
{
    float3 emissive_color_sampled =
        mat_param.emissive_texture >= 0
            ? textures[mat_param.emissive_texture].SampleLevel(materialTextureSampler, tex_coord, 0).rgb
            : float3(1, 1, 1);
    return emissive_color_sampled * mat_param.emissive_color;
}

float3 SampleSurface(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float3 w_i,
                     out float out_pdf, out bool is_specular_lobe)
{
    if (mat_param.eta > 0)
    {
        return BxdfDieletric(w_o, w_i, out_pdf, is_specular_lobe, mat_param, intersection);
    }
    else
    {
        return SamplePbrBsdf(w_o, w_i, out_pdf, is_specular_lobe, mat_param, intersection);
    }
}

bool IsRayOccluded(RayDesc ray_desc)
{
    // FORCE_OPAQUE is valid only because the pipeline has no alpha testing / any-hit shader.
    const uint occlusion_flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE;
    RayQuery<occlusion_flags> ray_query;
    ray_query.TraceRayInline(tlas, occlusion_flags, 0xFF, ray_desc);
    while (ray_query.Proceed())
    {
    }

    return ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#define BaryLerp(v0, v1, v2, uv) (v0 + (v1 - v0) * uv.x + (v2 - v0) * uv.y)

#define AccessFaceAttribute(triangle_id, buf, vertex_idx) (buf[triangle_id * 3 + vertex_idx])

VertexAttribute GetCornerAttribute(uint instance_id, uint triangle_id, uint corner)
{
    return vertexAttributeBuffers[instance_id][AccessFaceAttribute(triangle_id, indexBuffers[instance_id], corner)];
}

float2 GetTexCoord(uint instance_id, uint triangle_id, float2 barycentrics)
{
    return BaryLerp(GetCornerAttribute(instance_id, triangle_id, 0).texCoord,
                    GetCornerAttribute(instance_id, triangle_id, 1).texCoord,
                    GetCornerAttribute(instance_id, triangle_id, 2).texCoord, barycentrics);
}

// a light sample for next event estimation. pdf is over solid angle and includes the choice of the light
struct LightSample
{
    float3 radiance;
    float3 direction;
    float distance;
    float pdf;
};

bool HasSkySampling()
{
    return sky_light.has_sky_map != 0 && sky_sampling_resolution > 0;
}

// next event estimation picks the sky or an emissive triangle, half and half when the scene has both
float SkySelectionProbability()
{
    if (!HasSkySampling())
    {
        return 0;
    }

    return emissive_triangle_count > 0 ? 0.5f : 1.f;
}

// a perfect dielectric scatters only into its two delta lobes, which a light sample never hits
bool IsLightSampled(MaterialParameters mat_param)
{
    return enable_nee != 0 && mat_param.eta <= 0 && (HasSkySampling() || emissive_triangle_count > 0);
}

// the solid angle density at uv of a point uniform within its cube map texel
float CubeMapTexelDensity(float2 uv)
{
    float2 st = uv * 2 - 1;
    float resolution = sky_sampling_resolution;
    return pow(1 + dot(st, st), 1.5f) * resolution * resolution * 0.25f;
}

LightSample SampleSkyDirection()
{
    const uint resolution = sky_sampling_resolution;
    const uint face_size = resolution * resolution;

    float probability;
    uint texel = SampleAliasTable(skySamplingTable, face_size * 6, probability);
    uint face_id = texel / face_size;
    uint2 xy = uint2(texel % resolution, (texel % face_size) / resolution);
    float2 uv = (float2(xy) + float2(RandomUnit(), RandomUnit())) / resolution;

    LightSample light;
    light.direction = GetDirectionFromCubeMapUV(uv, face_id);
    light.radiance = SampleSkyLight(light.direction);
    light.distance = 1000.0;
    light.pdf = SkySelectionProbability() * probability * CubeMapTexelDensity(uv);
    return light;
}

// the pdf of SampleSkyDirection choosing a direction an escaped ray took
float SkyLightPdf(float3 direction)
{
    if (!HasSkySampling())
    {
        return 0;
    }

    const uint resolution = sky_sampling_resolution;
    uint face_id;
    float2 uv = GetCubeMapUVFromDirection(direction, face_id);
    uint2 xy = min(uint2(uv * resolution), resolution - 1);
    float probability = skySamplingTable[(face_id * resolution + xy.y) * resolution + xy.x].probability;
    return SkySelectionProbability() * probability * CubeMapTexelDensity(uv);
}

// converts a triangle's area density at a point to solid angle density as seen from distance squared away
float AreaToSolidAngle(EmissiveTriangle emitter, float3 direction, float distance_squared)
{
    float3 normal =
        cross(emitter.vertices[1].xyz - emitter.vertices[0].xyz, emitter.vertices[2].xyz - emitter.vertices[0].xyz);
    // emitters are two-sided
    float cos_light = abs(dot(normal, direction)) / length(normal);
    return emitter.pdf_area * distance_squared / max(cos_light, Eps);
}

LightSample SampleEmissivePoint(float3 position)
{
    float probability;
    EmissiveTriangle emitter =
        emissiveTriangles[SampleAliasTable(emissiveSamplingTable, emissive_triangle_count, probability)];

    // uniform on the triangle: the weights of vertex 1 and 2
    float r = sqrt(RandomUnit());
    float u = RandomUnit();
    float2 barycentrics = float2(u * r, (1 - u) * r);
    float3 light_position =
        BaryLerp(emitter.vertices[0].xyz, emitter.vertices[1].xyz, emitter.vertices[2].xyz, barycentrics);

    float3 to_light = light_position - position;
    float distance_squared = dot(to_light, to_light);

    LightSample light;
    light.distance = sqrt(distance_squared);
    light.direction = to_light / light.distance;
    MaterialParameters mat_param = materialBuffer[materialIdBuffer[emitter.instance_id]];
    light.radiance = SampleEmissive(GetTexCoord(emitter.instance_id, emitter.triangle_id, barycentrics), mat_param);
    light.pdf = (1 - SkySelectionProbability()) * AreaToSolidAngle(emitter, light.direction, distance_squared);
    return light;
}

// the pdf of SampleEmissivePoint choosing the emitter point a ray from origin hit
float EmissiveLightPdf(float3 origin, Intersection intersection)
{
    if (emissive_triangle_count == 0)
    {
        return 0;
    }

    uint offset = emissiveTriangleOffsets[intersection.primitiveId];
    if (offset == 0xFFFFFFFFu)
    {
        return 0;
    }

    float3 to_light = intersection.position - origin;
    float distance_squared = dot(to_light, to_light);
    return (1 - SkySelectionProbability()) * AreaToSolidAngle(emissiveTriangles[offset + intersection.triangleId],
                                                              to_light * rsqrt(distance_squared), distance_squared);
}

// the power heuristic weight of a strategy with pdf > 0 against another strategy for the same sample
float PowerHeuristic(float pdf, float other_pdf)
{
    if (other_pdf > pdf)
    {
        float ratio = pdf / other_pdf;
        float r2 = ratio * ratio;
        return r2 / (1 + r2);
    }

    float ratio = other_pdf / pdf;
    return 1 / (1 + ratio * ratio);
}

// next event estimation: one light sample, weighted against BSDF sampling. specular_out is its specular-lobe part
float3 EstimateDirectLight(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float3 specular_out)
{
    specular_out = (float3)0;

    LightSample light =
        RandomUnit() < SkySelectionProbability() ? SampleSkyDirection() : SampleEmissivePoint(intersection.position);
    PbrBsdfValue bsdf = EvaluatePbrBsdf(intersection.normal, w_o, light.direction, mat_param);
    if (bsdf.pdf <= 0 || light.pdf <= 0 || light.distance <= 2 * Tolerance || all(light.radiance == 0))
    {
        return 0;
    }

    RayDesc ray_desc;
    ray_desc.Origin = intersection.position;
    ray_desc.Direction = light.direction;
    ray_desc.TMin = Tolerance;
    ray_desc.TMax = light.distance - Tolerance;
    if (IsRayOccluded(ray_desc))
    {
        return 0;
    }

    float3 weighted_radiance = light.radiance * (PowerHeuristic(light.pdf, bsdf.pdf) / light.pdf);
    specular_out = bsdf.specular * weighted_radiance;
    return (bsdf.specular + bsdf.diffuse) * weighted_radiance;
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

void GetIntersection(RayDesc ray_desc, RayQuery<CLOSEST_HIT_FLAGS> ray_query, out Intersection intersection)
{
    const float2 intersection_uv = ray_query.CommittedTriangleBarycentrics();
    const uint instance_id = ray_query.CommittedInstanceID();
    const uint triangle_id = ray_query.CommittedPrimitiveIndex();
    const float4x3 inv_transform = ray_query.CommittedWorldToObject4x3();

    const VertexAttribute va0 = GetCornerAttribute(instance_id, triangle_id, 0);
    const VertexAttribute va1 = GetCornerAttribute(instance_id, triangle_id, 1);
    const VertexAttribute va2 = GetCornerAttribute(instance_id, triangle_id, 2);

    // parametric hit position: CommittedObjectToWorld4x3 is unreliable on some drivers (Adreno)
    intersection.position = ray_desc.Origin + ray_desc.Direction * ray_query.CommittedRayT();
    intersection.inv_transform = inv_transform;
    intersection.normal = BaryLerp(va0.normal, va1.normal, va2.normal, intersection_uv);
    intersection.tangent = BaryLerp(va0.tangent, va1.tangent, va2.tangent, intersection_uv);
    intersection.texCoord = BaryLerp(va0.texCoord, va1.texCoord, va2.texCoord, intersection_uv);
    intersection.materialId = materialIdBuffer[instance_id];
    intersection.primitiveId = instance_id;
    intersection.triangleId = triangle_id;
}

// Karis' analytic split-sum environment BRDF approximation (no LUT). Returns (scale, bias) so the
// pre-integrated specular reflectance is F0*scale + bias. Used as the specular demodulation factor:
// the denoiser divides the specular radiance by it before denoising and multiplies back after (the
// factor is a per-pixel constant from the noise-free center ray, so the round-trip conserves energy
// exactly).
float2 EnvBRDFApprox(float NoV, float roughness)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

// Trace a single noise-free primary ray (pixel center, no anti-aliasing jitter) and emit the
// primary-hit G-buffer. Separate from the noisy path integrator on purpose: the denoiser requires a
// clean G-buffer.
void WritePrimaryHitGBuffer(uint2 pixel)
{
    float2 pixel_size = 1.f / (float2(camera.resolution) - 1);
    float2 uv = (float2(pixel) + 0.5f) * pixel_size;
    uv.y = 1.0f - uv.y;

    RayDesc ray_desc;
    ray_desc.Origin = camera.position;
    ray_desc.Direction = normalize(camera.lowerLeft + uv.x * camera.max_u + uv.y * camera.max_v - ray_desc.Origin);
    ray_desc.TMin = Tolerance;
    ray_desc.TMax = 1000.0f;

    RayQuery<CLOSEST_HIT_FLAGS> ray_query;
    ray_query.TraceRayInline(tlas, CLOSEST_HIT_FLAGS, 0xFF, ray_desc);
    while (ray_query.Proceed())
    {
    }

    float3 normal = float3(0, 0, 0);
    float3 albedo = float3(0, 0, 0);
    float3 spec_albedo = float3(0, 0, 0);
    float spec_roughness = 1.f;
    float linear_z = 0.f;
    float object_id = 0.f;
    float2 motion = float2(0, 0);
    float is_hit = 0.f;
    float metallic = 0.f;

    if (ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        Intersection intersection;
        GetIntersection(ray_desc, ray_query, intersection);
        MaterialParameters mat_param = GetMaterial(intersection);

        normal = normalize(intersection.normal);
        albedo = mat_param.base_color;
        object_id = float(intersection.primitiveId);

        // pre-integrated specular BRDF: the demodulation factor for the specular channel
        float3 f0 = ComputeF0(mat_param.base_color, mat_param.metallic);
        float n_dot_v = saturate(dot(normal, -ray_desc.Direction));
        float2 env_brdf = EnvBRDFApprox(n_dot_v, mat_param.roughness);
        spec_albedo = f0 * env_brdf.x + env_brdf.y;
        spec_roughness = mat_param.roughness;
        metallic = mat_param.metallic;

        // view-space linear depth: distance along the camera forward axis
        float3 front = normalize(cross(camera.max_v, camera.max_u));
        linear_z = dot(intersection.position - camera.position, front);

        // screen-space motion vector: where this surface projected last frame, minus this frame.
        // Both endpoints use the same projection of the same world point, so a static camera yields
        // exactly zero motion (no per-frame reprojection drift / temporal blur).
        float4 prev_clip = mul(prevViewProjection, float4(intersection.position, 1.0f));
        float4 cur_clip = mul(viewProjection, float4(intersection.position, 1.0f));
        float2 prev_uv = prev_clip.xy / prev_clip.w * 0.5f + 0.5f;
        float2 cur_uv = cur_clip.xy / cur_clip.w * 0.5f + 0.5f;
        motion = prev_uv - cur_uv;

        // 2 = primary-hit emissive: terminal and deterministic (the accumulator equals it exactly),
        // so the NRD chain bypasses it instead of blurring its texture detail (see nrd_pack/nrd_resolve).
        float3 emissive = SampleEmissive(intersection.texCoord, mat_param);
        is_hit = dot(emissive, emissive) > Eps * Eps ? 2.f : 1.f;
    }

    gNormalDepth[pixel] = float4(normal, linear_z);
    gAlbedoObj[pixel] = float4(albedo, object_id);
    gMotion[pixel] = float4(motion, is_hit, metallic);
    gSpecAlbedo[pixel] = float4(spec_albedo, spec_roughness);
}

float3 SamplePixel(RayDesc ray_desc, out float3 out_specular, out float out_diff_hit_dist, out float out_spec_hit_dist)
{
    out_specular = float3(0, 0, 0);
    out_diff_hit_dist = 0.f;
    out_spec_hit_dist = 0.f;

    // NRD hit distance: length of the first indirect ray (primary hit -> first bounce). Sky at that bounce
    // means "no nearby occluder" -> the far distance. Routed below to the channel the primary lobe sampled.
    float first_hit_dist = 0.f;

    RayQuery<CLOSEST_HIT_FLAGS> ray_query;
    Intersection intersection;

    float3 pixel_color = float3(0, 0, 0);
    float3 throughput = float3(1, 1, 1);
    int bounce;

    // Specular-channel attribution: the part of pixel_color reflected through the primary hit's specular
    // lobe (its NEE specular term + everything indirect when the primary bounce sampled specular). The
    // denoiser demodulates/denoises this on a separate channel from the diffuse remainder
    // (pixel_color - specular).
    float3 specular_color = float3(0, 0, 0);
    bool primary_specular = false;

    // the pdf of the BSDF sample that set the ray's direction, which MIS weighs against light sampling where the ray
    // hits a light. a camera ray or a ray after a vertex without light sampling takes the light's full weight
    float bsdf_pdf = 0.f;
    bool light_sampled = false;

    [loop] for (bounce = 0; bounce < camera.max_bounce; bounce++)
    {
        // initialize ray query
        ray_query.TraceRayInline(tlas, CLOSEST_HIT_FLAGS, 0xFF, ray_desc);

        // wait for ray query completion
        while (ray_query.Proceed())
        {
        }

        if (bounce == 1)
        {
            first_hit_dist =
                (ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? ray_query.CommittedRayT() : ray_desc.TMax;
        }

        // terminal condition: hit sky
        if (ray_query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            float3 sky_light = SampleSkyLight(ray_desc.Direction);
            float mis_weight = light_sampled ? PowerHeuristic(bsdf_pdf, SkyLightPdf(ray_desc.Direction)) : 1.f;
            float3 contribution = sky_light * throughput * mis_weight;
            pixel_color += contribution;
            // primary ray seeing the environment directly stays on the diffuse channel (bounce 0);
            // deeper bounces inherit the primary lobe's channel.
            if (bounce > 0 && primary_specular)
            {
                specular_color += contribution;
            }
            break;
        }

        // get intersection details
        GetIntersection(ray_desc, ray_query, intersection);
        MaterialParameters mat_param = GetMaterial(intersection);

        // terminal condition: emissive
        float3 emissive_color = SampleEmissive(intersection.texCoord, mat_param);
        if (dot(emissive_color, emissive_color) > Eps * Eps)
        {
            float mis_weight =
                light_sampled ? PowerHeuristic(bsdf_pdf, EmissiveLightPdf(ray_desc.Origin, intersection)) : 1.f;
            float3 contribution = emissive_color * throughput * mis_weight;
            pixel_color += contribution;
            if (bounce > 0 && primary_specular)
            {
                specular_color += contribution;
            }
            break;
        }

        // core function: surface sampling
        float3 next_direction;
        float next_bsdf_pdf = 0.f;
        bool is_specular_lobe = false;
        float3 this_throughput = SampleSurface(intersection, mat_param, -ray_desc.Direction, next_direction,
                                               next_bsdf_pdf, is_specular_lobe);

        // the primary bounce's lobe selects which channel all subsequent (indirect) radiance lands on.
        if (bounce == 0)
        {
            primary_specular = is_specular_lobe;
        }

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
            return mat_param.base_color;
        case 10:
            return emissive_color;
        case 11: {
            float3 front = normalize(cross(camera.max_v, camera.max_u));
            return ((float3)1) * dot(intersection.position - camera.position, front) / ray_desc.TMax;
        }
        default:
            break;
        }

        // a light reached from here would end a path one segment longer than max_bounce allows
        if (bounce + 1 == camera.max_bounce)
        {
            break;
        }

        light_sampled = IsLightSampled(mat_param);
        if (light_sampled)
        {
            float3 nee_specular;
            float3 nee_radiance = EstimateDirectLight(intersection, mat_param, -ray_desc.Direction, nee_specular);
            pixel_color += nee_radiance * throughput;

            // at the primary hit, split the direct light by its own lobes; deeper hits are already on the
            // primary lobe's channel, so the whole indirect NEE contribution inherits it.
            if (bounce == 0)
            {
                specular_color += nee_specular * throughput;
            }
            else if (primary_specular)
            {
                specular_color += nee_radiance * throughput;
            }
        }
        bsdf_pdf = next_bsdf_pdf;

        throughput *= this_throughput;

        // terminal condition: early out
        if (dot(throughput, throughput) < Eps * Eps)
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

    out_specular = specular_color;
    out_diff_hit_dist = primary_specular ? 0.f : first_hit_dist;
    out_spec_hit_dist = primary_specular ? first_hit_dist : 0.f;
    return pixel_color;
}

// Average `spp` path samples for `pixel`, seeded by `seed`.
float3 ComputeFrameRadiance(uint2 pixel, uint seed, float2 pixel_size, out float3 specular_radiance,
                            out float diff_hit_dist, out float spec_hit_dist)
{
    float3 this_samples = (float3)0;
    float3 specular_samples = (float3)0;
    float diff_hd_samples = 0.f;
    float spec_hd_samples = 0.f;
    float diff_hd_count = 0.f;
    float spec_hd_count = 0.f;

    InitRandomBase(pixel.x, pixel.y, seed);

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

        float3 this_specular;
        float this_diff_hd;
        float this_spec_hd;
        float3 this_pixel = SamplePixel(ray_desc, this_specular, this_diff_hd, this_spec_hd);

        // firefly removal (clamp the specular component to the same ceiling so it stays consistent)
        this_pixel = min(this_pixel, output_limit);
        this_specular = min(this_specular, output_limit);

        this_samples += this_pixel;
        specular_samples += this_specular;
        // in-lobe average (NRD requirement): only samples that SAMPLED the lobe contribute, or zeros from
        // skipped-lobe samples dilute the hit distance by the selection probability at spp > 1.
        if (this_diff_hd > 0.f)
        {
            diff_hd_samples += this_diff_hd;
            diff_hd_count += 1.f;
        }
        if (this_spec_hd > 0.f)
        {
            spec_hd_samples += this_spec_hd;
            spec_hd_count += 1.f;
        }
    }

    specular_radiance = specular_samples / float(spp);
    diff_hit_dist = diff_hd_count > 0.f ? diff_hd_samples / diff_hd_count : 0.f;
    spec_hit_dist = spec_hd_count > 0.f ? spec_hd_samples / spec_hd_count : 0.f;
    return this_samples / float(spp);
}

[shader("compute")][numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = uint2(DTid.xy);

    // bound check
    if (pixel.x >= camera.resolution.x || pixel.y >= camera.resolution.y)
    {
        return;
    }

    if (write_gbuffer != 0)
    {
        WritePrimaryHitGBuffer(pixel);
    }

    float2 pixel_size = 1.f / (float2(camera.resolution) - 1);

    float3 this_frame_specular;
    float this_frame_diff_hd;
    float this_frame_spec_hd;
    float3 this_frame_radiance =
        ComputeFrameRadiance(pixel, time_seed, pixel_size, this_frame_specular, this_frame_diff_hd, this_frame_spec_hd);

    if (write_gbuffer != 0)
    {
        // per-frame raw radiance for the denoiser (not the accumulated average). gRadiance carries the
        // full radiance; gRadianceSpecular carries its specular-lobe part so it can be denoised on a
        // separate demodulated channel (the diffuse part is gRadiance - gRadianceSpecular). .w =
        // first-bounce hit distance per channel (NRD's radiance+hitDist packing).
        gRadiance[pixel] = float4(this_frame_radiance, this_frame_diff_hd);
        gRadianceSpecular[pixel] = float4(this_frame_specular, this_frame_spec_hd);
    }

    float3 all_samples = imageData[pixel].rgb;
    float moving_average = (float)total_sample_count / (total_sample_count + spp);
    all_samples = lerp(this_frame_radiance, all_samples, moving_average);

    // output
    imageData[pixel] = float4(all_samples, 1.f);
}
