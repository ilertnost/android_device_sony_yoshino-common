/*
 * SPDX-FileCopyrightText: 2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fstream>

#include "GloveMode.h"

namespace aidl {
namespace vendor {
namespace lineage {
namespace touch {

const std::string kGloveModePath = "/sys/devices/virtual/input/clearpad/glove";

ndk::ScopedAStatus GloveMode::getEnabled(bool* _aidl_return) {
    std::ifstream file(kGloveModePath);
    bool enabled;

    file >> enabled;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GloveMode::setEnabled(bool enabled) {
    std::ofstream file(kGloveModePath);

    file << enabled << std::flush;

    return ndk::ScopedAStatus::ok();
}

}  // namespace touch
}  // namespace lineage
}  // namespace vendor
}  // namespace aidl
