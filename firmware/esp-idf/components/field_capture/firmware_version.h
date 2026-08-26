/**
 * @file firmware_version.h
 * @brief Global hardware + firmware version strings.
 *
 * Embedded in the header of every SD file the device writes, so every
 * recording can be traced back to the exact hardware revision and firmware
 * build that produced it.
 *
 * Versioning scheme (MAJOR.MINOR.PATCH):
 *   - PATCH -- bumped by Claude on any driver / firmware code change
 *   - MINOR -- bumped by Ivan on feature adds
 *   - MAJOR -- bumped when a release is happy (beta / RC / GA)
 *
 * Each individual component ALSO carries its own <NAME>_DRIVER_VERSION macro
 * at the top of its main header. When any of those bumps, KOMPIC_FW_VERSION
 * bumps here too so file-level provenance always matches the source tree.
 */

#ifndef FIRMWARE_VERSION_H
#define FIRMWARE_VERSION_H

#define KOMPIC_HW_VERSION   "iv7.1"
#define KOMPIC_FW_VERSION   "0.4.17"

#endif // FIRMWARE_VERSION_H
