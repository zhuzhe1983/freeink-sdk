#include <FsLib/FsFile.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>

#include "SdmmcVolume.h"

namespace {

using Sector = std::array<uint8_t, 512>;
constexpr uint32_t kVolumeSectors = 32000000;
constexpr uint32_t kReservedSectors = 32;
constexpr uint32_t kFatSectors = 16384;
constexpr uint8_t kSectorsPerCluster = 16;
constexpr uint32_t kPartitionStart = 2048;

void put16(Sector& sector, size_t offset, uint16_t value) {
  sector[offset] = static_cast<uint8_t>(value);
  sector[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(Sector& sector, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) sector[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

// Sparse host-only image: realistic 16 GB FAT32 geometry without allocating a
// card-sized buffer. Every write is rejected and counted, including on failure.
class Image final : public FsBlockDeviceInterface {
 public:
  std::map<uint32_t, Sector> sectors;
  uint32_t unreadable = UINT32_MAX;
  size_t writes = 0;

  bool isBusy() override { return false; }
  bool syncDevice() override { return true; }
  Sector_t sectorCount() override { return kVolumeSectors + kPartitionStart; }

  bool readSector(Sector_t sector, uint8_t* destination) override {
    if (sector >= sectorCount() || sector == unreadable) return false;
    const auto found = sectors.find(sector);
    if (found == sectors.end()) {
      std::memset(destination, 0, 512);
    } else {
      std::memcpy(destination, found->second.data(), 512);
    }
    return true;
  }

  bool readSectors(Sector_t sector, uint8_t* destination, size_t count) override {
    for (size_t i = 0; i < count; ++i) {
      if (!readSector(sector + static_cast<Sector_t>(i), destination + i * 512)) return false;
    }
    return true;
  }

  bool writeSector(Sector_t, const uint8_t*) override {
    ++writes;
    return false;
  }

  bool writeSectors(Sector_t, const uint8_t*, size_t) override {
    ++writes;
    return false;
  }

  void addFat32(uint32_t start, const char* contents) {
    auto& boot = sectors[start];
    boot[0] = 0xEB;
    boot[1] = 0x58;
    boot[2] = 0x90;
    std::memcpy(boot.data() + 3, "MSDOS5.0", 8);
    put16(boot, 11, 512);
    boot[13] = kSectorsPerCluster;
    put16(boot, 14, kReservedSectors);
    boot[16] = 2;
    boot[21] = 0xF8;
    put32(boot, 28, start);
    put32(boot, 32, kVolumeSectors);
    put32(boot, 36, kFatSectors);
    put32(boot, 44, 2);
    put16(boot, 48, 1);
    put16(boot, 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    std::memcpy(boot.data() + 71, "TEST VOLUME", 11);
    std::memcpy(boot.data() + 82, "FAT32   ", 8);
    put16(boot, 510, 0xAA55);
    sectors[start + 6] = boot;

    auto& info = sectors[start + 1];
    put32(info, 0, 0x41615252);
    put32(info, 484, 0x61417272);
    put32(info, 488, UINT32_MAX);
    put32(info, 492, UINT32_MAX);
    put32(info, 508, 0xAA550000);
    sectors[start + 7] = info;

    auto& fat = sectors[start + kReservedSectors];
    put32(fat, 0, 0x0FFFFFF8);
    put32(fat, 4, 0x0FFFFFFF);
    put32(fat, 8, 0x0FFFFFFF);
    put32(fat, 12, 0x0FFFFFFF);
    sectors[start + kReservedSectors + kFatSectors] = fat;
    const uint32_t dataStart = start + kReservedSectors + 2 * kFatSectors;
    auto& root = sectors[dataStart];
    std::memcpy(root.data(), "HELLO   TXT", 11);
    root[11] = 0x20;
    put16(root, 26, 3);
    put32(root, 28, static_cast<uint32_t>(std::strlen(contents)));
    assert(std::strlen(contents) < 512);
    std::memcpy(sectors[dataStart + kSectorsPerCluster].data(), contents, std::strlen(contents));
  }

  void addMbr() {
    auto& mbr = sectors[0];
    mbr[450] = 0x0C;
    put32(mbr, 454, kPartitionStart);
    put32(mbr, 458, kVolumeSectors);
    put16(mbr, 510, 0xAA55);
  }
};

void readExpected(FsVolume& volume, const char* expected) {
  assert(volume.fatType() == 32);
  assert(volume.bytesPerCluster() == 8192);
  assert(volume.clusterCount() == (kVolumeSectors - kReservedSectors - 2 * kFatSectors) / kSectorsPerCluster);
  FsFile file = volume.open("/HELLO.TXT", O_RDONLY);
  assert(file);
  char contents[64] = {};
  assert(file.read(contents, sizeof(contents)) == static_cast<int>(std::strlen(expected)));
  assert(std::strcmp(contents, expected) == 0);
  assert(file.read() == -1);
}

void expectRejected(Image& image) {
  FsVolume volume;
  assert(!freeink::mountSdmmcVolume(volume, &image));
  assert(volume.fatType() == 0);
  assert(image.writes == 0);
}

}  // namespace

int main() {
  {
    Image image;
    image.addFat32(kPartitionStart, "partition");
    image.addMbr();
    FsVolume volume;
    assert(freeink::mountSdmmcVolume(volume, &image));
    assert(volume.fatStartSector() == kPartitionStart + kReservedSectors);
    readExpected(volume, "partition");
    assert(image.writes == 0);
  }
  {
    Image image;
    image.addFat32(0, "whole device");
    FsVolume volume;
    // Reproduce the former failure with the exact old default call first.
    assert(!volume.begin(&image));
    assert(freeink::mountSdmmcVolume(volume, &image));
    assert(volume.fatStartSector() == kReservedSectors);
    readExpected(volume, "whole device");
    // USB raw-access remount and recovery after a failed read use the same object.
    volume.end();
    image.unreadable = 0;
    assert(!freeink::mountSdmmcVolume(volume, &image));
    image.unreadable = UINT32_MAX;
    assert(freeink::mountSdmmcVolume(volume, &image));
    readExpected(volume, "whole device");
    assert(image.writes == 0);
  }
  {
    // A deliberately dual-valid image proves MBR success wins over sector 0.
    Image image;
    image.addFat32(0, "fallback must not win");
    image.addFat32(kPartitionStart, "MBR wins");
    image.addMbr();
    FsVolume volume;
    assert(freeink::mountSdmmcVolume(volume, &image));
    readExpected(volume, "MBR wins");
    assert(image.writes == 0);
  }
  {
    Image image;
    expectRejected(image);  // Unformatted media.
    put16(image.sectors[0], 510, 0xAA55);
    expectRejected(image);  // Signature alone is not a filesystem.
    image.addMbr();
    expectRejected(image);  // Partition table pointing at a blank boot sector.
    image.unreadable = kPartitionStart;
    expectRejected(image);
  }
  // Real SdFat BPB validation must reject these corrupt fields on both layouts.
  for (const uint32_t start : {uint32_t(0), kPartitionStart}) {
    for (const auto& change : std::array<std::array<uint16_t, 2>, 5>{{
             {{11, 0}},
             {{13, 129}},
             {{13, 3}},
             {{16, 0}},
             {{16, 3}},
         }}) {
      Image image;
      image.addFat32(start, "must not mount");
      if (start != 0) image.addMbr();
      image.sectors[start][change[0]] = static_cast<uint8_t>(change[1]);
      if (change[0] == 11) image.sectors[start][12] = 4;  // 1024-byte sectors.
      expectRejected(image);
    }
  }
  std::puts("PASS: real SdFat MBR/superfloppy, MBR priority, readback, remount, 14 invalid-media cases, zero writes");
}
