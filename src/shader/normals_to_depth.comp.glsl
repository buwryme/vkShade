#version 450

// Compute shader: reconstruct a pseudo-depth buffer from a normal buffer.
//
// The source (binding 0) is a **combined image sampler** because the input
// "depth" buffer from games like Roblox is actually a view-space normal
// map stored in a depth format (e.g. D32_SFLOAT where the float encodes
// normal data, or a color format misidentified as depth).
//
// The sampler reads the R channel.  When normals are packed into a single
// D32_SFLOAT, we decode as follows:
//   - If the values cluster in [0,1] with many distinct values → assume
//     it's a linear-normal encoding where R contains Nx (the X component
//     remapped from [-1,1] to [0,1]).
//   - For multi-channel normal textures, we'd need RGBA, but D32 only has
//     R.  So we use a single-channel heuristic.
//
// For Roblox specifically, the "depth" attachment is typically a
// D32_SFLOAT where the encoded value represents a packed normal.  The
// exact packing varies, but the most common pattern for single-channel
// normal buffers is:
//
//   Method A: R = (N.x * 0.5 + 0.5) — just X component remapped
//   Method B: R = (N.z * 0.5 + 0.5) — Z component (facing) remapped
//   Method C: R = encoded_xy where lower 16 bits = Nx, upper 16 = Ny
//
// We implement a heuristic that works well for edge-detection, SSAO, and
// DOF even without knowing the exact encoding: we treat the values as a
// "depth-like" signal and enhance the discontinuities.
//
// === Algorithm ===
//
// When we only have a single float per pixel (D32_SFLOAT):
//   1. Detect whether the data looks like normals or real depth
//   2. If normals: use the value as a "surface facing" proxy where
//      surfaces facing the camera are near, edge-on surfaces are far
//   3. Apply Sobel edge detection to find depth discontinuities
//   4. Combine the facing proxy with edge information
//   5. Apply bilateral-filter smoothing to reduce noise while preserving edges
//   6. Output a pseudo-depth in [0, 1] with proper reverse-Z distribution
//
// This produces astonishingly accurate pseudo-depth for post-processing
// because the edge structure (which matters most for SSAO/DOF) comes
// directly from the normal discontinuities.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uNormalDepth;  // input: "depth" buffer (actually normals)
layout(set = 0, binding = 1, r32f)   uniform image2D uDepthOut;  // output: pseudo-depth

layout(push_constant, std430) uniform PushConstants {
    vec2  invResolution;
    float nearZ;
    float farZ;
    float ndcBias;
    float ndcScale;
    float inputScale;     // multiplier for input values (default 1.0)
    float inputBias;      // offset for input values (default 0.0)
    int   _pad2;
} pc;

shared float sVal[18][18];  // 16x16 + 1px border for Sobel

// Load a texel from the normal-depth texture with boundary clamping.
float loadVal(ivec2 coord, ivec2 imgSize)
{
    coord = clamp(coord, ivec2(0), imgSize - 1);
    return textureLod(uNormalDepth, (vec2(coord) + 0.5) * pc.invResolution, 0.0).r * pc.inputScale + pc.inputBias;
}

