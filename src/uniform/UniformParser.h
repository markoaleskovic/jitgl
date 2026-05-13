#pragma once

#include <string>
#include <vector>

#include "uniform/UniformControls.h"

std::vector<UniformDescriptor> ParseUniformDescriptors(const std::string& shaderSource);
