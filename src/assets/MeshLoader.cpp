#include "assets/MeshLoader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
    constexpr int kFloatsPerVertex = 12;  // 3+3+2+4

    struct Vec3 {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };
    struct Vec2 {
        float x = 0.0f, y = 0.0f;
    };
    struct Vec4 {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    };

    // Three-tuple of source-array indices identifying a unique vertex in the
    // .obj input. `-1` means "this attribute was missing on this corner."
    struct VertexKey {
        int posIndex = -1;
        int uvIndex = -1;
        int normalIndex = -1;

        bool operator==(const VertexKey& other) const noexcept {
            return posIndex == other.posIndex &&
                   uvIndex == other.uvIndex &&
                   normalIndex == other.normalIndex;
        }
    };

    struct VertexKeyHash {
        std::size_t operator()(const VertexKey& k) const noexcept {
            // Cheap mix; the indices are dense ints so even a weak hash
            // gives reasonable distribution.
            std::size_t h = 1469598103934665603ull;
            const auto mix = [&h](int v) {
                h ^= static_cast<std::size_t>(static_cast<std::uint32_t>(v));
                h *= 1099511628211ull;
            };
            mix(k.posIndex);
            mix(k.uvIndex);
            mix(k.normalIndex);
            return h;
        }
    };

    int ResolveObjIndex(int rawIndex, std::size_t arraySize) {
        // OBJ indices are 1-based; negative indices count back from the end.
        if (rawIndex == 0) {
            return -1;
        }
        if (rawIndex > 0) {
            const int zeroBased = rawIndex - 1;
            if (zeroBased < 0 || static_cast<std::size_t>(zeroBased) >= arraySize) {
                return -1;
            }
            return zeroBased;
        }
        const int fromEnd = static_cast<int>(arraySize) + rawIndex;
        if (fromEnd < 0 || static_cast<std::size_t>(fromEnd) >= arraySize) {
            return -1;
        }
        return fromEnd;
    }

    bool ParseFaceToken(const std::string& token,
                        std::size_t posCount,
                        std::size_t uvCount,
                        std::size_t normalCount,
                        VertexKey* outKey) {
        if (token.empty()) {
            return false;
        }

        std::array<std::string, 3> parts;
        std::size_t fieldIndex = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= token.size(); ++i) {
            if (i == token.size() || token[i] == '/') {
                if (fieldIndex >= parts.size()) {
                    return false;
                }
                parts[fieldIndex] = token.substr(start, i - start);
                ++fieldIndex;
                start = i + 1;
            }
        }

        auto parseInt = [](const std::string& s, int* out) -> bool {
            if (s.empty()) {
                *out = 0;
                return true;
            }
            try {
                std::size_t consumed = 0;
                const int value = std::stoi(s, &consumed);
                if (consumed != s.size()) {
                    return false;
                }
                *out = value;
                return true;
            } catch (...) {
                return false;
            }
        };

        int rawPos = 0;
        int rawUv = 0;
        int rawNorm = 0;
        if (!parseInt(parts[0], &rawPos)) {
            return false;
        }
        if (!parseInt(parts[1], &rawUv)) {
            return false;
        }
        if (!parseInt(parts[2], &rawNorm)) {
            return false;
        }

        outKey->posIndex = ResolveObjIndex(rawPos, posCount);
        if (outKey->posIndex < 0) {
            return false;
        }
        outKey->uvIndex = ResolveObjIndex(rawUv, uvCount);
        outKey->normalIndex = ResolveObjIndex(rawNorm, normalCount);
        return true;
    }

    Vec3 Sub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3 Add(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 Scale(const Vec3& a, float s)     { return { a.x * s, a.y * s, a.z * s }; }
    Vec3 Cross(const Vec3& a, const Vec3& b) {
        return { a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
    }
    float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    Vec3 Normalize(const Vec3& a) {
        const float len = Length(a);
        if (len <= 1e-12f) {
            return { 0.0f, 0.0f, 0.0f };
        }
        return Scale(a, 1.0f / len);
    }

    void ExpandBbox(LoadedMeshData* mesh, const Vec3& p) {
        if (mesh->vertexCount == 0) {
            mesh->bboxMin[0] = p.x;
            mesh->bboxMin[1] = p.y;
            mesh->bboxMin[2] = p.z;
            mesh->bboxMax[0] = p.x;
            mesh->bboxMax[1] = p.y;
            mesh->bboxMax[2] = p.z;
            return;
        }
        mesh->bboxMin[0] = std::min(mesh->bboxMin[0], p.x);
        mesh->bboxMin[1] = std::min(mesh->bboxMin[1], p.y);
        mesh->bboxMin[2] = std::min(mesh->bboxMin[2], p.z);
        mesh->bboxMax[0] = std::max(mesh->bboxMax[0], p.x);
        mesh->bboxMax[1] = std::max(mesh->bboxMax[1], p.y);
        mesh->bboxMax[2] = std::max(mesh->bboxMax[2], p.z);
    }
}  // namespace

bool MeshLoader::LoadObjFromFile(const std::string& absolutePath,
                                 LoadedMeshData* outMesh,
                                 std::string* outError) {
    if (outMesh == nullptr || outError == nullptr) {
        return false;
    }
    *outMesh = LoadedMeshData{};
    outError->clear();

    std::ifstream file(absolutePath, std::ios::binary);
    if (!file.is_open()) {
        *outError = "cannot open file";
        return false;
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;

    // Each face produces one or more triangle corners; we collect them as
    // (posIdx, uvIdx, normalIdx) triplets and dedup later.
    struct FaceCorner {
        VertexKey key;
    };
    std::vector<FaceCorner> faceCorners;
    std::vector<std::array<int, 3>> triangles;  // indices into faceCorners

    std::string line;
    line.reserve(256);
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Strip leading whitespace.
        std::size_t cursor = 0;
        while (cursor < line.size() &&
               std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        if (cursor >= line.size() || line[cursor] == '#') {
            continue;
        }

        std::istringstream stream(line.substr(cursor));
        std::string tag;
        stream >> tag;
        if (tag == "v") {
            Vec3 p;
            stream >> p.x >> p.y >> p.z;
            if (!stream || stream.fail()) {
                *outError = "malformed 'v' line";
                return false;
            }
            positions.push_back(p);
            if (positions.size() > MeshLoader::kMaxVertexCount) {
                *outError = "vertex count exceeds limit";
                return false;
            }
        } else if (tag == "vn") {
            Vec3 n;
            stream >> n.x >> n.y >> n.z;
            if (!stream || stream.fail()) {
                *outError = "malformed 'vn' line";
                return false;
            }
            normals.push_back(n);
        } else if (tag == "vt") {
            Vec2 t;
            stream >> t.x >> t.y;
            if (!stream || stream.fail()) {
                t.x = 0.0f;
                t.y = 0.0f;
            }
            uvs.push_back(t);
        } else if (tag == "f") {
            std::vector<int> cornerIndicesInFace;
            std::string token;
            while (stream >> token) {
                VertexKey key;
                if (!ParseFaceToken(token, positions.size(), uvs.size(), normals.size(), &key)) {
                    *outError = "malformed face token: " + token;
                    return false;
                }
                FaceCorner corner;
                corner.key = key;
                cornerIndicesInFace.push_back(static_cast<int>(faceCorners.size()));
                faceCorners.push_back(corner);
            }
            if (cornerIndicesInFace.size() < 3) {
                continue;  // skip degenerate
            }
            // Triangulate as a fan.
            for (std::size_t i = 1; i + 1 < cornerIndicesInFace.size(); ++i) {
                triangles.push_back({ cornerIndicesInFace[0],
                                      cornerIndicesInFace[i],
                                      cornerIndicesInFace[i + 1] });
                if (triangles.size() * 3 > MeshLoader::kMaxIndexCount) {
                    *outError = "index count exceeds limit";
                    return false;
                }
            }
        }
        // ignore o, g, s, mtllib, usemtl
    }

    if (positions.empty() || triangles.empty()) {
        *outError = "no geometry in file";
        return false;
    }

    const bool hasInputNormals = !normals.empty();

    // Synthesise per-position normals when the file is missing them.
    // Strategy: accumulate face normals weighted by area at each shared
    // position. This gives smooth shading where adjacent faces share a
    // vertex by position, and falls back to per-face normals when nothing
    // is shared.
    std::vector<Vec3> synthesisedPositionNormals(positions.size(), Vec3{0, 0, 0});
    if (!hasInputNormals) {
        for (const auto& tri : triangles) {
            const Vec3& p0 = positions[faceCorners[tri[0]].key.posIndex];
            const Vec3& p1 = positions[faceCorners[tri[1]].key.posIndex];
            const Vec3& p2 = positions[faceCorners[tri[2]].key.posIndex];
            const Vec3 faceN = Cross(Sub(p1, p0), Sub(p2, p0));  // not normalized -> area weight
            for (int corner = 0; corner < 3; ++corner) {
                const int pIdx = faceCorners[tri[corner]].key.posIndex;
                synthesisedPositionNormals[pIdx] = Add(synthesisedPositionNormals[pIdx], faceN);
            }
        }
        for (auto& n : synthesisedPositionNormals) {
            n = Normalize(n);
        }
    }

    // Build the deduped vertex set indexed by (pos, uv, normal) triplet.
    std::unordered_map<VertexKey, unsigned int, VertexKeyHash> uniqueVertices;
    std::vector<unsigned int> indices;
    indices.reserve(triangles.size() * 3);
    std::vector<Vec3> outPositions;
    std::vector<Vec3> outNormals;
    std::vector<Vec2> outUvs;

    auto resolveVertex = [&](const VertexKey& key) -> unsigned int {
        if (auto it = uniqueVertices.find(key); it != uniqueVertices.end()) {
            return it->second;
        }
        const unsigned int idx = static_cast<unsigned int>(outPositions.size());
        uniqueVertices.emplace(key, idx);
        const Vec3 p = positions[key.posIndex];
        ExpandBbox(outMesh, p);
        outPositions.push_back(p);

        Vec3 n{0, 0, 1};
        if (hasInputNormals && key.normalIndex >= 0 &&
            static_cast<std::size_t>(key.normalIndex) < normals.size()) {
            n = Normalize(normals[key.normalIndex]);
        } else {
            n = synthesisedPositionNormals[key.posIndex];
            if (Length(n) <= 1e-12f) {
                n = Vec3{0, 0, 1};
            }
        }
        outNormals.push_back(n);

        Vec2 t{0, 0};
        if (key.uvIndex >= 0 && static_cast<std::size_t>(key.uvIndex) < uvs.size()) {
            t = uvs[key.uvIndex];
        }
        outUvs.push_back(t);
        return idx;
    };

    for (const auto& tri : triangles) {
        const unsigned int i0 = resolveVertex(faceCorners[tri[0]].key);
        const unsigned int i1 = resolveVertex(faceCorners[tri[1]].key);
        const unsigned int i2 = resolveVertex(faceCorners[tri[2]].key);
        indices.push_back(i0);
        indices.push_back(i1);
        indices.push_back(i2);
    }

    if (outPositions.size() > MeshLoader::kMaxVertexCount ||
        indices.size() > MeshLoader::kMaxIndexCount) {
        *outError = "deduplicated mesh exceeds limits";
        return false;
    }

    // Compute per-triangle tangents from UVs + positions, then accumulate
    // per-vertex and orthonormalise against the final normal. w stores the
    // bitangent handedness sign.
    std::vector<Vec3> tangentSum(outPositions.size(), Vec3{0, 0, 0});
    std::vector<Vec3> bitangentSum(outPositions.size(), Vec3{0, 0, 0});
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned int i0 = indices[i + 0];
        const unsigned int i1 = indices[i + 1];
        const unsigned int i2 = indices[i + 2];
        const Vec3 e1 = Sub(outPositions[i1], outPositions[i0]);
        const Vec3 e2 = Sub(outPositions[i2], outPositions[i0]);
        const Vec2 d1{ outUvs[i1].x - outUvs[i0].x, outUvs[i1].y - outUvs[i0].y };
        const Vec2 d2{ outUvs[i2].x - outUvs[i0].x, outUvs[i2].y - outUvs[i0].y };
        const float denom = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(denom) < 1e-12f) {
            continue;
        }
        const float r = 1.0f / denom;
        const Vec3 tangent{
            (d2.y * e1.x - d1.y * e2.x) * r,
            (d2.y * e1.y - d1.y * e2.y) * r,
            (d2.y * e1.z - d1.y * e2.z) * r,
        };
        const Vec3 bitangent{
            (-d2.x * e1.x + d1.x * e2.x) * r,
            (-d2.x * e1.y + d1.x * e2.y) * r,
            (-d2.x * e1.z + d1.x * e2.z) * r,
        };
        for (unsigned int idx : {i0, i1, i2}) {
            tangentSum[idx] = Add(tangentSum[idx], tangent);
            bitangentSum[idx] = Add(bitangentSum[idx], bitangent);
        }
    }

    outMesh->interleavedVertices.reserve(outPositions.size() * kFloatsPerVertex);
    for (std::size_t i = 0; i < outPositions.size(); ++i) {
        const Vec3 p = outPositions[i];
        const Vec3 n = outNormals[i];
        const Vec2 t = outUvs[i];

        // Gram-Schmidt: tangent = normalize(T - N * dot(N, T))
        Vec3 tang = tangentSum[i];
        const float d = Dot(n, tang);
        tang = Sub(tang, Scale(n, d));
        tang = Normalize(tang);
        if (Length(tang) <= 1e-12f) {
            // Pick an arbitrary tangent perpendicular to n.
            const Vec3 axis = std::abs(n.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            tang = Normalize(Cross(n, axis));
        }
        const float handedness = (Dot(Cross(n, tang), bitangentSum[i]) < 0.0f) ? -1.0f : 1.0f;

        outMesh->interleavedVertices.push_back(p.x);
        outMesh->interleavedVertices.push_back(p.y);
        outMesh->interleavedVertices.push_back(p.z);
        outMesh->interleavedVertices.push_back(n.x);
        outMesh->interleavedVertices.push_back(n.y);
        outMesh->interleavedVertices.push_back(n.z);
        outMesh->interleavedVertices.push_back(t.x);
        outMesh->interleavedVertices.push_back(t.y);
        outMesh->interleavedVertices.push_back(tang.x);
        outMesh->interleavedVertices.push_back(tang.y);
        outMesh->interleavedVertices.push_back(tang.z);
        outMesh->interleavedVertices.push_back(handedness);
    }

    outMesh->indices = std::move(indices);
    outMesh->vertexCount = static_cast<int>(outPositions.size());
    outMesh->indexCount = static_cast<int>(outMesh->indices.size());
    return true;
}

void MeshLoader::BuildFallbackCube(LoadedMeshData* outMesh) {
    if (outMesh == nullptr) {
        return;
    }
    *outMesh = LoadedMeshData{};

    // Unit cube centered at origin, per-face normals, uvs map each face to
    // the full [0,1] square. Constructed by hand so the fallback has the
    // canonical layout end-to-end without going through the OBJ parser.
    struct Face {
        Vec3 normal;
        Vec3 corners[4];
    };

    const std::array<Face, 6> faces = {{
        // +Z
        { {0, 0, 1},  { {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f} } },
        // -Z
        { {0, 0, -1}, { { 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f} } },
        // +X
        { {1, 0, 0},  { { 0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f} } },
        // -X
        { {-1, 0, 0}, { {-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f} } },
        // +Y
        { {0, 1, 0},  { {-0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f} } },
        // -Y
        { {0, -1, 0}, { {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f,  0.5f} } },
    }};
    const std::array<Vec2, 4> faceUv = {{
        {0, 0}, {1, 0}, {1, 1}, {0, 1}
    }};

    outMesh->interleavedVertices.reserve(faces.size() * 4 * kFloatsPerVertex);
    outMesh->indices.reserve(faces.size() * 6);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        const unsigned int base = static_cast<unsigned int>(outMesh->interleavedVertices.size() / kFloatsPerVertex);
        // Pick an arbitrary tangent perpendicular to the normal.
        const Vec3 helper = (std::abs(face.normal.x) < 0.9f) ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 tangent = Normalize(Cross(face.normal, helper));
        for (int corner = 0; corner < 4; ++corner) {
            const Vec3 p = face.corners[corner];
            ExpandBbox(outMesh, p);
            ++outMesh->vertexCount;
            outMesh->interleavedVertices.push_back(p.x);
            outMesh->interleavedVertices.push_back(p.y);
            outMesh->interleavedVertices.push_back(p.z);
            outMesh->interleavedVertices.push_back(face.normal.x);
            outMesh->interleavedVertices.push_back(face.normal.y);
            outMesh->interleavedVertices.push_back(face.normal.z);
            outMesh->interleavedVertices.push_back(faceUv[corner].x);
            outMesh->interleavedVertices.push_back(faceUv[corner].y);
            outMesh->interleavedVertices.push_back(tangent.x);
            outMesh->interleavedVertices.push_back(tangent.y);
            outMesh->interleavedVertices.push_back(tangent.z);
            outMesh->interleavedVertices.push_back(1.0f);  // bitangent sign
        }
        // Two triangles per face: (0,1,2) and (0,2,3).
        outMesh->indices.push_back(base + 0);
        outMesh->indices.push_back(base + 1);
        outMesh->indices.push_back(base + 2);
        outMesh->indices.push_back(base + 0);
        outMesh->indices.push_back(base + 2);
        outMesh->indices.push_back(base + 3);
    }
    outMesh->indexCount = static_cast<int>(outMesh->indices.size());
}
