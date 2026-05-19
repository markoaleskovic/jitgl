#include "assets/AssetRegistry.h"
#include "assets/MeshLoader.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

namespace fs = std::filesystem;

namespace {
    constexpr std::size_t kMaxTextureBytes = 256 * 1024 * 1024;  // 256 MB decoded cap.

    bool IsKnownImageExtension(std::string_view ext) {
        constexpr std::array<std::string_view, 8> kExts = {
            ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".psd", ".hdr", ".gif"
        };
        std::string lower(ext);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        for (const auto& candidate : kExts) {
            if (lower == candidate) {
                return true;
            }
        }
        return false;
    }

    std::string LowercaseCopy(std::string_view input) {
        std::string out(input);
        std::ranges::transform(out, out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    // Reject `..` segments, drive letters, and embedded NULs so a malicious
    // (or accidental) relative path can't escape the workspace assets/ tree.
    bool ContainsParentRefOrAbsolute(const std::string& p) {
        if (p.empty()) {
            return true;
        }
        if (p.front() == '/' || p.front() == '\\') {
            return true;
        }
        if (p.size() >= 2 && p[1] == ':') {  // Windows drive letter
            return true;
        }
        if (p.find('\0') != std::string::npos) {
            return true;
        }
        std::string canonical = p;
        std::ranges::replace(canonical, '\\', '/');
        std::size_t start = 0;
        while (start < canonical.size()) {
            const std::size_t end = canonical.find('/', start);
            const std::string_view seg(canonical.data() + start,
                                       (end == std::string::npos) ? canonical.size() - start
                                                                  : end - start);
            if (seg == "..") {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    // Verify that `absolute` is inside `root` (after normalization). Both
    // paths must exist for std::filesystem::relative to behave reliably,
    // so we walk parents textually.
    bool IsInsideDirectory(const fs::path& absolute, const fs::path& root) {
        std::error_code ec;
        const fs::path canonAbs = fs::weakly_canonical(absolute, ec);
        if (ec) {
            return false;
        }
        const fs::path canonRoot = fs::weakly_canonical(root, ec);
        if (ec) {
            return false;
        }
        const std::string absStr = canonAbs.lexically_normal().string();
        const std::string rootStr = canonRoot.lexically_normal().string();
        if (absStr.size() < rootStr.size()) {
            return false;
        }
        if (absStr.compare(0, rootStr.size(), rootStr) != 0) {
            return false;
        }
        if (absStr.size() == rootStr.size()) {
            return true;
        }
        const char nextChar = absStr[rootStr.size()];
        return nextChar == '/' || nextChar == '\\';
    }

    // Pull the file stem (filename without extension) and lowercase it for
    // suffix-based sRGB inference.
    std::string LowercaseStem(const std::string& requestedPath) {
        const fs::path p(requestedPath);
        return LowercaseCopy(p.stem().string());
    }
}  // namespace

AssetRegistry::AssetRegistry(std::string workspaceDir, std::string sharedAssetsDir)
    : workspaceAssetsDir_((fs::path(workspaceDir) / "assets").lexically_normal().string()),
      sharedAssetsDir_(std::move(sharedAssetsDir)) {}

AssetRegistry::~AssetRegistry() {
    for (auto& [_, entry] : texturesByKey_) {
        if (entry.handle.id != 0 && entry.handle.id != fallbackTexture_.id) {
            glDeleteTextures(1, &entry.handle.id);
            entry.handle.id = 0;
        }
    }
    texturesByKey_.clear();
    textureAbsoluteToKey_.clear();
    if (fallbackTexture_.id != 0) {
        glDeleteTextures(1, &fallbackTexture_.id);
        fallbackTexture_.id = 0;
    }

    auto deleteMeshObjects = [](JitMesh& m) {
        if (m.vao != 0) {
            glDeleteVertexArrays(1, &m.vao);
            m.vao = 0;
        }
        if (m.vbo != 0) {
            glDeleteBuffers(1, &m.vbo);
            m.vbo = 0;
        }
        if (m.ebo != 0) {
            glDeleteBuffers(1, &m.ebo);
            m.ebo = 0;
        }
    };
    for (auto& [_, entry] : meshesByKey_) {
        if (entry.handle.vao != fallbackMesh_.vao) {
            deleteMeshObjects(entry.handle);
        }
    }
    meshesByKey_.clear();
    meshAbsoluteToKey_.clear();
    deleteMeshObjects(fallbackMesh_);
}

void AssetRegistry::Initialize() {
    if (initialized_) {
        return;
    }
    CreateFallbackTexture();
    CreateFallbackMesh();
    initialized_ = true;
}

void AssetRegistry::CreateFallbackTexture() {
    // 2x2 magenta/black checker: maximally visible "this is broken" cue.
    constexpr std::array<unsigned char, 16> pixels = {
        255, 0, 255, 255,    0,   0,   0, 255,
          0, 0,   0, 255,  255,   0, 255, 255,
    };

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    fallbackTexture_.id = tex;
    fallbackTexture_.width = 2;
    fallbackTexture_.height = 2;
    fallbackTexture_.channels = 4;
    fallbackTexture_.is_srgb = 0;
    fallbackTexture_.is_hdr = 0;
    fallbackTexture_.is_fallback = 1;
}

std::string AssetRegistry::ResolveAssetPath(const std::string& requestedPath) const {
    if (ContainsParentRefOrAbsolute(requestedPath)) {
        return {};
    }

    std::error_code ec;
    auto tryRoot = [&](const std::string& root) -> std::string {
        if (root.empty()) {
            return {};
        }
        const fs::path candidate = fs::path(root) / fs::path(requestedPath);
        if (!fs::exists(candidate, ec) || ec) {
            return {};
        }
        if (!fs::is_regular_file(candidate, ec) || ec) {
            return {};
        }
        if (!IsInsideDirectory(candidate, fs::path(root))) {
            return {};
        }
        const fs::path canonical = fs::weakly_canonical(candidate, ec);
        if (ec) {
            return {};
        }
        return canonical.lexically_normal().string();
    };

    if (auto resolved = tryRoot(workspaceAssetsDir_); !resolved.empty()) {
        return resolved;
    }
    return tryRoot(sharedAssetsDir_);
}

bool AssetRegistry::InferSrgbFromPath(const std::string& requestedPath) {
    const std::string stem = LowercaseStem(requestedPath);
    // Linear-data conventions take priority; anything else is treated as
    // color and gets sRGB.
    constexpr std::array<std::string_view, 12> kLinearHints = {
        "_normal", "_nrm", "_norm", "_data", "_linear",
        "_rough", "_roughness", "_metal", "_metallic",
        "_ao", "_height", "_depth"
    };
    for (const auto& hint : kLinearHints) {
        if (stem.size() >= hint.size() &&
            stem.compare(stem.size() - hint.size(), hint.size(), hint) == 0) {
            return false;
        }
    }
    return true;
}

bool AssetRegistry::DecodeAndUploadInto(const std::string& absolutePath,
                                        bool srgbHint,
                                        JitTexture* out) {
    if (out == nullptr) {
        return false;
    }

    const bool isHdr = (stbi_is_hdr(absolutePath.c_str()) != 0);
    int width = 0;
    int height = 0;
    int channels = 0;

    stbi_set_flip_vertically_on_load(1);

    GLenum internalFormat = GL_RGBA8;
    GLenum dataFormat = GL_RGBA;
    GLenum dataType = GL_UNSIGNED_BYTE;
    void* pixels = nullptr;

    if (isHdr) {
        // HDR is always linear, regardless of name.
        float* hdrPixels = stbi_loadf(absolutePath.c_str(), &width, &height, &channels, 4);
        if (hdrPixels == nullptr) {
            return false;
        }
        pixels = hdrPixels;
        internalFormat = GL_RGBA16F;
        dataFormat = GL_RGBA;
        dataType = GL_FLOAT;
        channels = 4;
    } else {
        unsigned char* ldrPixels = stbi_load(absolutePath.c_str(), &width, &height, &channels, 4);
        if (ldrPixels == nullptr) {
            return false;
        }
        pixels = ldrPixels;
        internalFormat = srgbHint ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        dataFormat = GL_RGBA;
        dataType = GL_UNSIGNED_BYTE;
        channels = 4;
    }

    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        return false;
    }
    const std::size_t bytesPerPixel = isHdr ? sizeof(float) * 4 : 4;
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPerPixel
        > kMaxTextureBytes) {
        stbi_image_free(pixels);
        return false;
    }

    const bool reuseExistingId = (out->id != 0 && out->is_fallback == 0);
    GLuint texId = reuseExistingId ? out->id : 0;
    if (!reuseExistingId) {
        glGenTextures(1, &texId);
    }
    if (texId == 0) {
        stbi_image_free(pixels);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat),
                 width, height, 0, dataFormat, dataType, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Anisotropic filtering, if the driver exposes it.
    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    if (maxAniso > 1.0f) {
        const GLfloat aniso = std::min(16.0f, maxAniso);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    out->id = texId;
    out->width = width;
    out->height = height;
    out->channels = channels;
    out->is_hdr = isHdr ? 1 : 0;
    out->is_srgb = (!isHdr && srgbHint) ? 1 : 0;
    out->is_fallback = 0;
    return true;
}

JitTexture AssetRegistry::LoadTexture(const std::string& requestedPath) {
    if (!initialized_) {
        Initialize();
    }
    if (requestedPath.empty()) {
        return fallbackTexture_;
    }

    const std::string key = LowercaseCopy(requestedPath);
    if (auto it = texturesByKey_.find(key); it != texturesByKey_.end()) {
        return it->second.handle;
    }

    const std::string absolute = ResolveAssetPath(requestedPath);
    if (absolute.empty()) {
        std::fprintf(stderr,
                     "[asset] FAILED texture \"%s\": path not found in workspace or shared assets\n",
                     requestedPath.c_str());
        // Cache the failure under the user-supplied key so we don't keep
        // hitting the disk every frame. The cache entry points at the
        // fallback; if the file later appears, the watcher will swap it in.
        CachedTexture entry;
        entry.handle = fallbackTexture_;
        entry.requestedKey = key;
        entry.absolutePath.clear();
        entry.srgbHint = InferSrgbFromPath(requestedPath);
        texturesByKey_[key] = entry;
        return fallbackTexture_;
    }

    JitTexture handle{};
    const bool srgbHint = InferSrgbFromPath(requestedPath);
    if (!DecodeAndUploadInto(absolute, srgbHint, &handle)) {
        std::fprintf(stderr, "[asset] FAILED texture \"%s\": %s\n",
                     requestedPath.c_str(), stbi_failure_reason());
        CachedTexture entry;
        entry.handle = fallbackTexture_;
        entry.requestedKey = key;
        entry.absolutePath = absolute;
        entry.srgbHint = srgbHint;
        texturesByKey_[key] = entry;
        textureAbsoluteToKey_[absolute] = key;
        return fallbackTexture_;
    }

    CachedTexture entry;
    entry.handle = handle;
    entry.requestedKey = key;
    entry.absolutePath = absolute;
    entry.srgbHint = srgbHint;
    texturesByKey_[key] = entry;
    textureAbsoluteToKey_[absolute] = key;

    std::fprintf(stderr, "[asset] loaded texture \"%s\" %dx%d %s\n",
                 requestedPath.c_str(), handle.width, handle.height,
                 handle.is_hdr ? "HDR" : (handle.is_srgb ? "sRGB" : "linear"));
    return handle;
}

bool AssetRegistry::ReloadTextureFromAbsolutePath(const std::string& absolutePath) {
    auto it = textureAbsoluteToKey_.find(absolutePath);
    if (it == textureAbsoluteToKey_.end()) {
        return false;
    }
    auto cacheIt = texturesByKey_.find(it->second);
    if (cacheIt == texturesByKey_.end()) {
        return false;
    }
    auto& entry = cacheIt->second;

    JitTexture newHandle = entry.handle;
    if (!DecodeAndUploadInto(absolutePath, entry.srgbHint, &newHandle)) {
        std::fprintf(stderr, "[asset] FAILED reload \"%s\": %s\n",
                     absolutePath.c_str(), stbi_failure_reason());
        return false;
    }
    entry.handle = newHandle;
    std::fprintf(stderr, "[asset] reloaded texture \"%s\" %dx%d\n",
                 absolutePath.c_str(), newHandle.width, newHandle.height);
    return true;
}

bool AssetRegistry::IsTrackedAbsolutePath(const std::string& absolutePath) const {
    return textureAbsoluteToKey_.contains(absolutePath) ||
           meshAbsoluteToKey_.contains(absolutePath);
}

void AssetRegistry::CreateFallbackMesh() {
    LoadedMeshData cube;
    MeshLoader::BuildFallbackCube(&cube);
    fallbackMesh_.is_fallback = 1;
    if (!UploadMeshInto(cube, &fallbackMesh_)) {
        // Even GPU allocation for the fallback failed -- leave it zeroed.
        fallbackMesh_ = JitMesh{};
        fallbackMesh_.is_fallback = 1;
    }
}

bool AssetRegistry::UploadMeshInto(const LoadedMeshData& cpuMesh, JitMesh* inOutMesh) {
    if (inOutMesh == nullptr || cpuMesh.indices.empty()) {
        return false;
    }

    const bool reuseExisting = (inOutMesh->vao != 0 &&
                                inOutMesh->vbo != 0 &&
                                inOutMesh->ebo != 0 &&
                                inOutMesh->is_fallback == 0);

    GLuint vao = inOutMesh->vao;
    GLuint vbo = inOutMesh->vbo;
    GLuint ebo = inOutMesh->ebo;
    if (!reuseExisting) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
    }
    if (vao == 0 || vbo == 0 || ebo == 0) {
        return false;
    }

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(cpuMesh.interleavedVertices.size() * sizeof(float)),
                 cpuMesh.interleavedVertices.data(),
                 GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(cpuMesh.indices.size() * sizeof(unsigned int)),
                 cpuMesh.indices.data(),
                 GL_STATIC_DRAW);

    // Canonical layout: vec3 position, vec3 normal, vec2 uv, vec4 tangent.
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(float) * 12);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(sizeof(float) * 3));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(sizeof(float) * 6));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(sizeof(float) * 8));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    inOutMesh->vao = vao;
    inOutMesh->vbo = vbo;
    inOutMesh->ebo = ebo;
    inOutMesh->vertex_count = cpuMesh.vertexCount;
    inOutMesh->index_count = cpuMesh.indexCount;
    inOutMesh->bbox_min[0] = cpuMesh.bboxMin[0];
    inOutMesh->bbox_min[1] = cpuMesh.bboxMin[1];
    inOutMesh->bbox_min[2] = cpuMesh.bboxMin[2];
    inOutMesh->bbox_max[0] = cpuMesh.bboxMax[0];
    inOutMesh->bbox_max[1] = cpuMesh.bboxMax[1];
    inOutMesh->bbox_max[2] = cpuMesh.bboxMax[2];
    return true;
}

