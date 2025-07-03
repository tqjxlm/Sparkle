#ifndef DEBUG_H_
#define DEBUG_H_

// a global debug color that can be set from anywhere
static float4 debug_color = (float4)0;

float3 VisualizeVector(float3 v)
{
    return (v + 1) * 0.5;
}

float3 PickColorPalette(int number)
{
    if (number >= 7)
    {
        return float3(0, 0, 0); // Black for out of bounds
    }

    // High contrast colors from https://www.schemecolor.com/high-contrast.php
    float3 palette[7] = {
        float3(137, 49, 239), // Purple
        float3(242, 202, 25), // Yellow
        float3(255, 0, 189),  // Magenta
        float3(0, 87, 233),   // Blue
        float3(135, 233, 17), // Green
        float3(225, 24, 69),  // Red
        float3(255, 255, 255) // White
    };

    return palette[number] / 255.0f;
}

#endif // DEBUG_H_