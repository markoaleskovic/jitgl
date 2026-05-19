#pragma once

#include <string>
#include <vector>

// CPU-side mesh payload produced by the OBJ parser. Vertices are interleaved
// in the canonical layout used by AssetRegistry / engine.hpp:
//   vec3 position, vec3 normal, vec2 uv, vec4 tangent.w = bitangent sign.
// That is 12 floats per vertex. Indices are 32-bit unsigned.
struct LoadedMeshData {
    std::vector<float> interleavedVertices;  // 12 * vertex_count floats
    std::vector<unsigned int> indices;       // triangle list
    int vertexCount = 0;
    int indexCount = 0;
    float bboxMin[3] = {0.0f, 0.0f, 0.0f};
    float bboxMax[3] = {0.0f, 0.0f, 0.0f};
};

namespace MeshLoader {
    // Caps tuned to be generous for thesis-scale work but small enough to
    // surface "you imported a 4 GB scan" before it nukes the editor.
    constexpr std::size_t kMaxVertexCount = 10'000'000u;
    constexpr std::size_t kMaxIndexCount  = 30'000'000u;

    // Parse a Wavefront OBJ file from disk. Missing normals are synthesised
    // (smooth-where-shared, flat otherwise). Missing tangents are computed
    // per-triangle from UVs and averaged at shared vertices. UVs missing
    // entirely default to (0,0). Multi-material files are flattened into a
    // single mesh; materials are ignored. Returns false with `*outError`
    // populated on parse failure.
    bool LoadObjFromFile(const std::string& absolutePath,
                         LoadedMeshData* outMesh,
                         std::string* outError);

    // Permanent fallback cube in the same canonical layout. Centered at the
    // origin, edge length 1. Used by AssetRegistry as the "asset is broken"
    // visible-fallback handle.
    void BuildFallbackCube(LoadedMeshData* outMesh);
}  // namespace MeshLoader
