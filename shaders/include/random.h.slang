#ifndef RANDOM_H_
#define RANDOM_H_

#include "math.h.slang"

static uint random_base;

uint rand_pcg()
{
    uint state = random_base;
    random_base = random_base * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float RandomUnit()
{
    return rand_pcg() / 4294967296.0f;
}

float2 UnitDisk()
{
    float rand = RandomUnit();
    float theta = RandomUnit() * 2.0f * Pi;

    float r = sqrt(rand);
    float x = r * cos(theta);
    float y = r * sin(theta);

    return float2(x, y);
}

float3 SampleCosineWeightedHemisphere()
{
    float2 unit_disk = UnitDisk();

    // Project z up to the unit hemisphere
    float z = sqrt(1.0f - dot(unit_disk, unit_disk));

    return normalize(float3(unit_disk, z));
}

// initial random number considering the pixel position and a seed (usually time)
void InitRandomBase(uint x, uint y, uint seed)
{
    // there may be overflow but it is ok since we do not really care about values
    seed += x * 1664525u + y * 214013u;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    random_base = seed;
}

#endif // RANDOM_H_