#pragma once

#include <array>
#include <cstdint>
#include <string>

enum class UniformType : std::uint8_t {
    Float,
    Int,
    Bool,
    Vec2,
    Vec3,
    Vec4,
};

enum class UniformWidgetHint : std::uint8_t {
    Slider,
    Color,
    Toggle,
};

struct UniformDescriptor {
    std::string name;
    UniformType type = UniformType::Float;
    UniformWidgetHint widget = UniformWidgetHint::Slider;

    float rangeMin = 0.0f;
    float rangeMax = 1.0f;
    float step = 0.0f;
    std::string label;
    std::string group;
    bool hidden = false;
    int location = -1;
};

struct UniformValue {
    UniformDescriptor desc;
    float f = 0.0f;
    int i = 0;
    bool b = false;
    std::array<float, 4> v{ 0.0f, 0.0f, 0.0f, 0.0f };
    bool dirty = true;
};

enum class UniformEditAction : std::uint8_t {
    SetFloat,
    SetInt,
    SetBool,
    SetVec,
    ResetOne,
    ResetAll,
};

struct UniformEditCommand {
    UniformEditAction action = UniformEditAction::SetFloat;
    std::string name;
    float floatValue = 0.0f;
    int intValue = 0;
    bool boolValue = false;
    std::array<float, 4> vecValue{ 0.0f, 0.0f, 0.0f, 0.0f };
    int components = 0;
};

inline int UniformComponentCount(const UniformType type) {
    switch (type) {
        case UniformType::Vec2: return 2;
        case UniformType::Vec3: return 3;
        case UniformType::Vec4: return 4;
        default: return 1;
    }
}

inline const char* UniformTypeToString(const UniformType type) {
    switch (type) {
        case UniformType::Float: return "float";
        case UniformType::Int: return "int";
        case UniformType::Bool: return "bool";
        case UniformType::Vec2: return "vec2";
        case UniformType::Vec3: return "vec3";
        case UniformType::Vec4: return "vec4";
    }
    return "unknown";
}

inline bool UniformTypeFromString(const std::string& text, UniformType* outType) {
    if (outType == nullptr) {
        return false;
    }
    if (text == "float") {
        *outType = UniformType::Float;
        return true;
    }
    if (text == "int") {
        *outType = UniformType::Int;
        return true;
    }
    if (text == "bool") {
        *outType = UniformType::Bool;
        return true;
    }
    if (text == "vec2") {
        *outType = UniformType::Vec2;
        return true;
    }
    if (text == "vec3") {
        *outType = UniformType::Vec3;
        return true;
    }
    if (text == "vec4") {
        *outType = UniformType::Vec4;
        return true;
    }
    return false;
}
