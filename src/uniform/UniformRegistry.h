#pragma once

#include <glad/gl.h>

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "uniform/UniformControls.h"

class UniformRegistry {
public:
    void Rebuild(const std::vector<UniformDescriptor>& descriptors);

    void BindProgram(GLuint programHandle);
    void UploadDirty();

    bool ApplyEdit(const UniformEditCommand& command);

    std::string SerializeToJson() const;
    bool LoadFromJson(const std::string& json);

    const std::vector<UniformValue>& Values() const { return values_; }

private:
    struct PersistedValue {
        UniformType type = UniformType::Float;
        float scalarFloat = 0.0f;
        int scalarInt = 0;
        bool scalarBool = false;
        std::array<float, 4> vec{ 0.0f, 0.0f, 0.0f, 0.0f };
        int components = 0;
    };

    void RebuildIndex();
    void ResetUniformToDefault(UniformValue* value) const;
    bool ApplyPersistedValue(UniformValue* target, const PersistedValue& persisted) const;
    static std::string EscapeJsonString(const std::string& value);

    std::vector<UniformValue> values_;
    std::unordered_map<std::string, std::size_t> indexByName_;
    std::unordered_map<std::string, PersistedValue> persistedValues_;
    GLuint currentProgram_ = 0;
};
