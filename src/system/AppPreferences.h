#pragma once

#include <string>
#include <unordered_map>

class AppPreferences {
public:
    explicit AppPreferences(std::string filePath = ".jitgl_prefs");

    bool Reload();
    bool Save() const;

    bool GetBool(const std::string& key, bool fallback) const;
    int GetInt(const std::string& key, int fallback) const;
    std::string GetString(const std::string& key, std::string fallback = {}) const;

    void SetBool(const std::string& key, bool value);
    void SetInt(const std::string& key, int value);
    void SetString(const std::string& key, const std::string& value);

private:
    static std::string Trim(std::string value);
    static std::string ToLower(std::string value);

    std::string filePath_;
    std::unordered_map<std::string, std::string> values_;
};