JitMesh AssetRegistry::LoadMesh(const std::string& requestedPath) {
    if (!initialized_) {
        Initialize();
    }
    if (requestedPath.empty()) {
        return fallbackMesh_;
    }

    const std::string key = LowercaseCopy(requestedPath);
    if (auto it = meshesByKey_.find(key); it != meshesByKey_.end()) {
        return it->second.handle;
    }

    const std::string absolute = ResolveAssetPath(requestedPath);
    auto cacheFallback = [&](const std::string& reason, const std::string& abs) {
        std::fprintf(stderr, "[asset] FAILED mesh \"%s\": %s\n",
                     requestedPath.c_str(), reason.c_str());
        CachedMesh entry;
        entry.handle = fallbackMesh_;
        entry.requestedKey = key;
        entry.absolutePath = abs;
        meshesByKey_[key] = entry;
        if (!abs.empty()) {
            meshAbsoluteToKey_[abs] = key;
        }
    };

    if (absolute.empty()) {
        cacheFallback("path not found in workspace or shared assets", {});
        return fallbackMesh_;
    }

    LoadedMeshData cpuMesh;
    std::string parseError;
    if (!MeshLoader::LoadObjFromFile(absolute, &cpuMesh, &parseError)) {
        cacheFallback(parseError.empty() ? std::string("parse failed") : parseError, absolute);
        return fallbackMesh_;
    }

    JitMesh handle{};
    if (!UploadMeshInto(cpuMesh, &handle)) {
        cacheFallback("GPU upload failed", absolute);
        return fallbackMesh_;
    }

    CachedMesh entry;
    entry.handle = handle;
    entry.requestedKey = key;
    entry.absolutePath = absolute;
    meshesByKey_[key] = entry;
    meshAbsoluteToKey_[absolute] = key;

    std::fprintf(stderr,
                 "[asset] loaded mesh \"%s\" %d verts %d tris\n",
                 requestedPath.c_str(),
                 handle.vertex_count,
                 handle.index_count / 3);
    return handle;
}