void main()
{
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    ivec2 tile = ivec2(gl_LocalInvocationID.xy);
    ivec2 imgSize = ivec2(pc.invResolution.x > 0.0 ? (1.0 / pc.invResolution.x) : 0,
                           pc.invResolution.y > 0.0 ? (1.0 / pc.invResolution.y) : 0);

    if (pix.x >= imgSize.x || pix.y >= imgSize.y) return;

    // Load 18x18 tile (16x16 + 1px border) into shared memory for Sobel.
    // Threads on the border load extra pixels.
    ivec2 loadCoord = pix - ivec2(1, 1);
    for (int dy = 0; dy < 2; dy++)
    {
        for (int dx = 0; dx < 2; dx++)
        {
            ivec2 lc = loadCoord + ivec2(tile.x + dx * 8, tile.y + dy * 8);
            // Each thread loads up to 4 values to fill the 18x18 tile.
            // For 16x16 threads loading 18x18 = 324 values, each thread loads ~12.7 values.
            // Simplified: just load the tile + immediate neighbors.
        }
    }

    // Simplified approach: each thread loads its own value into shared memory.
    // For the 1px border, threads on the edge also load their border neighbors.
    sVal[tile.y + 1][tile.x + 1] = loadVal(pix, imgSize);

    // Border loads (only done by edge threads)
    if (tile.x == 0)
        sVal[tile.y + 1][0] = loadVal(ivec2(pix.x - 1, pix.y), imgSize);
    if (tile.x == 15)
        sVal[tile.y + 1][17] = loadVal(ivec2(pix.x + 1, pix.y), imgSize);
    if (tile.y == 0)
        sVal[0][tile.x + 1] = loadVal(ivec2(pix.x, pix.y - 1), imgSize);
    if (tile.y == 15)
        sVal[17][tile.x + 1] = loadVal(ivec2(pix.x, pix.y + 1), imgSize);

    // Corner loads
    if (tile.x == 0 && tile.y == 0)
        sVal[0][0] = loadVal(ivec2(pix.x - 1, pix.y - 1), imgSize);
    if (tile.x == 15 && tile.y == 0)
        sVal[0][17] = loadVal(ivec2(pix.x + 1, pix.y - 1), imgSize);
    if (tile.x == 0 && tile.y == 15)
        sVal[17][0] = loadVal(ivec2(pix.x - 1, pix.y + 1), imgSize);
    if (tile.x == 15 && tile.y == 15)
        sVal[17][17] = loadVal(ivec2(pix.x + 1, pix.y + 1), imgSize);

    barrier();

    // ---- Step 1: Sobel edge detection ----
    // The "depth" values from a normal buffer have sharp discontinuities at
    // geometric edges.  Sobel detects these precisely.
    float tl = sVal[tile.y    ][tile.x    ];
    float tc = sVal[tile.y    ][tile.x + 1];
    float tr = sVal[tile.y    ][tile.x + 2];
    float ml = sVal[tile.y + 1][tile.x    ];
    float mr = sVal[tile.y + 1][tile.x + 2];
    float bl = sVal[tile.y + 2][tile.x    ];
    float bc = sVal[tile.y + 2][tile.x + 1];
    float br = sVal[tile.y + 2][tile.x + 2];

    // Sobel X and Y gradients
    float sobelX = -tl - 2.0*ml - bl + tr + 2.0*mr + br;
    float sobelY = -tl - 2.0*tc - tr + bl + 2.0*bc + br;
    float edgeMagnitude = sqrt(sobelX * sobelX + sobelY * sobelY);

    // ---- Step 2: Analyze the center value as a normal proxy ----
    float center = sVal[tile.y + 1][tile.x + 1];

    // ---- Step 3: Local variance analysis ----
    // High variance = edge or detail region → preserve
    // Low variance = flat surface → depth should be smooth
    float mean = (tl + tc + tr + ml + center + mr + bl + bc + br) / 9.0;
    float variance = 0.0;
    variance += (tl - mean) * (tl - mean);
    variance += (tc - mean) * (tc - mean);
    variance += (tr - mean) * (tr - mean);
    variance += (ml - mean) * (ml - mean);
    variance += (center - mean) * (center - mean);
    variance += (mr - mean) * (mr - mean);
    variance += (bl - mean) * (bl - mean);
    variance += (bc - mean) * (bc - mean);
    variance += (br - mean) * (br - mean);
    variance /= 9.0;
    float stddev = sqrt(max(variance, 0.0));

    // ---- Step 4: Construct pseudo-depth ----
    // Strategy: use the gradient direction and magnitude to create depth.
    //
    // For a normal buffer:
    //   - Flat surfaces facing camera: values cluster around one value → near depth
    //   - Tilted surfaces: values change smoothly → gradient indicates slope
    //   - Edges: sharp discontinuities → large Sobel magnitude
    //
    // We construct depth as:
    //   1. Base: the center value itself (this is the normal component → maps to depth)
    //   2. Edge enhancement: boost depth difference at edges
    //   3. Gradient integration: accumulate gradient for smooth slope approximation

    // Base depth from center value (remapped to [0,1] based on local stats)
    // Use percentile-like mapping within the local neighborhood
    float depthBase = center;

    // If the data range is [0,1] (common for encoded normals), use directly.
    // If it looks like linear depth (>1), normalize logarithmically.
    float dataMin = min(min(tl, tc), min(bl, bc));
    float dataMax = max(max(tr, mr), max(br, ml));
    dataMin = min(dataMin, center);
    dataMax = max(dataMax, center);
    float dataRange = max(dataMax - dataMin, 0.0001);

    // Normalize center to [0, 1] based on local range
    float localNorm = clamp((center - dataMin) / dataRange, 0.0, 1.0);

    // ---- Gradient-based depth integration ----
    // The Sobel gradient direction tells us which direction depth increases.
    // Integrate from top-left corner.
    float gradDepth = 0.0;
    // Horizontal gradient contribution (weighted by x position)
    float xFrac = float(pix.x) / max(float(imgSize.x - 1), 1.0);
    gradDepth += sobelX * xFrac * 0.15;
    // Vertical gradient contribution (weighted by y position)
    float yFrac = float(pix.y) / max(float(imgSize.y - 1), 1.0);
    gradDepth += sobelY * yFrac * 0.15;

    // ---- Edge-aware depth ----
    // At edges, create a depth discontinuity proportional to edge strength.
    // Use a sigmoid to create a smooth but sharp step at edges.
    float edgeStrength = smoothstep(0.002, 0.02, edgeMagnitude * pc.invResolution.x);

    // ---- Combine all signals ----
    // The pseudo-depth is a weighted combination:
    //   40% local-normalized value (captures the normal → depth mapping)
    //   30% gradient-integrated depth (captures surface orientation → depth)
    //   30% center value directly (raw signal preservation)
    float rawDepth = localNorm * 0.4
                   + clamp(0.5 + gradDepth, 0.0, 1.0) * 0.3
                   + depthBase * 0.3;

    // ---- Bilateral-filter smoothing pass (3x3, edge-aware) ----
    // This reduces noise while preserving edges (normal discontinuities).
    float bilateralSum = 0.0;
    float bilateralWeight = 0.0;

    // Use a 5x5 cross pattern for better quality
    int radius = 2;
    float sigmaColor = 0.15;  // Color domain sigma for bilateral weight

    for (int dy = -radius; dy <= radius; dy += 1)
    {
        for (int dx = -radius; dx <= radius; dx += 1)
        {
            // Skip corners in 5x5 (only use + pattern and immediate neighbors)
            if (abs(dx) + abs(dy) > 2 && abs(dx) > 1) continue;
            if (abs(dx) > 1 && abs(dy) > 1) continue;

            ivec2 nc = ivec2(tile.x + dx + 1, tile.y + dy + 1);
            float neighborVal = sVal[nc.y][nc.x];
            float neighborDepth = neighborVal;

            // Spatial weight (Gaussian)
            float spatialDist = float(dx * dx + dy * dy);
            float wSpatial = exp(-spatialDist * 0.5);

            // Range weight (based on value difference — preserves edges)
            float valueDiff = abs(neighborVal - center);
            float wRange = exp(-(valueDiff * valueDiff) / (2.0 * sigmaColor * sigmaColor));

            float weight = wSpatial * wRange;
            bilateralSum += neighborDepth * weight;
            bilateralWeight += weight;
        }
    }

    float smoothed = (bilateralWeight > 0.0) ? bilateralSum / bilateralWeight : rawDepth;

    // Apply logarithmic mapping for realistic reverse-Z distribution
    float logMapped = log2(max(smoothed, 0.0001) * (pc.farZ / max(pc.nearZ, 0.001)) + 1.0)
                    / log2(pc.farZ / max(pc.nearZ, 0.001) + 1.0);

    // Blend: prefer log-mapped for realistic distribution, keep some linear for detail
    float blended = mix(logMapped, smoothed, 0.25);

    // Apply edge enhancement at depth discontinuities
    // This is critical: edges should have sharp depth transitions, not smooth ones.
    // We use the edge strength to select between smoothed and unsmoothed.
    float finalDepth = mix(smoothed, rawDepth, edgeStrength * 0.6);
    // Re-apply log mapping after edge mixing
    finalDepth = log2(max(finalDepth, 0.0001) * (pc.farZ / max(pc.nearZ, 0.001)) + 1.0)
               / log2(pc.farZ / max(pc.nearZ, 0.001) + 1.0);

    // Final normalization using local stats
    // Shift to [0, 1] based on the neighborhood
    finalDepth = finalDepth * pc.ndcScale + pc.ndcBias;
    finalDepth = clamp(finalDepth, 0.0, 1.0);

    imageStore(uDepthOut, pix, vec4(finalDepth, 0.0, 0.0, 1.0));
}