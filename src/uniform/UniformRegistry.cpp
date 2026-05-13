#include "uniform/UniformRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    float DefaultScalarInRange(float minValue, float maxValue) {
        if (maxValue < minValue) {
            std::swap(minValue, maxValue);
        }
        if (0.0f < minValue) {
            return minValue;
        }
        if (0.0f > maxValue) {
            return maxValue;
        }
        return 0.0f;
    }

    class JsonCursor {
    public:
        explicit JsonCursor(std::string_view text) : text_(text) {}

        void SkipWhitespace() {
            while (pos_ < text_.size() &&
                   (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
                ++pos_;
            }
        }

        bool ConsumeChar(const char expected) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != expected) {
                return false;
            }
            ++pos_;
            return true;
        }

        bool PeekChar(const char expected) {
            SkipWhitespace();
            return pos_ < text_.size() && text_[pos_] == expected;
        }

        bool ParseString(std::string* out) {
            if (out == nullptr) {
                return false;
            }
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return false;
            }
            ++pos_;

            out->clear();
            bool escape = false;
            while (pos_ < text_.size()) {
                const char c = text_[pos_++];
                if (escape) {
                    switch (c) {
                        case '"': out->push_back('"'); break;
                        case '\\': out->push_back('\\'); break;
                        case '/': out->push_back('/'); break;
                        case 'b': out->push_back('\b'); break;
                        case 'f': out->push_back('\f'); break;
                        case 'n': out->push_back('\n'); break;
                        case 'r': out->push_back('\r'); break;
                        case 't': out->push_back('\t'); break;
                        default: out->push_back(c); break;
                    }
                    escape = false;
                    continue;
                }
                if (c == '\\') {
                    escape = true;
                    continue;
                }
                if (c == '"') {
                    return true;
                }
                out->push_back(c);
            }
            return false;
        }

        bool ParseNumber(double* out) {
            if (out == nullptr) {
                return false;
            }
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return false;
            }

            const std::size_t start = pos_;
            if (text_[pos_] == '+' || text_[pos_] == '-') {
                ++pos_;
            }
            bool sawDigit = false;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                sawDigit = true;
            }
            if (pos_ < text_.size() && text_[pos_] == '.') {
                ++pos_;
                while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                    ++pos_;
                    sawDigit = true;
                }
            }
            if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
                ++pos_;
                if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                    ++pos_;
                }
                bool expDigit = false;
                while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                    ++pos_;
                    expDigit = true;
                }
                if (!expDigit) {
                    return false;
                }
            }

            if (!sawDigit) {
                pos_ = start;
                return false;
            }

            const std::string token(text_.substr(start, pos_ - start));
            char* end = nullptr;
            const double parsed = std::strtod(token.c_str(), &end);
            if (end == token.c_str() || *end != '\0') {
                return false;
            }
            *out = parsed;
            return true;
        }

        bool ParseBool(bool* out) {
            if (out == nullptr) {
                return false;
            }
            SkipWhitespace();
            if (text_.substr(pos_, 4) == "true") {
                pos_ += 4;
                *out = true;
                return true;
            }
            if (text_.substr(pos_, 5) == "false") {
                pos_ += 5;
                *out = false;
                return true;
            }
            return false;
        }

        bool ParseNull() {
            SkipWhitespace();
            if (text_.substr(pos_, 4) == "null") {
                pos_ += 4;
                return true;
            }
            return false;
        }

        bool SkipValue() {
            SkipWhitespace();
            if (pos_ >= text_.size()) {
                return false;
            }

            const char c = text_[pos_];
            if (c == '{') {
                ++pos_;
                SkipWhitespace();
                if (ConsumeChar('}')) {
                    return true;
                }
                while (true) {
                    std::string key;
                    if (!ParseString(&key)) {
                        return false;
                    }
                    if (!ConsumeChar(':') || !SkipValue()) {
                        return false;
                    }
                    if (ConsumeChar('}')) {
                        return true;
                    }
                    if (!ConsumeChar(',')) {
                        return false;
                    }
                }
            }

            if (c == '[') {
                ++pos_;
                SkipWhitespace();
                if (ConsumeChar(']')) {
                    return true;
                }
                while (true) {
                    if (!SkipValue()) {
                        return false;
                    }
                    if (ConsumeChar(']')) {
                        return true;
                    }
                    if (!ConsumeChar(',')) {
                        return false;
                    }
                }
            }

            if (c == '"') {
                std::string value;
                return ParseString(&value);
            }

            bool boolValue = false;
            if (ParseBool(&boolValue)) {
                return true;
            }
            if (ParseNull()) {
                return true;
            }
            double number = 0.0;
            return ParseNumber(&number);
        }

        bool ParseNumberArray(std::vector<double>* out) {
            if (out == nullptr) {
                return false;
            }
            out->clear();
            if (!ConsumeChar('[')) {
                return false;
            }
            if (ConsumeChar(']')) {
                return true;
            }
            while (true) {
                double value = 0.0;
                if (!ParseNumber(&value)) {
                    return false;
                }
                out->push_back(value);
                if (ConsumeChar(']')) {
                    return true;
                }
                if (!ConsumeChar(',')) {
                    return false;
                }
            }
        }

        bool AtEnd() {
            SkipWhitespace();
            return pos_ >= text_.size();
        }

    private:
        std::string_view text_;
        std::size_t pos_ = 0;
    };

    struct ParsedEntryValue {
        enum class Kind {
            None,
            Number,
            Bool,
            Array,
        };

        Kind kind = Kind::None;
        double number = 0.0;
        bool boolValue = false;
        std::array<float, 4> vec{ 0.0f, 0.0f, 0.0f, 0.0f };
        int components = 0;
    };

    bool ParseEntryValue(JsonCursor* cursor, ParsedEntryValue* outValue) {
        if (cursor == nullptr || outValue == nullptr) {
            return false;
        }
        cursor->SkipWhitespace();
        if (cursor->PeekChar('[')) {
            std::vector<double> numbers;
            if (!cursor->ParseNumberArray(&numbers) || numbers.empty() || numbers.size() > 4) {
                return false;
            }
            outValue->kind = ParsedEntryValue::Kind::Array;
            outValue->components = static_cast<int>(numbers.size());
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                outValue->vec[i] = static_cast<float>(numbers[i]);
            }
            return true;
        }

        bool boolValue = false;
        if (cursor->ParseBool(&boolValue)) {
            outValue->kind = ParsedEntryValue::Kind::Bool;
            outValue->boolValue = boolValue;
            return true;
        }

        double number = 0.0;
        if (cursor->ParseNumber(&number)) {
            outValue->kind = ParsedEntryValue::Kind::Number;
            outValue->number = number;
            return true;
        }

        return false;
    }
}

