#pragma once

#include <FsLib/FsVolume.h>

namespace freeink {

inline bool mountSdmmcVolume(FsVolume& volume, FsBlockDevice* device) {
  // Preserve the existing MBR-first behavior. Some cards instead contain a
  // whole-device FAT/exFAT volume; let SdFat validate that boot sector too.
  return volume.begin(device) || volume.begin(device, true, 0, 0);
}

}  // namespace freeink
