#pragma once

#include <atomic>
#include <filesystem>
#include <string>

bool firmware_installed(const std::filesystem::path& flash);
bool firmware_extract(const std::filesystem::path& pup, const std::filesystem::path& flash, std::atomic<float>& progress, std::string& error,
                      const std::atomic<bool>* cancel = nullptr);
bool firmware_download(const std::filesystem::path& pup, std::atomic<float>& progress, std::string& error,
                       const std::atomic<bool>* cancel = nullptr);
bool update_download(const std::filesystem::path& pkg, std::atomic<float>& progress, std::string& error,
                     const std::atomic<bool>* cancel = nullptr, const std::string& title_id = "BLES01227");