void UniformRegistry::RebuildIndex() {
    indexByName_.clear();
    indexByName_.reserve(values_.size());
    for (std::size_t i = 0; i < values_.size(); ++i) {
        indexByName_[values_[i].desc.name] = i;
    }
}

void UniformRegistry::ResetUniformToDefault(UniformValue* value) const {
    if (value == nullptr) {
        return;
    }

    value->f = DefaultScalarInRange(value->desc.rangeMin, value->desc.rangeMax);
    value->i = static_cast<int>(std::lround(DefaultScalarInRange(value->desc.rangeMin, value->desc.rangeMax)));
    value->b = false;
    value->v.fill(value->f);
    value->dirty = true;
}

bool UniformRegistry::ApplyPersistedValue(UniformValue* target, const PersistedValue& persisted) const {
    if (target == nullptr || target->desc.type != persisted.type) {
        return false;
    }

    switch (target->desc.type) {
        case UniformType::Float:
            target->f = persisted.scalarFloat;
            break;
        case UniformType::Int:
            target->i = persisted.scalarInt;
            break;
        case UniformType::Bool:
            target->b = persisted.scalarBool;
            break;
        case UniformType::Vec2:
        case UniformType::Vec3:
        case UniformType::Vec4:
            target->v = persisted.vec;
            break;
    }
    target->dirty = true;
    return true;
}

