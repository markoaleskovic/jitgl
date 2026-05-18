#pragma once

#include <string>
#include <vector>

#include "uniform/UniformControls.h"

std::vector<UniformDescriptor> ParseUniformDescriptors(const std::string& shaderSource);
std::vector<std::string> ParseSamplerUniformNames(const std::string& shaderSource);
std::vector<std::string> ParseStorageBufferNames(const std::string& shaderSource);
