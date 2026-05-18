#include "uniform/UniformParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    std::string Trim(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    std::string_view TrimView(std::string_view view) {
        while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front()))) {
            view.remove_prefix(1);
        }
        while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back()))) {
            view.remove_suffix(1);
        }
        return view;
    }

    bool IsIdentifierStart(const char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    bool IsIdentifierChar(const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    bool ParseFloat(const std::string& value, float* out) {
        if (out == nullptr) {
            return false;
        }
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        if (end == value.c_str()) {
            return false;
        }
        while (*end != '\0') {
            if (!std::isspace(static_cast<unsigned char>(*end))) {
                return false;
            }
            ++end;
        }
        *out = parsed;
        return true;
    }

    std::string ExtractParenthesized(std::string_view text, std::size_t startIndex, std::size_t* outEndIndex) {
        if (startIndex >= text.size() || text[startIndex] != '(') {
            return {};
        }

        int depth = 0;
        for (std::size_t i = startIndex; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    if (outEndIndex != nullptr) {
                        *outEndIndex = i + 1;
                    }
                    return std::string(text.substr(startIndex + 1, i - startIndex - 1));
                }
            }
        }
        return {};
    }

    std::string ParseQuotedString(std::string_view text) {
        text = TrimView(text);
        if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
            return std::string(text);
        }

        std::string out;
        out.reserve(text.size() - 2);
        bool escape = false;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            const char c = text[i];
            if (escape) {
                out.push_back(c);
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            out.push_back(c);
        }
        return out;
    }

    struct UniformHintState {
        bool hasRange = false;
        float rangeMin = 0.0f;
        float rangeMax = 1.0f;
        bool hasStep = false;
        float step = 0.0f;
        bool color = false;
        bool toggle = false;
        bool hidden = false;
        std::string label;
        std::string group;

        void MergeFrom(const UniformHintState& rhs) {
            if (rhs.hasRange) {
                hasRange = true;
                rangeMin = rhs.rangeMin;
                rangeMax = rhs.rangeMax;
            }
            if (rhs.hasStep) {
                hasStep = true;
                step = rhs.step;
            }
            if (rhs.color) {
                color = true;
            }
            if (rhs.toggle) {
                toggle = true;
            }
            if (rhs.hidden) {
                hidden = true;
            }
            if (!rhs.label.empty()) {
                label = rhs.label;
            }
            if (!rhs.group.empty()) {
                group = rhs.group;
            }
        }

        bool IsEmpty() const {
            return !hasRange && !hasStep && !color && !toggle && !hidden && label.empty() && group.empty();
        }
    };

    UniformHintState ParseHintAnnotations(std::string_view commentText) {
        UniformHintState hints;
        std::size_t cursor = 0;
        while (cursor < commentText.size()) {
            const std::size_t at = commentText.find('@', cursor);
            if (at == std::string_view::npos) {
                break;
            }
            cursor = at + 1;
            std::size_t nameEnd = cursor;
            while (nameEnd < commentText.size() &&
                   (std::isalnum(static_cast<unsigned char>(commentText[nameEnd])) || commentText[nameEnd] == '_')) {
                ++nameEnd;
            }
            if (nameEnd == cursor) {
                continue;
            }

            const std::string hintName(commentText.substr(cursor, nameEnd - cursor));
            cursor = nameEnd;
            while (cursor < commentText.size() && std::isspace(static_cast<unsigned char>(commentText[cursor]))) {
                ++cursor;
            }

            std::string hintArgs;
            if (cursor < commentText.size() && commentText[cursor] == '(') {
                std::size_t endIndex = cursor;
                hintArgs = ExtractParenthesized(commentText, cursor, &endIndex);
                if (!hintArgs.empty() && endIndex > cursor) {
                    cursor = endIndex;
                }
            }

            if (hintName == "range") {
                const std::size_t comma = hintArgs.find(',');
                if (comma != std::string::npos) {
                    float minValue = 0.0f;
                    float maxValue = 0.0f;
                    if (ParseFloat(Trim(hintArgs.substr(0, comma)), &minValue) &&
                        ParseFloat(Trim(hintArgs.substr(comma + 1)), &maxValue)) {
                        hints.hasRange = true;
                        hints.rangeMin = minValue;
                        hints.rangeMax = maxValue;
                    }
                }
            } else if (hintName == "step") {
                float step = 0.0f;
                if (ParseFloat(Trim(hintArgs), &step)) {
                    hints.hasStep = true;
                    hints.step = step;
                }
            } else if (hintName == "color") {
                hints.color = true;
            } else if (hintName == "toggle") {
                hints.toggle = true;
            } else if (hintName == "hidden") {
                hints.hidden = true;
            } else if (hintName == "label") {
                hints.label = ParseQuotedString(hintArgs);
            } else if (hintName == "group") {
                hints.group = ParseQuotedString(hintArgs);
            }
        }
        return hints;
    }

    bool IsPrecisionToken(const std::string& token) {
        return token == "highp" || token == "mediump" || token == "lowp";
    }

    bool ParseUniformType(const std::string& token, UniformType* outType) {
        if (outType == nullptr) {
            return false;
        }
        if (token == "float") {
            *outType = UniformType::Float;
            return true;
        }
        if (token == "int") {
            *outType = UniformType::Int;
            return true;
        }
        if (token == "bool") {
            *outType = UniformType::Bool;
            return true;
        }
        if (token == "vec2") {
            *outType = UniformType::Vec2;
            return true;
        }
        if (token == "vec3") {
            *outType = UniformType::Vec3;
            return true;
        }
        if (token == "vec4") {
            *outType = UniformType::Vec4;
            return true;
        }
        return false;
    }

    bool ConsumeToken(std::string_view* text, std::string* outToken) {
        if (text == nullptr || outToken == nullptr) {
            return false;
        }
        *text = TrimView(*text);
        if (text->empty()) {
            return false;
        }
        std::size_t cursor = 0;
        while (cursor < text->size() && !std::isspace(static_cast<unsigned char>((*text)[cursor]))) {
            ++cursor;
        }
        *outToken = std::string(text->substr(0, cursor));
        text->remove_prefix(cursor);
        *text = TrimView(*text);
        return !outToken->empty();
    }

    bool ParseUniformDeclaration(std::string_view code,
                                 UniformType* outType,
                                 std::vector<std::string>* outNames) {
        if (outType == nullptr || outNames == nullptr) {
            return false;
        }
        outNames->clear();

        code = TrimView(code);
        if (!code.starts_with("uniform")) {
            if (code.starts_with("layout")) {
                const std::size_t uniformPos = code.find("uniform");
                if (uniformPos == std::string_view::npos) {
                    return false;
                }
                const bool leftBoundary = (uniformPos == 0) ||
                                          !std::isalnum(static_cast<unsigned char>(code[uniformPos - 1]));
                const std::size_t rightPos = uniformPos + std::char_traits<char>::length("uniform");
                const bool rightBoundary = (rightPos >= code.size()) ||
                                           !std::isalnum(static_cast<unsigned char>(code[rightPos]));
                if (!leftBoundary || !rightBoundary) {
                    return false;
                }
                code = code.substr(uniformPos);
            } else {
                return false;
            }
        }
        if (!code.starts_with("uniform")) {
            return false;
        }
        code.remove_prefix(std::char_traits<char>::length("uniform"));
        code = TrimView(code);

        const std::size_t semicolon = code.find(';');
        if (semicolon == std::string_view::npos) {
            return false;
        }
        code = code.substr(0, semicolon);
        code = TrimView(code);
        if (code.empty()) {
            return false;
        }

        std::string firstToken;
        if (!ConsumeToken(&code, &firstToken)) {
            return false;
        }

        std::string typeToken = firstToken;
        if (IsPrecisionToken(firstToken)) {
            if (!ConsumeToken(&code, &typeToken)) {
                return false;
            }
        }

        UniformType type = UniformType::Float;
        if (!ParseUniformType(typeToken, &type)) {
            return false;
        }

        std::string namesList = Trim(std::string(code));
        if (namesList.empty()) {
            return false;
        }

        std::size_t start = 0;
        while (start <= namesList.size()) {
            const std::size_t comma = namesList.find(',', start);
            const std::size_t end = (comma == std::string::npos) ? namesList.size() : comma;
            std::string nameToken = Trim(namesList.substr(start, end - start));

            if (const std::size_t equals = nameToken.find('='); equals != std::string::npos) {
                nameToken = Trim(nameToken.substr(0, equals));
            }
            if (nameToken.find('[') != std::string::npos || nameToken.find(']') != std::string::npos) {
                nameToken.clear();
            }

            if (!nameToken.empty() && IsIdentifierStart(nameToken.front())) {
                const bool valid = std::all_of(nameToken.begin() + 1, nameToken.end(), IsIdentifierChar);
                if (valid) {
                    outNames->push_back(nameToken);
                }
            }

            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        if (outNames->empty()) {
            return false;
        }

        *outType = type;
        return true;
    }

    bool ParseSamplerUniformDeclaration(std::string_view code,
                                        std::vector<std::string>* outNames) {
        if (outNames == nullptr) {
            return false;
        }
        outNames->clear();

        code = TrimView(code);
        if (!code.starts_with("uniform")) {
            if (code.starts_with("layout")) {
                const std::size_t uniformPos = code.find("uniform");
                if (uniformPos == std::string_view::npos) {
                    return false;
                }
                const bool leftBoundary = (uniformPos == 0) ||
                                          !std::isalnum(static_cast<unsigned char>(code[uniformPos - 1]));
                const std::size_t rightPos = uniformPos + std::char_traits<char>::length("uniform");
                const bool rightBoundary = (rightPos >= code.size()) ||
                                           !std::isalnum(static_cast<unsigned char>(code[rightPos]));
                if (!leftBoundary || !rightBoundary) {
                    return false;
                }
                code = code.substr(uniformPos);
            } else {
                return false;
            }
        }
        if (!code.starts_with("uniform")) {
            return false;
        }
        code.remove_prefix(std::char_traits<char>::length("uniform"));
        code = TrimView(code);

        const std::size_t semicolon = code.find(';');
        if (semicolon == std::string_view::npos) {
            return false;
        }
        code = code.substr(0, semicolon);
        code = TrimView(code);
        if (code.empty()) {
            return false;
        }

        std::string firstToken;
        if (!ConsumeToken(&code, &firstToken)) {
            return false;
        }

        std::string typeToken = firstToken;
        if (IsPrecisionToken(firstToken)) {
            if (!ConsumeToken(&code, &typeToken)) {
                return false;
            }
        }
        if (typeToken != "sampler2D") {
            return false;
        }

        std::string namesList = Trim(std::string(code));
        if (namesList.empty()) {
            return false;
        }

        std::size_t start = 0;
        while (start <= namesList.size()) {
            const std::size_t comma = namesList.find(',', start);
            const std::size_t end = (comma == std::string::npos) ? namesList.size() : comma;
            std::string nameToken = Trim(namesList.substr(start, end - start));

            if (const std::size_t equals = nameToken.find('='); equals != std::string::npos) {
                nameToken = Trim(nameToken.substr(0, equals));
            }
            if (nameToken.find('[') != std::string::npos || nameToken.find(']') != std::string::npos) {
                nameToken.clear();
            }

            if (!nameToken.empty() && IsIdentifierStart(nameToken.front())) {
                const bool valid = std::all_of(nameToken.begin() + 1, nameToken.end(), IsIdentifierChar);
                if (valid) {
                    outNames->push_back(nameToken);
                }
            }

            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        return !outNames->empty();
    }

    bool ParseStorageBufferDeclaration(std::string_view code,
                                       std::string* outName) {
        if (outName == nullptr) {
            return false;
        }
        outName->clear();

        code = TrimView(code);
        if (!code.starts_with("buffer")) {
            if (code.starts_with("layout")) {
                const std::size_t bufferPos = code.find("buffer");
                if (bufferPos == std::string_view::npos) {
                    return false;
                }
                const bool leftBoundary = (bufferPos == 0) ||
                                          !std::isalnum(static_cast<unsigned char>(code[bufferPos - 1]));
                const std::size_t rightPos = bufferPos + std::char_traits<char>::length("buffer");
                const bool rightBoundary = (rightPos >= code.size()) ||
                                           !std::isalnum(static_cast<unsigned char>(code[rightPos]));
                if (!leftBoundary || !rightBoundary) {
                    return false;
                }
                code = code.substr(bufferPos);
            } else {
                return false;
            }
        }
        if (!code.starts_with("buffer")) {
            return false;
        }
        code.remove_prefix(std::char_traits<char>::length("buffer"));
        code = TrimView(code);
        if (code.empty()) {
            return false;
        }

        std::size_t nameEnd = 0;
        while (nameEnd < code.size() && IsIdentifierChar(code[nameEnd])) {
            ++nameEnd;
        }
        if (nameEnd == 0) {
            return false;
        }
        if (!IsIdentifierStart(code.front())) {
            return false;
        }

        *outName = std::string(code.substr(0, nameEnd));
        return !outName->empty();
    }

    UniformDescriptor BuildDescriptor(const std::string& uniformName,
                                      UniformType type,
                                      const UniformHintState& hints) {
        UniformDescriptor descriptor;
        descriptor.name = uniformName;
        descriptor.type = type;
        descriptor.widget = (type == UniformType::Bool) ? UniformWidgetHint::Toggle : UniformWidgetHint::Slider;
        descriptor.rangeMin = 0.0f;
        descriptor.rangeMax = 1.0f;
        descriptor.step = 0.0f;
        descriptor.hidden = hints.hidden;
        descriptor.label = hints.label;
        descriptor.group = hints.group;

        if (hints.hasRange) {
            descriptor.rangeMin = hints.rangeMin;
            descriptor.rangeMax = hints.rangeMax;
            if (descriptor.rangeMax < descriptor.rangeMin) {
                std::swap(descriptor.rangeMin, descriptor.rangeMax);
            }
        } else if (type == UniformType::Int) {
            descriptor.rangeMin = 0.0f;
            descriptor.rangeMax = 10.0f;
        }

        if (hints.hasStep && hints.step > 0.0f) {
            descriptor.step = hints.step;
        }

        if (hints.toggle && (type == UniformType::Bool || type == UniformType::Int)) {
            descriptor.widget = UniformWidgetHint::Toggle;
        }
        if (hints.color && (type == UniformType::Vec3 || type == UniformType::Vec4)) {
            descriptor.widget = UniformWidgetHint::Color;
        }

        return descriptor;
    }
}

std::vector<UniformDescriptor> ParseUniformDescriptors(const std::string& shaderSource) {
    std::vector<UniformDescriptor> descriptors;
    descriptors.reserve(16);
    std::unordered_set<std::string> seenNames;
    seenNames.reserve(32);

    UniformHintState pendingHints;
    std::size_t cursor = 0;
    while (cursor <= shaderSource.size()) {
        const std::size_t lineEnd = shaderSource.find('\n', cursor);
        const std::size_t lineLength = (lineEnd == std::string::npos) ? (shaderSource.size() - cursor)
                                                                       : (lineEnd - cursor);
        const std::string line = shaderSource.substr(cursor, lineLength);

        const std::size_t commentPos = line.find("//");
        const std::string code = (commentPos == std::string::npos) ? line : line.substr(0, commentPos);
        const std::string comment = (commentPos == std::string::npos) ? std::string() : line.substr(commentPos + 2);

        const UniformHintState inlineHints = ParseHintAnnotations(comment);
        const bool hasInlineHints = !inlineHints.IsEmpty();
        const std::string trimmedCode = Trim(code);

        if (trimmedCode.empty()) {
            if (hasInlineHints) {
                pendingHints.MergeFrom(inlineHints);
            } else {
                pendingHints = UniformHintState{};
            }
        } else {
            UniformType type = UniformType::Float;
            std::vector<std::string> names;
            if (ParseUniformDeclaration(trimmedCode, &type, &names)) {
                UniformHintState effectiveHints = pendingHints;
                if (hasInlineHints) {
                    effectiveHints.MergeFrom(inlineHints);
                }
                pendingHints = UniformHintState{};

                for (const auto& uniformName : names) {
                    if (!seenNames.insert(uniformName).second) {
                        continue;
                    }
                    descriptors.push_back(BuildDescriptor(uniformName, type, effectiveHints));
                }
            } else {
                pendingHints = UniformHintState{};
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        cursor = lineEnd + 1;
    }

    return descriptors;
}

std::vector<std::string> ParseSamplerUniformNames(const std::string& shaderSource) {
    std::vector<std::string> names;
    names.reserve(8);
    std::unordered_set<std::string> seenNames;
    seenNames.reserve(16);

    std::size_t cursor = 0;
    while (cursor <= shaderSource.size()) {
        const std::size_t lineEnd = shaderSource.find('\n', cursor);
        const std::size_t lineLength = (lineEnd == std::string::npos) ? (shaderSource.size() - cursor)
                                                                       : (lineEnd - cursor);
        const std::string line = shaderSource.substr(cursor, lineLength);

        const std::size_t commentPos = line.find("//");
        const std::string code = (commentPos == std::string::npos) ? line : line.substr(0, commentPos);
        const std::string trimmedCode = Trim(code);
        if (!trimmedCode.empty()) {
            std::vector<std::string> localNames;
            if (ParseSamplerUniformDeclaration(trimmedCode, &localNames)) {
                for (const auto& name : localNames) {
                    if (seenNames.insert(name).second) {
                        names.push_back(name);
                    }
                }
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        cursor = lineEnd + 1;
    }

    return names;
}

std::vector<std::string> ParseStorageBufferNames(const std::string& shaderSource) {
    std::vector<std::string> names;
    names.reserve(8);
    std::unordered_set<std::string> seenNames;
    seenNames.reserve(16);

    std::size_t cursor = 0;
    while (cursor <= shaderSource.size()) {
        const std::size_t lineEnd = shaderSource.find('\n', cursor);
        const std::size_t lineLength = (lineEnd == std::string::npos) ? (shaderSource.size() - cursor)
                                                                       : (lineEnd - cursor);
        const std::string line = shaderSource.substr(cursor, lineLength);

        const std::size_t commentPos = line.find("//");
        const std::string code = (commentPos == std::string::npos) ? line : line.substr(0, commentPos);
        const std::string trimmedCode = Trim(code);
        if (!trimmedCode.empty()) {
            std::string localName;
            if (ParseStorageBufferDeclaration(trimmedCode, &localName) &&
                seenNames.insert(localName).second) {
                names.push_back(std::move(localName));
            }
        }

        if (lineEnd == std::string::npos) {
            break;
        }
        cursor = lineEnd + 1;
    }

    return names;
}
