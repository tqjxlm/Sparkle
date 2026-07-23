#include "ray_trace.h.slang"

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
    float2 frame_jitter;
    uint use_frame_jitter;
    uint padding;
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
                     out float out_pdf, out bool is_specular_lobe)
{
    if (mat_param.eta > 0)
    {
        return BxdfDieletric(w_o, w_i, out_pdf, is_specular_lobe, mat_param, intersection);
    }
    else
    {
        return BxdfMetal(w_o, w_i, out_pdf, is_specular_lobe, mat_param, intersection);
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

float3 SampleSkyLight(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float out_pdf,
                      out float3 specular_out)
{
    specular_out = (float3)0;

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

    // evaluate brdf, split into diffuse/specular lobes (the denoiser demodulates them separately)
    float3 diffuse_radiance;
    float3 specular_radiance;
    float3 radiance =
        CalculateDirectLightingSplit(w_o, w_i, light_sample, surface, diffuse_radiance, specular_radiance);

    float weight = SaturateDot(w_i, intersection.normal) / (out_pdf + Eps);
    specular_out = specular_radiance * weight;
    return radiance * weight;
}

float3 SampleLights(Intersection intersection, MaterialParameters mat_param, float3 w_o, out float out_pdf,
                    out float3 specular_out)
{
    // only consider sky light for now
    return SampleSkyLight(intersection, mat_param, w_o, out_pdf, specular_out);
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

void GetIntersection(RayDesc ray_desc, RayQuery<CLOSEST_HIT_FLAGS> ray_query, out Intersection intersection)
{
    const float2 intersection_uv = ray_query.CommittedTriangleBarycentrics();
    const uint instance_id = ray_query.CommittedInstanceID();
    const uint triangle_id = ray_query.CommittedPrimitiveIndex();
    const float4x3 inv_transform = ray_query.CommittedWorldToObject4x3();

    uint index0 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 0);
    uint index1 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 1);
    uint index2 = AccessFaceAttribute(triangle_id, indexBuffers[instance_id], 2);

    const VertexAttribute va0 = GetVertexAttribute(instance_id, index0);
    const VertexAttribute va1 = GetVertexAttribute(instance_id, index1);
    const VertexAttribute va2 = GetVertexAttribute(instance_id, index2);

    // parametric hit position: CommittedObjectToWorld4x3 is unreliable on some drivers (Adreno)
    intersection.position = ray_desc.Origin + ray_desc.Direction * ray_query.CommittedRayT();
    intersection.inv_transform = inv_transform;
    intersection.normal = BaryLerp(va0.normal, va1.normal, va2.normal, intersection_uv);
    intersection.tangent = BaryLerp(va0.tangent, va1.tangent, va2.tangent, intersection_uv);
    intersection.texCoord = BaryLerp(va0.texCoord, va1.texCoord, va2.texCoord, intersection_uv);
    intersection.materialId = materialIdBuffer[instance_id];
    intersection.primitiveId = instance_id;
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
    float2 primary_jitter = use_frame_jitter != 0 ? frame_jitter : float2(0.f, 0.f);
    float2 uv = (float2(pixel) + 0.5f + primary_jitter) * pixel_size;
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
        float3 emissive = SampleEmissive(intersection, mat_param);
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

    // When we introduce NEE, non-NEE paths should be weighted down
    float bsdf_mis_weight = 1.f;

    [loop] for (bounce = 0; bounce <= camera.max_bounce; bounce++)
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
            float3 contribution = sky_light * throughput * bsdf_mis_weight;
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
        float3 emissive_color = SampleEmissive(intersection, mat_param);
        if (dot(emissive_color, emissive_color) > Eps * Eps)
        {
            float3 contribution = emissive_color * throughput * bsdf_mis_weight;
            pixel_color += contribution;
            if (bounce > 0 && primary_specular)
            {
                specular_color += contribution;
            }
            break;
        }

        // core function: surface sampling
        float3 next_direction;
        float bsdf_pdf = 1.f;
        bool is_specular_lobe = false;
        float3 this_throughput =
            SampleSurface(intersection, mat_param, -ray_desc.Direction, next_direction, bsdf_pdf, is_specular_lobe);

        // the primary bounce's lobe selects which channel all subsequent (indirect) radiance lands on.
        if (bounce == 0)
        {
            primary_specular = is_specular_lobe;
        }

        if (enable_nee != 0)
        {
            // next event estimation: explicitly sample light sources every bounce
            float light_pdf = 0.f;
            float3 nee_specular = float3(0, 0, 0);
            float3 nee_radiance = SampleLights(intersection, mat_param, -ray_desc.Direction, light_pdf, nee_specular);
            float light_mis_weight = GetMISWeight(light_pdf, bsdf_pdf);
            pixel_color += nee_radiance * throughput * light_mis_weight;

            // at the primary hit, split the direct light by its own lobes; deeper hits are already on the
            // primary lobe's channel, so the whole indirect NEE contribution inherits it.
            if (bounce == 0)
            {
                specular_color += nee_specular * throughput * light_mis_weight;
            }
            else if (primary_specular)
            {
                specular_color += nee_radiance * throughput * light_mis_weight;
            }

            // update mis weight for next bounce
            bsdf_mis_weight = GetMISWeight(bsdf_pdf, GetLightPdf(intersection.normal, next_direction));
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
        float2 antialiasing_noise = use_frame_jitter != 0 ? 0.5f + frame_jitter : float2(RandomUnit(), RandomUnit());
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
