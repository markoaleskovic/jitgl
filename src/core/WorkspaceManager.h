//
// Created by malesko on 17. 04. 2026..
//

#ifndef JIT_IDE_WORKSPACEMANAGER_H
#define JIT_IDE_WORKSPACEMANAGER_H

#include <string>
#include <vector>
#include <optional>

struct WorkspaceFile {
    std::string filename;
    std::string filepath;
    std::string content;
};

class WorkspaceManager {
public:
    explicit WorkspaceManager(std::string directory);
    ~WorkspaceManager() = default;

    void Initialize();
    std::vector<WorkspaceFile> LoadAllFiles() const;
    bool SaveFile(const std::string& filepath, const std::string& content) const;
    std::optional<std::string> ReadFile(const std::string& filepath) const;
    bool IsPathInsideWorkspace(const std::string& filepath) const;
    const std::string& Directory() const { return directory_; }

private:
    std::string directory_;
};


#endif //JIT_IDE_WORKSPACEMANAGER_H
