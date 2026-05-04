//
// Created by malesko on 17. 04. 2026..
//

#ifndef JIT_IDE_WORKSPACEMANAGER_H
#define JIT_IDE_WORKSPACEMANAGER_H

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

struct WorkspaceFile {
    std::string filename;
    std::string filepath;
    std::string content;
};

struct WorkspaceDescriptor {
    std::string name;
    std::string directory;
    std::string cppPath;
    std::string shaderPath;
    std::string consoleLogPath;
    std::string engineLogPath;
};

class WorkspaceManager {
public:
    explicit WorkspaceManager(std::string directory);
    ~WorkspaceManager() = default;

    void Initialize() const;
    std::vector<WorkspaceDescriptor> ListWorkspaces() const;
    std::optional<WorkspaceDescriptor> GetWorkspace(const std::string& workspaceName) const;
    std::optional<WorkspaceDescriptor> CreateWorkspace(const std::string& workspaceName) const;
    bool DeleteWorkspace(const std::string& workspaceName) const;

    std::vector<std::string> LoadWorkspaceConsoleLog(const std::string& workspaceName) const;
    std::vector<std::string> LoadWorkspaceEngineLog(const std::string& workspaceName) const;
    bool AppendWorkspaceConsoleLog(const std::string& workspaceName, const std::string& line) const;
    bool AppendWorkspaceEngineLog(const std::string& workspaceName, const std::string& line) const;

    std::vector<WorkspaceFile> LoadAllFiles() const;
    // Save/Read reject paths outside workspace root to avoid arbitrary file access.
    bool SaveFile(const std::string& filepath, const std::string& content) const;
    std::optional<std::string> ReadFile(const std::string& filepath) const;
    bool IsPathInsideWorkspace(const std::filesystem::path& filepath) const;
    const std::string& Directory() const { return directory_; }

private:
    std::optional<WorkspaceDescriptor> BuildDescriptor(const std::string& workspaceName) const;
    bool EnsureWorkspaceScaffold(const WorkspaceDescriptor& descriptor) const;
    static bool IsValidWorkspaceName(const std::string& workspaceName);
    static std::vector<std::string> LoadLogFileLines(const std::string& logPath);
    static bool AppendLogFileLine(const std::string& logPath, const std::string& line);

    std::string directory_;
};


#endif //JIT_IDE_WORKSPACEMANAGER_H
