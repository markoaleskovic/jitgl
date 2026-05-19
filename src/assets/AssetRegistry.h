#pragma once

#include "runtime/EngineContext.h"

#include <string>
#include <unordered_map>

// Per-workspace cache of GPU asset handles.
//
// Responsibilities:
//   * Resolve user-supplied paths against the workspace's own `assets/` dir
//     first, then a shared/root `assets/` dir.
//   * Reject paths that escape those roots (no `..`, no absolute paths).
//   * Decode + upload textures via stb_image with sane defaults (sRGB on
//     for color, linear for `_normal` / `_data` / `_linear` / etc.,
//     mipmaps + max anisotropy).
//   * Hand back a permanent fallback texture (2x2 magenta/black checker)
//     for any path that does not resolve / decode -- valid GL state so the
//     user's draw call still binds something and the bug is visible.
//   * Hot-reload: re-upload into the same GL id when the underlying file
//     changes, so user code never has to re-fetch a stale handle.
//
// Lifetime: one instance per workspace. GL resources are freed in the
// destructor, which must run while the GL context is still current
// (i.e. before window destruction).
class AssetRegistry {
public:
    AssetRegistry(std::string workspaceDir, std::string sharedAssetsDir);
    ~AssetRegistry();

    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // Creates the permanent fallback texture. Requires a current GL context.
    // Idempotent.
    void Initialize();

    // Always returns a valid handle. Falls back to the permanent checker on
    // resolution / decode failure.
    JitTexture LoadTexture(const std::string& requestedPath);

    // Always returns a valid handle. Falls back to a permanent unit cube on
    // resolution / decode failure.
    JitMesh LoadMesh(const std::string& requestedPath);

    // If `absolutePath` matches a cached texture, re-decode it into the same
    // GL id (preserving any handles already held by user code). Returns true
    // iff a cached entry was found.
    bool ReloadTextureFromAbsolutePath(const std::string& absolutePath);

    // Mesh equivalent: re-parses the OBJ and re-uploads into the cached
    // VAO/VBO/EBO so user code holding the JitMesh keeps drawing the same
    // GL objects.
    bool ReloadMeshFromAbsolutePath(const std::string& absolutePath);

    // Test if the given absolute path corresponds to a known asset under
    // either of our roots. Useful for the file watcher to filter changes.
    bool IsTrackedAbsolutePath(const std::string& absolutePath) const;

    // Convenience: returns true if the path lives under one of the two
    // asset roots (workspace or shared), regardless of whether the file
    // has been loaded yet. Cheap path containment check, not a stat.
    bool IsUnderAssetRoots(const std::string& absolutePath) const;

private:
    struct CachedTexture {
        JitTexture handle{};
        std::string requestedKey;
        std::string absolutePath;
        bool srgbHint = false;
    };

    struct CachedMesh {
        JitMesh handle{};
        std::string requestedKey;
        std::string absolutePath;
    };

    std::string workspaceAssetsDir_;
    std::string sharedAssetsDir_;
    std::unordered_map<std::string, CachedTexture> texturesByKey_;
    std::unordered_map<std::string, CachedMesh> meshesByKey_;
    // Reverse maps: absolute filesystem path -> requested key. Maintained per
    // asset kind so a watcher event can find the right cache slot quickly.
    std::unordered_map<std::string, std::string> textureAbsoluteToKey_;
    std::unordered_map<std::string, std::string> meshAbsoluteToKey_;
    JitTexture fallbackTexture_{};
    JitMesh fallbackMesh_{};
    bool initialized_ = false;

    // Returns canonical absolute path on success, empty string on rejection.
    std::string ResolveAssetPath(const std::string& requestedPath) const;

    bool DecodeAndUploadInto(const std::string& absolutePath,
                             bool srgbHint,
                             JitTexture* out);

    // Re-uploads `cpuMesh` into the existing VAO/VBO/EBO referenced by
    // `*inOutMesh`. Allocates fresh GL objects if the handle is currently
    // pointing at the fallback (or unset).
    bool UploadMeshInto(const struct LoadedMeshData& cpuMesh, JitMesh* inOutMesh);

    void CreateFallbackTexture();
    void CreateFallbackMesh();
    static bool InferSrgbFromPath(const std::string& requestedPath);
};
