#pragma once

#include <string>

struct ImGuiTestEngine;

unsigned int registerVisualCapture(ImGuiTestEngine *engine, std::string &outputDirectory);