void UniformRegistry::Rebuild(const std::vector<UniformDescriptor>& descriptors) {
    std::unordered_map<std::string, UniformValue> previousValues;
    previousValues.reserve(values_.size());
    for (const auto& value : values_) {
        previousValues[value.desc.name] = value;
    }

    std::vector<UniformValue> nextValues;
    nextValues.reserve(descriptors.size());
    for (const auto& descriptor : descriptors) {
        UniformValue value;
        value.desc = descriptor;
        value.desc.location = -1;
        ResetUniformToDefault(&value);

        if (auto previousIt = previousValues.find(descriptor.name); previousIt != previousValues.end()) {
            const UniformValue& previous = previousIt->second;
            if (previous.desc.type == descriptor.type) {
                value.f = previous.f;
                value.i = previous.i;
                value.b = previous.b;
                value.v = previous.v;
            }
        } else if (auto persistedIt = persistedValues_.find(descriptor.name); persistedIt != persistedValues_.end()) {
            (void)ApplyPersistedValue(&value, persistedIt->second);
        }

        value.dirty = true;
        nextValues.push_back(std::move(value));
    }

    values_ = std::move(nextValues);
    RebuildIndex();

    if (currentProgram_ != 0) {
        const GLuint reboundProgram = currentProgram_;
        currentProgram_ = 0;
        BindProgram(reboundProgram);
    }
}

void UniformRegistry::BindProgram(const GLuint programHandle) {
    if (currentProgram_ == programHandle) {
        return;
    }

    currentProgram_ = programHandle;
    for (auto& value : values_) {
        if (currentProgram_ != 0) {
            value.desc.location = glGetUniformLocation(currentProgram_, value.desc.name.c_str());
        } else {
            value.desc.location = -1;
        }
        value.dirty = true;
    }
}

void UniformRegistry::UploadDirty() {
    if (currentProgram_ == 0) {
        return;
    }

    for (auto& value : values_) {
        if (!value.dirty || value.desc.location < 0) {
            continue;
        }

        switch (value.desc.type) {
            case UniformType::Float:
                glUniform1f(value.desc.location, value.f);
                break;
            case UniformType::Int:
                glUniform1i(value.desc.location, value.i);
                break;
            case UniformType::Bool:
                glUniform1i(value.desc.location, value.b ? 1 : 0);
                break;
            case UniformType::Vec2:
                glUniform2fv(value.desc.location, 1, value.v.data());
                break;
            case UniformType::Vec3:
                glUniform3fv(value.desc.location, 1, value.v.data());
                break;
            case UniformType::Vec4:
                glUniform4fv(value.desc.location, 1, value.v.data());
                break;
        }

        value.dirty = false;
    }
}

bool UniformRegistry::ApplyEdit(const UniformEditCommand& command) {
    if (command.action == UniformEditAction::ResetAll) {
        bool changed = false;
        for (auto& value : values_) {
            ResetUniformToDefault(&value);
            changed = true;
        }
        return changed;
    }

    const auto it = indexByName_.find(command.name);
    if (it == indexByName_.end()) {
        return false;
    }
    UniformValue& value = values_[it->second];

    switch (command.action) {
        case UniformEditAction::SetFloat:
            if (value.desc.type != UniformType::Float) {
                return false;
            }
            value.f = command.floatValue;
            value.dirty = true;
            return true;

        case UniformEditAction::SetInt:
            if (value.desc.type != UniformType::Int) {
                return false;
            }
            value.i = command.intValue;
            value.dirty = true;
            return true;

        case UniformEditAction::SetBool:
            if (value.desc.type != UniformType::Bool && value.desc.type != UniformType::Int) {
                return false;
            }
            if (value.desc.type == UniformType::Bool) {
                value.b = command.boolValue;
            } else {
                value.i = command.boolValue ? 1 : 0;
            }
            value.dirty = true;
            return true;

        case UniformEditAction::SetVec:
            if (value.desc.type != UniformType::Vec2 &&
                value.desc.type != UniformType::Vec3 &&
                value.desc.type != UniformType::Vec4) {
                return false;
            }
            if (command.components != UniformComponentCount(value.desc.type)) {
                return false;
            }
            value.v = command.vecValue;
            value.dirty = true;
            return true;

        case UniformEditAction::ResetOne:
            ResetUniformToDefault(&value);
            return true;

        case UniformEditAction::ResetAll:
            return false;
    }

    return false;
}

std::string UniformRegistry::EscapeJsonString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 2);
    for (const char c : value) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

