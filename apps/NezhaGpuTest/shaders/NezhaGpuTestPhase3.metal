/* SPDX-License-Identifier: MIT */
/* Metal 3.0 source; Apple SDK compilation and arm64e linking await Mac CI. */
#include <metal_stdlib>
using namespace metal;

struct Phase3Vertex { float x, y, z, padding; uint r, g, b, a; };
struct Phase3Out {
    float4 position [[position]];
    float4 rgba [[flat]];
};
vertex Phase3Out p3_scene_vertex(uint id [[vertex_id]],
                               const device Phase3Vertex *vertices [[buffer(0)]],
                               constant float2 &extent [[buffer(1)]]) {
    Phase3Vertex v = vertices[id];
    Phase3Out out;
    out.position = float4(2.0f * v.x / extent.x - 1.0f, 1.0f - 2.0f * v.y / extent.y, v.z, 1.0f);
    out.rgba = float4(v.r, v.g, v.b, v.a) / 255.0f;
    return out;
}
vertex Phase3Out p3_fullscreen_vertex(uint id [[vertex_id]]) {
    const float2 points[] = {float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    Phase3Out out; out.position = float4(points[id], 0.0f, 1.0f); out.rgba = float4(1.0f);
    return out;
}
fragment float4 p3_flat_fragment(Phase3Out in [[stage_in]]) { return in.rgba; }

uint4 p3_boundary_rgba(uint2 p, uint2 extent) {
    uint4 color(p.x * 255u / max(1u, extent.x - 1u), p.y * 255u / max(1u, extent.y - 1u),
                (p.x + p.y) * 255u / max(1u, extent.x + extent.y - 2u), 255u);
    if ((p.x + 2u * p.y) % 193u < 4u) color = uint4(240u, 240u, 240u, 255u);
    if (p.x % 96u == 95u) color = uint4(255u, 0u, 255u, 255u);
    if (p.x % 96u == 0u) color = uint4(0u, 255u, 0u, 255u);
    if (p.y % 32u == 31u) color = uint4(255u, 96u, 0u, 255u);
    if (p.y % 32u == 0u) color = uint4(0u, 224u, 255u, 255u);
    if ((p.x % 96u == 0u || p.x % 96u == 95u) && (p.y % 32u == 0u || p.y % 32u == 31u))
        color = uint4(p.y % 32u == 0u ? 255u : 0u, p.x % 96u == 0u ? 255u : 0u,
                      (p.x / 96u * 29u + p.y / 32u * 17u) % 256u, 255u);
    return color;
}
fragment float4 p3_boundary_fragment(Phase3Out in [[stage_in]], constant uint2 &extent [[buffer(0)]]) {
    return float4(p3_boundary_rgba(uint2(in.position.xy), extent)) / 255.0f;
}
uint p3_tap(uint x, uint y, uint factor) { return (x * factor + y * (factor + 2u)) % 256u; }
uint p3_gaussian(uint x, uint y, uint factor) {
    return (p3_tap(x, y, factor) + 2u * p3_tap(x + 1u, y, factor) + p3_tap(x + 2u, y, factor) + 2u) / 4u;
}
/* Four draws remain in ONE render encoder. color(0) fetches the preceding
 * draw's current-pixel attachment; no texture snapshot or CPU upload occurs.
 * The Gaussian approximation samples an analytic color field, since ordinary
 * framebuffer fetch provides the current pixel, not arbitrary neighbor reads.
 */
struct Phase3FetchOut { float4 rgba [[color(0), raster_order_group(0)]]; };
fragment Phase3FetchOut p3_fetch_fragment(Phase3Out in [[stage_in]],
                                        float4 previous [[color(0), raster_order_group(0)]],
                                        constant uint &pass [[buffer(0)]]) {
    uint2 p = uint2(in.position.xy) + uint2(pass * 13u, pass * 7u);
    uint4 source(p3_gaussian(p.x, p.y, 7u), p3_gaussian(p.x, p.y, 3u), p3_gaussian(p.x, p.y, 5u), 255u);
    uint4 old = uint4(round(clamp(previous, 0.0f, 1.0f) * 255.0f));
    uint weight = 1u + pass * 2u;
    Phase3FetchOut out;
    out.rgba = float4((old * (8u - weight) + source * weight + 4u) / 8u) / 255.0f;
    return out;
}
uint4 p3_ubwc_rgba(uint2 p) {
    uint2 block = p / uint2(16u, 4u);
    if ((block.x + block.y) % 2u == 0u) return uint4(192u, 48u, (block.x * 29u + block.y * 17u) % 256u, 255u);
    uint seed = p.x * 747796405u + p.y * 2891336453u + 277803737u;
    seed = (seed ^ (seed >> 16u)) * 2246822519u; seed ^= seed >> 13u;
    return uint4((seed >> 16u) & 255u, (seed >> 8u) & 255u, seed & 255u, 255u);
}
kernel void p3_ubwc_write(texture2d<float, access::write> target [[texture(0)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x < target.get_width() && p.y < target.get_height()) target.write(float4(p3_ubwc_rgba(p)) / 255.0f, p);
}
fragment float4 p3_ubwc_sample_fragment(Phase3Out in [[stage_in]], texture2d<float> source [[texture(0)]],
                                       sampler nearest [[sampler(0)]]) {
    float2 uv = in.position.xy / float2(source.get_width(), source.get_height());
    return source.sample(nearest, uv);
}