bool AssetRegistry::ReloadMeshFromAbsolutePath(const std::string& absolutePath) {
    auto it = meshAbsoluteToKey_.find(absolutePath);
    if (it == meshAbsoluteToKey_.end()) {
        return false;
    }
    auto cacheIt = meshesByKey_.find(it->second);
    if (cacheIt == meshesByKey_.end()) {
        return false;
    }
    auto& entry = cacheIt->second;

    LoadedMeshData cpuMesh;
    std::string parseError;
    if (!MeshLoader::LoadObjFromFile(absolutePath, &cpuMesh, &parseError)) {
        std::fprintf(stderr, "[asset] FAILED reload mesh \"%s\": %s\n",
                     absolutePath.c_str(), parseError.c_str());
        return false;
    }

    JitMesh newHandle = entry.handle;
    if (!UploadMeshInto(cpuMesh, &newHandle)) {
        std::fprintf(stderr, "[asset] FAILED reload mesh upload \"%s\"\n",
                     absolutePath.c_str());
        return false;
    }
    entry.handle = newHandle;
    std::fprintf(stderr,
                 "[asset] reloaded mesh \"%s\" %d verts %d tris\n",
                 absolutePath.c_str(),
                 newHandle.vertex_count,
                 newHandle.index_count / 3);
    return true;
}

bool AssetRegistry::IsUnderAssetRoots(const std::string& absolutePath) const {
    if (absolutePath.empty()) {
        return false;
    }
    const fs::path p(absolutePath);
    if (!workspaceAssetsDir_.empty() && IsInsideDirectory(p, fs::path(workspaceAssetsDir_))) {
        return true;
    }
    if (!sharedAssetsDir_.empty() && IsInsideDirectory(p, fs::path(sharedAssetsDir_))) {
        return true;
    }
    return false;
}