std::string UniformRegistry::SerializeToJson() const {
    if (values_.empty()) {
        return "{}\n";
    }

    std::ostringstream out;
    out << "{\n";
    for (std::size_t i = 0; i < values_.size(); ++i) {
        const auto& value = values_[i];
        out << "  \"" << EscapeJsonString(value.desc.name) << "\": {\"type\":\""
            << UniformTypeToString(value.desc.type) << "\",\"value\":";

        switch (value.desc.type) {
            case UniformType::Float:
                out << value.f;
                break;
            case UniformType::Int:
                out << value.i;
                break;
            case UniformType::Bool:
                out << (value.b ? "true" : "false");
                break;
            case UniformType::Vec2:
                out << "[" << value.v[0] << "," << value.v[1] << "]";
                break;
            case UniformType::Vec3:
                out << "[" << value.v[0] << "," << value.v[1] << "," << value.v[2] << "]";
                break;
            case UniformType::Vec4:
                out << "[" << value.v[0] << "," << value.v[1] << "," << value.v[2] << "," << value.v[3] << "]";
                break;
        }

        out << "}";
        if (i + 1 < values_.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "}\n";
    return out.str();
}

bool UniformRegistry::LoadFromJson(const std::string& json) {
    persistedValues_.clear();
    if (json.empty()) {
        return true;
    }

    JsonCursor cursor(json);
    if (!cursor.ConsumeChar('{')) {
        return false;
    }

    if (cursor.ConsumeChar('}')) {
        return true;
    }

    while (true) {
        std::string uniformName;
        if (!cursor.ParseString(&uniformName)) {
            return false;
        }
        if (!cursor.ConsumeChar(':') || !cursor.ConsumeChar('{')) {
            return false;
        }

        std::string typeText;
        ParsedEntryValue parsedValue;
        bool hasType = false;
        bool hasValue = false;

        if (!cursor.ConsumeChar('}')) {
            while (true) {
                std::string fieldName;
                if (!cursor.ParseString(&fieldName) || !cursor.ConsumeChar(':')) {
                    return false;
                }

                if (fieldName == "type") {
                    if (!cursor.ParseString(&typeText)) {
                        return false;
                    }
                    hasType = true;
                } else if (fieldName == "value") {
                    if (!ParseEntryValue(&cursor, &parsedValue)) {
                        return false;
                    }
                    hasValue = true;
                } else if (!cursor.SkipValue()) {
                    return false;
                }

                if (cursor.ConsumeChar('}')) {
                    break;
                }
                if (!cursor.ConsumeChar(',')) {
                    return false;
                }
            }
        }

        if (hasType && hasValue) {
            UniformType type = UniformType::Float;
            if (UniformTypeFromString(typeText, &type)) {
                PersistedValue persisted;
                persisted.type = type;

                bool accepted = false;
                switch (type) {
                    case UniformType::Float:
                        if (parsedValue.kind == ParsedEntryValue::Kind::Number) {
                            persisted.scalarFloat = static_cast<float>(parsedValue.number);
                            accepted = true;
                        }
                        break;
                    case UniformType::Int:
                        if (parsedValue.kind == ParsedEntryValue::Kind::Number) {
                            persisted.scalarInt = static_cast<int>(std::lround(parsedValue.number));
                            accepted = true;
                        }
                        break;
                    case UniformType::Bool:
                        if (parsedValue.kind == ParsedEntryValue::Kind::Bool) {
                            persisted.scalarBool = parsedValue.boolValue;
                            accepted = true;
                        } else if (parsedValue.kind == ParsedEntryValue::Kind::Number) {
                            persisted.scalarBool = (std::lround(parsedValue.number) != 0);
                            accepted = true;
                        }
                        break;
                    case UniformType::Vec2:
                    case UniformType::Vec3:
                    case UniformType::Vec4:
                        if (parsedValue.kind == ParsedEntryValue::Kind::Array) {
                            const int expectedComponents = UniformComponentCount(type);
                            if (parsedValue.components == expectedComponents) {
                                persisted.vec = parsedValue.vec;
                                persisted.components = parsedValue.components;
                                accepted = true;
                            }
                        }
                        break;
                }

                if (accepted) {
                    persistedValues_[uniformName] = persisted;
                }
            }
        }

        if (cursor.ConsumeChar('}')) {
            break;
        }
        if (!cursor.ConsumeChar(',')) {
            return false;
        }
    }

    if (!cursor.AtEnd()) {
        return false;
    }

    for (auto& value : values_) {
        if (auto persistedIt = persistedValues_.find(value.desc.name); persistedIt != persistedValues_.end()) {
            (void)ApplyPersistedValue(&value, persistedIt->second);
        }
    }

    return true;
}
