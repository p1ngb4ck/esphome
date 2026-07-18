#pragma once
// file_system selection for FatFs-backed media (sd_storage / usb_storage).
//
// Only exists in builds with esp32 enable_exfat — codegen defines
// USE_STORAGE_FILE_SYSTEM_SELECT together with the YAML option (see
// storage/__init__.py: file_system_to_code). Builds without exFAT contain none of this and
// their mount paths are untouched.
//
// Header-only on purpose: compiled where included, so the storage component itself takes no
// FatFs dependency.

#ifdef USE_STORAGE_FILE_SYSTEM_SELECT

#include "esphome/core/log.h"

#include "diskio.h"  // NOLINT(modernize-deprecated-headers) - FatFs's own header
#include "ff.h"

#include <cstring>
#include <memory>

namespace esphome::storage {

// Values match the codegen mapping in storage/__init__.py file_system_to_code().
static constexpr uint8_t FS_SELECT_AUTO = 0;
static constexpr uint8_t FS_SELECT_FAT32 = 1;
static constexpr uint8_t FS_SELECT_EXFAT = 2;

namespace fatfs_select_detail {

enum class Detected : uint8_t { NONE, FAT, EXFAT };

inline bool has_boot_signature(const uint8_t *sec) { return sec[510] == 0x55 && sec[511] == 0xAA; }

// What filesystem sits in this boot sector? The same first-bytes FatFs itself keys on:
// the exFAT name field ("EXFAT   " at offset 3) and the FAT jump instruction + signature.
inline Detected classify_boot_sector(const uint8_t *sec) {
  if (!has_boot_signature(sec))
    return Detected::NONE;
  if (memcmp(sec + 3, "EXFAT   ", 8) == 0)
    return Detected::EXFAT;
  if (sec[0] == 0xEB || sec[0] == 0xE9)
    return Detected::FAT;
  return Detected::NONE;
}

// Probe the medium BEFORE any mount: sector 0 directly, and — when sector 0 is a partition
// table instead of a boot sector — one level of indirection: the first MBR partition, or
// for a protective MBR (0xEE) the first GPT entry. Exactly the volumes FatFs would mount.
inline Detected probe(const char *tag, uint8_t pdrv) {
  auto sec = std::make_unique<uint8_t[]>(FF_MAX_SS);
  if (disk_read(pdrv, sec.get(), 0, 1) != RES_OK) {
    ESP_LOGW(tag, "file_system probe: cannot read sector 0");
    return Detected::NONE;
  }
  Detected direct = classify_boot_sector(sec.get());
  if (direct != Detected::NONE)
    return direct;
  if (!has_boot_signature(sec.get()))
    return Detected::NONE;
  // Sector 0 is a partition table. First MBR entry: type at 0x1BE+4, start LBA at 0x1BE+8.
  const uint8_t *entry = sec.get() + 0x1BE;
  uint8_t part_type = entry[4];
  uint64_t start_lba = static_cast<uint32_t>(entry[8]) | (static_cast<uint32_t>(entry[9]) << 8) |
                       (static_cast<uint32_t>(entry[10]) << 16) | (static_cast<uint32_t>(entry[11]) << 24);
  if (part_type == 0xEE) {
    // Protective MBR -> GPT. Header at LBA 1: entry array start LBA at offset 72; the first
    // entry's first LBA at offset 32 within the entry.
    if (disk_read(pdrv, sec.get(), 1, 1) != RES_OK || memcmp(sec.get(), "EFI PART", 8) != 0)
      return Detected::NONE;
    uint64_t entries_lba = 0;
    memcpy(&entries_lba, sec.get() + 72, sizeof(entries_lba));
    if (disk_read(pdrv, sec.get(), static_cast<LBA_t>(entries_lba), 1) != RES_OK)
      return Detected::NONE;
    memcpy(&start_lba, sec.get() + 32, sizeof(start_lba));
  }
  if (start_lba == 0 || disk_read(pdrv, sec.get(), static_cast<LBA_t>(start_lba), 1) != RES_OK)
    return Detected::NONE;
  return classify_boot_sector(sec.get());
}

}  // namespace fatfs_select_detail

// Make the medium carry the requested filesystem BEFORE the one and only mount happens.
// AUTO does nothing at all: f_mount's own boot-sector detection is the automatic mode.
// fat32/exfat probe the first sectors; a different (or no recognizable) filesystem is
// reformatted to the requested one right here — destructive by configured contract, and the
// subsequent mount is then already on the correct filesystem. Returns false only when the
// reformat itself failed.
inline bool ensure_requested_filesystem(const char *tag, uint8_t pdrv, const char *drive, uint8_t requested) {
  using fatfs_select_detail::Detected;
  if (requested == FS_SELECT_AUTO)
    return true;
  const bool want_exfat = requested == FS_SELECT_EXFAT;
  Detected found = fatfs_select_detail::probe(tag, pdrv);
  if ((found == Detected::EXFAT) == want_exfat && found != Detected::NONE)
    return true;
  if (found == Detected::NONE) {
    ESP_LOGW(tag, "file_system: no recognizable filesystem on the medium - formatting as %s",
             want_exfat ? "exFAT" : "FAT32");
  } else {
    ESP_LOGW(tag, "file_system: found %s but %s is configured - REFORMATTING, all data on the medium is erased",
             found == Detected::EXFAT ? "exFAT" : "FAT", want_exfat ? "exFAT" : "FAT32");
  }
  auto work = std::make_unique<uint8_t[]>(FF_MAX_SS);
  MKFS_PARM parm{};
  // FAT side deliberately FM_FAT | FM_FAT32: the request means "the FAT family, not exFAT";
  // FatFs picks the FAT width the medium size allows (forcing FAT32 on tiny media fails).
  parm.fmt = want_exfat ? FM_EXFAT : (FM_FAT | FM_FAT32);
  FRESULT res = f_mkfs(drive, &parm, work.get(), FF_MAX_SS);
  if (res != FR_OK) {
    ESP_LOGE(tag, "file_system: formatting as %s failed (FatFs error %d)", want_exfat ? "exFAT" : "FAT32", res);
    return false;
  }
  ESP_LOGI(tag, "file_system: medium formatted as %s", want_exfat ? "exFAT" : "FAT32");
  return true;
}

}  // namespace esphome::storage

#endif  // USE_STORAGE_FILE_SYSTEM_SELECT
