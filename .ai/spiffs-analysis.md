# SPIFFS vs LittleFS Analysis for binary_storage

## Date: 2025-11-19

## Summary

**Recommendation: Do NOT implement SPIFFS - LittleFS is sufficient**

---

## 1. Size Threshold Analysis

### User's Question
"Is ~1MB a good threshold to decide between LittleFS and SPIFFS?"

### Answer: No - It's About File Size Patterns, Not Total Capacity

| Aspect | LittleFS | SPIFFS |
|--------|----------|--------|
| **Min file allocation** | 4KB | 256 bytes |
| **Ideal for** | General use, larger files | Many small files (configs, state) |
| **Sweet spot** | Any size, especially >64KB | <128KB with many small files |

**Better heuristic:**
- **SPIFFS**: Best for devices storing **many small files** (<1KB each) like config fragments, state flags
- **LittleFS**: Best for **general use** - larger files, logs, any scenario needing reliability

The 1MB threshold isn't about total capacity - it's about **file size patterns**:
- Lots of tiny files (10-100 bytes each)? → SPIFFS saves space due to 256-byte allocation
- Normal files (1KB+) → LittleFS is better in every way

---

## 2. Should Users Choose Between Filesystems?

### Pros of Offering Choice
- User knows their use case best
- Allows optimization for specific scenarios
- Future-proof if SPIFFS gets updates

### Cons of Offering Choice
- SPIFFS is **deprecated** by Espressif
- SPIFFS has **no directory support** (flat namespace with `/` in filenames)
- SPIFFS **not power-fail safe** (corruption risk)
- More code complexity for marginal benefit
- Users might make wrong choice without understanding tradeoffs

### Recommendation: No - Don't Add SPIFFS

1. **SPIFFS is deprecated** - "not being developed and maintained anymore"
2. **LittleFS is strictly better** for reliability (power-safe, wear-leveling)
3. The marginal space savings from SPIFFS's 256-byte allocation is rarely worth the reliability tradeoff
4. Adding choice creates maintenance burden and user confusion

---

## 3. Technical Implementation Analysis

### New Methods Needed?

**No new base class methods needed.** The existing `BinaryStorage` interface provides exactly what SPIFFS HAL needs:
- `read(address, data, length)` → `hal_read_f`
- `write(address, data, length)` → `hal_write_f`
- `erase_block(address)` → `hal_erase_f`

### Critical Finding: ESP-IDF esp_spiffs is Partition-Only

The raw SPIFFS library (pellepl/spiffs) supports custom HAL callbacks, but **ESP-IDF's esp_spiffs does NOT expose this!**

ESP-IDF only provides:
```c
esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf)
```

This **requires a partition** - no custom block device support.

### Implementation Options

1. **Use raw pellepl/spiffs + custom VFS wrapper** (like we did for LittleFS)
   - Need: `spiffs_mount.h`, `spiffs_mount.cpp`
   - Same pattern as `littlefs_mount.cpp`
   - Must implement VFS functions ourselves

2. **Abuse esp_partition_register_external** (hacky)
   - Only works with `esp_flash_t` devices (SPI NOR flash)
   - Won't work for FRAM, EEPROM, MRAM
   - Limited to Flash-based devices only

---

## 4. Technical Comparison

| Feature | SPIFFS | LittleFS |
|---------|--------|----------|
| Min allocation | 256 bytes | 4K |
| Directory support | No (fake with `/`) | Yes |
| Power-failure safe | Limited | Yes |
| Development status | **Deprecated** | Active |
| Max flash size | 128 MB | N/A |
| RAM usage | Very low | Low (~100 bytes) |
| Wear leveling | Basic | Efficient |
| Garbage collection | Manual/slow | Automatic |
| Best for | Many small files | General use, reliability |

---

## 5. Integration Plan (If Needed)

### Architecture

```
esphome/components/binary_storage/
├── spiffs_mount.h        # NEW
├── spiffs_mount.cpp      # NEW
├── littlefs_mount.h
├── littlefs_mount.cpp
└── ...
```

### Steps

**Phase 1: Library Integration**
- Add pellepl/spiffs library (raw, not esp_spiffs)
- Configure `spiffs_config.h` for ESP32
- Enable `SPIFFS_HAL_CALLBACK_EXTRA` for context pointer

**Phase 2: C++ Implementation**
- Create `SPIFFSMount` class (mirrors `LittleFSMount`)
- HAL callbacks mapping to `BinaryStorage`
- VFS wrapper functions
- Mount/unmount/format functions

**Phase 3: Python Codegen**
- Add `MODE_SPIFFS = "spiffs"` constant
- Add `SPIFFSMount` class registration
- Update schema for `spiffs` mode option
- Add `cg.add_library()` for spiffs
- Add `cg.add_define("USE_BINARY_STORAGE_SPIFFS")`

**Phase 4: Header Updates**
- Add `#define USE_BINARY_STORAGE_SPIFFS` to `esphome/core/defines.h`

### Complexity Estimate
- **Lines of code**: ~800-1000 (similar to littlefs_mount)
- **Development time**: Medium
- **Testing**: Requires extensive power-fail testing

---

## 6. Final Recommendation

**Do NOT implement SPIFFS.** Reasons:

1. **SPIFFS is deprecated** - Espressif has moved on
2. **LittleFS is better in every measurable way** except min allocation size
3. **The implementation complexity is the same** - no savings
4. **User confusion** - "which one should I use?" leads to wrong choices
5. **Maintenance burden** - two filesystems to support
6. **FRAM/MRAM don't need SPIFFS** - unlimited write cycles, wear leveling irrelevant
7. **EEPROM is too small** - 64KB max, SPIFFS overhead makes it impractical

**The only valid use case for SPIFFS:**
- ESP32 internal flash with many tiny (<256 byte) config files
- But this is already handled by ESP-IDF's native SPIFFS on partitions

---

## References

- ESP-IDF SPIFFS docs: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/spiffs.html
- pellepl/spiffs: https://github.com/pellepl/spiffs
- ESP-IDF File System Considerations: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/file-system-considerations.html
