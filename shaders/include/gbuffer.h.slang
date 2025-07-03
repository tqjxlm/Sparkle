#ifndef GBUFFER_H_
#define GBUFFER_H_

#include "math.h.slang"
#include "surface.h.slang"

uint EncodeEmissive(float3 rgb)
{
    // 5-bit exponent + 9-bit RGB mantissas

    float r = rgb.r;
    float g = rgb.g;
    float b = rgb.b;
    float max_component = max(max(r, g), b);

    // Handle zero or very small values
    if (max_component <= 1e-7)
    {
        return 0;
    }

    // Calculate the shared exponent
    float exponent = ceil(log2(max_component));
    uint shared_exp = clamp(uint(exponent + 15.0), 0, 31);

    // Compute scaling factor to normalize mantissas
    float scale = exp2(15.0 - shared_exp) * GetBitMask(9);

    // Compute mantissas and clamp to 9-bit range
    uint red = min(GetBitMask(9), uint(round(r * scale)));
    uint green = min(GetBitMask(9), uint(round(g * scale)));
    uint blue = min(GetBitMask(9), uint(round(b * scale)));

    // Pack components into 32-bit value
    uint encoded = red;            // Bits [0-8]
    encoded |= (green << 9);       // Bits [9-17]
    encoded |= (blue << 18);       // Bits [18-26]
    encoded |= (shared_exp << 27); // Bits [27-31]
    return encoded;
}

float3 DecodeEmissive(uint encoded)
{
    // 5-bit exponent + 9-bit RGB mantissas

    uint exponent = (encoded >> 27) & GetBitMask(5); // Bits [27-31]
    uint blue = (encoded >> 18) & GetBitMask(9);     // Bits [18-26]
    uint green = (encoded >> 9) & GetBitMask(9);     // Bits [9-17]
    uint red = encoded & GetBitMask(9);              // Bits [0-8]

    // Calculate shared scale factor
    float shared_exp = exp2(float(exponent) - 15.0);

    // Normalize mantissas and apply exponent scale
    const float inv_mantissa = 1.0 / 511.0; // 9-bit max (2^9 - 1)
    float3 rgb;
    rgb.r = float(red) * inv_mantissa * shared_exp;
    rgb.g = float(green) * inv_mantissa * shared_exp;
    rgb.b = float(blue) * inv_mantissa * shared_exp;

    return rgb;
}

float2 NormalToOctahedral(float3 normal)
{
    // Octahedral projection
    normal /= (abs(normal.x) + abs(normal.y) + abs(normal.z));

    // Wrap to upper hemisphere
    float2 octahedral = normal.z >= 0.0 ? normal.xy : ((1.0 - abs(normal.yx)) * select(normal.xy >= 0.0, 1.0, -1.0));

    // Map from [-1,1] to [0,1]
    octahedral = octahedral * 0.5 + 0.5;

    return octahedral;
}

float3 OctahedralToNormal(float2 octahedral)
{
    // Map from [0,1] to [-1,1]
    octahedral = octahedral * 2.0 - 1.0;

    // Reconstruct normal from octahedral coordinates
    float3 normal = float3(octahedral.x, octahedral.y, 1.0 - abs(octahedral.x) - abs(octahedral.y));

    // Handle lower hemisphere wrap
    if (normal.z < 0.0)
    {
        normal.xy = (1.0 - abs(normal.yx)) * select(normal.xy >= 0.0, 1.0, -1.0);
    }

    normal = normalize(normal);

    return normal;
}

uint EncodeNormalRoughnessMetallic(float3 normal, float roughness, float metallic)
{
    // R10G10B10A2_UNORM

    float2 octahedral = NormalToOctahedral(normal);
    uint enc_normal_x = Float2UNorm(octahedral.x, 10);
    uint enc_normal_y = Float2UNorm(octahedral.y, 10);

    uint enc_roughness = Float2UNorm(roughness, 10);

    uint enc_metallic = Float2UNorm(metallic, 2);

    uint encoded = 0;
    encoded |= (enc_normal_x & GetBitMask(10)) << 0;
    encoded |= (enc_normal_y & GetBitMask(10)) << 10;
    encoded |= (enc_roughness & GetBitMask(10)) << 20;
    encoded |= (enc_metallic & GetBitMask(2)) << 30;

    return encoded;
}

void DecodeNormalRoughnessMetallic(uint encoded, out float3 normal, out float roughness, out float metallic)
{
    // R10G10B10A2_UNORM

    uint dec_normal_x = (encoded >> 0) & GetBitMask(10);
    uint dec_normal_y = (encoded >> 10) & GetBitMask(10);
    uint dec_roughness = (encoded >> 20) & GetBitMask(10);
    uint dec_metallic = (encoded >> 30) & GetBitMask(2);

    float2 octahedral = float2(dec_normal_x, dec_normal_y) / GetBitMask(10);
    normal = OctahedralToNormal(octahedral);

    roughness = float(dec_roughness) / GetBitMask(10);

    metallic = float(dec_metallic) / GetBitMask(2);
}

uint EncodeAlbedo(float3 albedo)
{
    // R8G8B8_UNOFRM

    uint enc_albedo_r = Float2UNorm(albedo.r, 8);
    uint enc_albedo_g = Float2UNorm(albedo.g, 8);
    uint enc_albedo_b = Float2UNorm(albedo.b, 8);

    uint encoded = 0;
    encoded |= (enc_albedo_r & GetBitMask(8)) << 0;
    encoded |= (enc_albedo_g & GetBitMask(8)) << 8;
    encoded |= (enc_albedo_b & GetBitMask(8)) << 16;

    return encoded;
}

float3 DecodeAlbedo(uint encoded)
{
    // R8G8B8_UNOFRM

    uint dec_albedo_r = (encoded >> 0) & GetBitMask(8);
    uint dec_albedo_g = (encoded >> 8) & GetBitMask(8);
    uint dec_albedo_b = (encoded >> 16) & GetBitMask(8);

    return float3(dec_albedo_r, dec_albedo_g, dec_albedo_b) / GetBitMask(8);
}

uint4 EncodeSurfaceToGBuffer(SurfaceAttributes surface)
{
    uint4 encoded;

    // 20 bits normal + 10 bits roughness + 2 bits metallic
    encoded.x = EncodeNormalRoughnessMetallic(surface.normal, surface.roughness, surface.metallic);

    // 24 bits albedo + (8 bits reserved)
    encoded.y = EncodeAlbedo(surface.albedo);

    // 32 bit emissive
    encoded.z = EncodeEmissive(surface.emissive);

    return encoded;
}

SurfaceAttributes DecodeSurfaceFromGBuffer(uint4 gbuffer_sample)
{
    SurfaceAttributes surface;

    // 20 bits normal + 10 bits roughness + 2 bits metallic
    DecodeNormalRoughnessMetallic(gbuffer_sample.x, surface.normal, surface.roughness, surface.metallic);

    // 24 bits albedo + (8 bits reserved)
    surface.albedo = DecodeAlbedo(gbuffer_sample.y);

    // 32 bit emissive
    surface.emissive = DecodeEmissive(gbuffer_sample.z);

    return surface;
}

#endif // GBUFFER_H_