#pragma once

#include <string>
#include <thread>

void bindThreadToCpu(std::thread& t, int cpu_id, const std::string& tag);
