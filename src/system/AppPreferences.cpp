#include "system/AppPreferences.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <ranges>
#include <utility>
#include <vector>

AppPreferences::AppPreferences(std::string filePath)
    : filePath_(std::move(filePath)) {}

bool AppPreferences::Reload() {
    values_.clear();

    std::ifstream inFile(filePath_, std::ios::binary);
    if (!inFile.is_open()) {
        return true;
    }

    std::string line;
    while (std::getline(inFile, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const std::size_t equalsPos = trimmed.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        std::string key = Trim(trimmed.substr(0, equalsPos));
        std::string value = Trim(trimmed.substr(equalsPos + 1));
        if (key.empty()) {
            continue;
        }
        values_[std::move(key)] = std::move(value);
    }

    return true;
}

bool AppPreferences::Save() const {
    std::ofstream outFile(filePath_, std::ios::trunc | std::ios::binary);
    if (!outFile.is_open()) {
        return false;
    }

    std::vector<std::string> keys;
    keys.reserve(values_.size());
    for (const auto& [key, value] : values_) {
        (void)value;
        keys.push_back(key);
    }
    std::ranges::sort(keys);

    outFile << "# JITGL preferences v1\n";
    for (const auto& key : keys) {
        if (const auto valueIt = values_.find(key); valueIt != values_.end()) {
            outFile << key << "=" << valueIt->second << '\n';
        }
    }

    return outFile.good();
}

bool AppPreferences::GetBool(const std::string& key, const bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }

    const std::string valueLower = ToLower(Trim(it->second));
    if (valueLower == "1" || valueLower == "true" || valueLower == "yes" || valueLower == "on") {
        return true;
    }
    if (valueLower == "0" || valueLower == "false" || valueLower == "no" || valueLower == "off") {
        return false;
    }
    return fallback;
}

int AppPreferences::GetInt(const std::string& key, const int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }

    const std::string value = Trim(it->second);
    int parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return fallback;
    }
    return parsed;
}

std::string AppPreferences::GetString(const std::string& key, std::string fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) {
        return fallback;
    }
    return it->second;
}

void AppPreferences::SetBool(const std::string& key, const bool value) {
    values_[key] = value ? "1" : "0";
}

void AppPreferences::SetInt(const std::string& key, const int value) {
    values_[key] = std::to_string(value);
}

void AppPreferences::SetString(const std::string& key, const std::string& value) {
    values_[key] = value;
}

std::string AppPreferences::Trim(std::string value) {
    auto isSpace = [](const unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string AppPreferences::ToLower(std::string value) {
    std::ranges::transform(value,
                           value.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
