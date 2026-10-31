/**
 * ntfs.c - Simple functionality for startup, mounting and unmounting of NTFS-based devices.
 *
 * Copyright (c) 2023 Extrems
 * Copyright (c) 2009 Rhys "Shareese" Koedijk
 * Copyright (c) 2006 Michael "Chishm" Chisholm
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "ntfs.h"
#include "ntfsinternal.h"
#include "ntfsfile.h"
#include "ntfsdir.h"
#include "gekko_io.h"
#include "cache.h"
#include "bootsect.h"

// NTFS device driver devoptab
static const devoptab_t devops_ntfs = {
    NULL, /* Device name */
    sizeof (ntfs_file_state),
    ntfs_open_r,
    ntfs_close_r,
    ntfs_write_r,
    ntfs_read_r,
    ntfs_seek_r,
    ntfs_fstat_r,
    ntfs_stat_r,
    NULL, // link_r
    ntfs_unlink_r,
    ntfs_chdir_r,
    ntfs_rename_r,
    ntfs_mkdir_r,
    sizeof (ntfs_dir_state),
    ntfs_diropen_r,
    ntfs_dirreset_r,
    ntfs_dirnext_r,
    ntfs_dirclose_r,
    ntfs_statvfs_r,
    ntfs_ftruncate_r,
    ntfs_fsync_r,
    NULL, /* Device data */
    NULL, // chmod_r
    NULL, // fchmod_r
    ntfs_rmdir_r,
    ntfs_lstat_r,
    NULL, // utimes_r
    NULL, // fpathconf_r
    NULL, // pathconf_r
    ntfs_symlink_r,
    NULL, // readlink_r
};

void ntfsInit (void)
{
    static bool isInit = false;

    // Initialise ntfs-3g (if not already done so)
    if (!isInit) {
        isInit = true;

        // Set the log handler
        #ifdef NTFS_ENABLE_LOG
        ntfs_log_set_handler(ntfs_log_handler_stderr);
        #else
        ntfs_log_set_handler(ntfs_log_handler_null);
        #endif
    }

    return;
}

/* GPT disks: the protective MBR points at a partition table in LBA 1. Find
 * the Microsoft basic-data partitions and keep the ones whose boot sector is
 * NTFS. (MBR and extended partitions are handled below.) */
static int ntfsFindGPT (DISC_INTERFACE *interface, sec_t **partitions)
{
    static const u8 ms_basic_data[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
    };
    u8 header[512] ATTRIBUTE_ALIGN(32);
    u8 *entries = NULL;
    sec_t starts[NTFS_MAX_PARTITIONS];
    u64 entry_lba;
    u32 nentries, entry_size, bytes, sectors;
    int count = 0, i;

    if (!interface->readSectors(1, 1, header))
        return -1;
    if (memcmp(header, "EFI PART", 8))
        return 0;
    memcpy(&entry_lba, header + 72, 8);
    entry_lba = le64_to_cpu(entry_lba);
    memcpy(&nentries, header + 80, 4);
    nentries = le32_to_cpu(nentries);
    memcpy(&entry_size, header + 84, 4);
    entry_size = le32_to_cpu(entry_size);
    if (entry_size < 128 || entry_size > 4096 || nentries == 0 || nentries > 128)
        return 0;
    bytes = nentries * entry_size;
    sectors = (bytes + 511) / 512;
    entries = ntfs_malloc((size_t)sectors * 512);
    if (!entries)
        return -1;
    if (!interface->readSectors((sec_t)entry_lba, sectors, entries)) {
        ntfs_free(entries);
        return -1;
    }
    for (i = 0; i < (int)nentries && count < NTFS_MAX_PARTITIONS; i++) {
        const u8 *e = entries + (size_t)i * entry_size;
        union {
            u8 buffer[MAX_SECTOR_SIZE] ATTRIBUTE_ALIGN(32);
            NTFS_BOOT_SECTOR boot;
        } sec;
        u64 first, last;
        if (memcmp(e, ms_basic_data, sizeof ms_basic_data))
            continue;
        memcpy(&first, e + 32, 8);
        first = le64_to_cpu(first);
        memcpy(&last, e + 40, 8);
        last = le64_to_cpu(last);
        if (!first || first >= last || first > 0xFFFFFFFFu)
            continue;
        if (!interface->readSectors((sec_t)first, 1, &sec.buffer))
            continue;
        if (!ntfs_boot_sector_is_ntfs(&sec.boot))
            continue;
        ntfs_log_debug("GPT partition %i: Valid NTFS boot sector found\n", i + 1);
        starts[count++] = (sec_t)first;
    }
    ntfs_free(entries);
    if (!count)
        return 0;
    *partitions = ntfs_malloc(sizeof(sec_t) * count);
    if (!*partitions)
        return -1;
    memcpy(*partitions, starts, sizeof(sec_t) * count);
    return count;
}

int ntfsFindPartitions (DISC_INTERFACE *interface, sec_t **partitions)
{
    MASTER_BOOT_RECORD mbr;
    PARTITION_RECORD *partition = NULL;
    sec_t partition_starts[NTFS_MAX_PARTITIONS] = {0};
    int partition_count = 0;
    sec_t part_lba = 0;
    int i;

    union {
        u8 buffer[MAX_SECTOR_SIZE] ATTRIBUTE_ALIGN(32);
        MASTER_BOOT_RECORD mbr;
        EXTENDED_BOOT_RECORD ebr;
        NTFS_BOOT_SECTOR boot;
    } sector;

    // Sanity check
    if (!interface) {
        errno = EINVAL;
        return -1;
    }
    if (!partitions)
        return 0;

    // Initialise ntfs-3g
    ntfsInit();

    // Start the device and check that it is inserted
    if (!interface->startup()) {
        errno = EIO;
        return -1;
    }
    if (!interface->isInserted()) {
        return 0;
    }

    // Read the first sector on the device
    if (!interface->readSectors(0, 1, &sector.buffer)) {
        errno = EIO;
        return -1;
    }

    if (ntfs_boot_sector_is_ntfs(&sector.boot)) {
        ntfs_log_debug("Valid NTFS boot sector found\n");
        if (partition_count < NTFS_MAX_PARTITIONS) {
            partition_starts[partition_count] = 0;
            partition_count++;
        }

    // If this is the devices master boot record
    } else if (sector.mbr.signature == MBR_SIGNATURE) {
        memcpy(&mbr, &sector, sizeof(MASTER_BOOT_RECORD));
        for (i = 0; i < 4; i++)
            if (mbr.partitions[i].type == 0xEE)
                return ntfsFindGPT(interface, partitions);
        ntfs_log_debug("Valid Master Boot Record found\n");

        // Search the partition table for all NTFS partitions (max. 4 primary partitions)
        for (i = 0; i < 4; i++) {
            partition = &mbr.partitions[i];
            part_lba = le32_to_cpu(mbr.partitions[i].lba_start);

            ntfs_log_debug("Partition %i: %s, sector %lld, type 0x%x\n", i + 1,
                           partition->status == PARTITION_STATUS_BOOTABLE ? "bootable (active)" : "non-bootable",
                           part_lba, partition->type);

            // Figure out what type of partition this is
            switch (partition->type) {

                // Ignore empty partitions
                case PARTITION_TYPE_EMPTY:
                    continue;

                // NTFS partition
                case PARTITION_TYPE_NTFS: {
                    ntfs_log_debug("Partition %i: Claims to be NTFS\n", i + 1);

                    // Read and validate the NTFS partition
                    if (interface->readSectors(part_lba, 1, &sector)) {
                        if (ntfs_boot_sector_is_ntfs(&sector.boot)) {
                            ntfs_log_debug("Partition %i: Valid NTFS boot sector found\n", i + 1);
                            if (partition_count < NTFS_MAX_PARTITIONS) {
                                partition_starts[partition_count] = part_lba;
                                partition_count++;
                            }
                        } else {
                            ntfs_log_debug("Partition %i: Invalid NTFS boot sector, not actually NTFS\n", i + 1);
                        }
                    }

                    break;

                }

                // DOS 3.3+ or Windows 95 extended partition
                case PARTITION_TYPE_DOS33_EXTENDED:
                case PARTITION_TYPE_WIN95_EXTENDED: {
                    ntfs_log_debug("Partition %i: Claims to be Extended\n", i + 1);

                    // Walk the extended partition chain, finding all NTFS partitions within it
                    sec_t ebr_lba = part_lba;
                    sec_t next_erb_lba = 0;
                    do {

                        // Read and validate the extended boot record
                        if (interface->readSectors(ebr_lba + next_erb_lba, 1, &sector)) {
                            if (sector.ebr.signature == EBR_SIGNATURE) {
                                ntfs_log_debug("Logical Partition @ %lld: %s type 0x%x\n", ebr_lba + next_erb_lba,
                                               sector.ebr.partition.status == PARTITION_STATUS_BOOTABLE ? "bootable (active)" : "non-bootable",
                                               sector.ebr.partition.type);

                                // Get the start sector of the current partition
                                // and the next extended boot record in the chain
                                part_lba = ebr_lba + next_erb_lba + le32_to_cpu(sector.ebr.partition.lba_start);
                                next_erb_lba = le32_to_cpu(sector.ebr.next_ebr.lba_start);

                                // Check if this partition has a valid NTFS boot record
                                if (interface->readSectors(part_lba, 1, &sector)) {
                                    if (ntfs_boot_sector_is_ntfs(&sector.boot)) {
                                        ntfs_log_debug("Logical Partition @ %lld: Valid NTFS boot sector found\n", part_lba);
                                        if(sector.ebr.partition.type != PARTITION_TYPE_NTFS) {
                                            ntfs_log_warning("Logical Partition @ %lld: Is NTFS but type is 0x%x; 0x%x was expected\n", part_lba, sector.ebr.partition.type, PARTITION_TYPE_NTFS);
                                        }
                                        if (partition_count < NTFS_MAX_PARTITIONS) {
                                            partition_starts[partition_count] = part_lba;
                                            partition_count++;
                                        }
                                    }
                                }

                            } else {
                                next_erb_lba = 0;
                            }
                        }

                    } while (next_erb_lba);

                    break;

                }

                // Unknown or unsupported partition type
                default: {

                    // Check if this partition has a valid NTFS boot record anyway,
                    // it might be misrepresented due to a lazy partition editor
                    if (interface->readSectors(part_lba, 1, &sector)) {
                        if (ntfs_boot_sector_is_ntfs(&sector.boot)) {
                            ntfs_log_debug("Partition %i: Valid NTFS boot sector found\n", i + 1);
                            if(partition->type != PARTITION_TYPE_NTFS) {
                                ntfs_log_warning("Partition %i: Is NTFS but type is 0x%x; 0x%x was expected\n", i + 1, partition->type, PARTITION_TYPE_NTFS);
                            }
                            if (partition_count < NTFS_MAX_PARTITIONS) {
                                partition_starts[partition_count] = part_lba;
                                partition_count++;
                            }
                        }
                    }

                    break;

                }

            }

        }

    // Else it is assumed this device has no master boot record
    } else {
        ntfs_log_debug("No Master Boot Record was found!\n");

        // As a last-ditched effort, search the first 64 sectors of the device for stray NTFS partitions
        for (i = 1; i < 64; i++) {
            if (interface->readSectors(i, 1, &sector)) {
                if (ntfs_boot_sector_is_ntfs(&sector.boot)) {
                    ntfs_log_debug("Valid NTFS boot sector found at sector %d!\n", i);
                    if (partition_count < NTFS_MAX_PARTITIONS) {
                        partition_starts[partition_count] = i;
                        partition_count++;
                    }
                }
            }
        }

    }

    // Return the found partitions (if any)
    if (partition_count > 0) {
        *partitions = (sec_t*)ntfs_malloc(sizeof(sec_t) * partition_count);
        if (*partitions) {
            memcpy(*partitions, &partition_starts, sizeof(sec_t) * partition_count);
            return partition_count;
        }
    }

    return 0;
}

int ntfsMountAll (ntfs_md **mounts, u32 flags)
{
    const INTERFACE_ID *discs = ntfsGetDiscInterfaces();
    const INTERFACE_ID *disc = NULL;
    ntfs_md mount_points[NTFS_MAX_MOUNTS];
    sec_t *partitions = NULL;
    int mount_count = 0;
    int partition_count = 0;
    char name[128];
    int i, j, k;

    // Initialise ntfs-3g
    ntfsInit();

    // Find and mount all NTFS partitions on all known devices
    for (i = 0; discs[i].name != NULL && discs[i].interface != NULL; i++) {
        disc = &discs[i];
        partition_count = ntfsFindPartitions(disc->interface, &partitions);
        if (partition_count > 0 && partitions) {
            for (j = 0, k = 0; j < partition_count; j++) {

                // Find the next unused mount name
                do {
                    sprintf(name, "%s%i", NTFS_MOUNT_PREFIX, k++);
                    if (k >= NTFS_MAX_MOUNTS) {
                        ntfs_free(partitions);
                        errno = EADDRNOTAVAIL;
                        return -1;
                    }
                } while (ntfsGetDevice(name, false));

                // Mount the partition
                if (mount_count < NTFS_MAX_MOUNTS) {
                    if (ntfsMount(name, disc->interface, partitions[j], CACHE_DEFAULT_PAGE_SIZE, CACHE_DEFAULT_PAGE_COUNT, flags)) {
                        strcpy(mount_points[mount_count].name, name);
                        mount_points[mount_count].interface = disc->interface;
                        mount_points[mount_count].startSector = partitions[j];
                        mount_count++;
                    }
                }

            }
            ntfs_free(partitions);
        }
    }

    // Return the mounts (if any)
    if (mount_count > 0 && mounts) {
        *mounts = (ntfs_md*)ntfs_malloc(sizeof(ntfs_md) * mount_count);
        if (*mounts) {
            memcpy(*mounts, &mount_points, sizeof(ntfs_md) * mount_count);
            return mount_count;
        }
    }

    return 0;
}

int ntfsMountDevice (DISC_INTERFACE *interface, ntfs_md **mounts, u32 flags)
{
    const INTERFACE_ID *discs = ntfsGetDiscInterfaces();
    const INTERFACE_ID *disc = NULL;
    ntfs_md mount_points[NTFS_MAX_MOUNTS];
    sec_t *partitions = NULL;
    int mount_count = 0;
    int partition_count = 0;
    char name[128];
    int i, j, k;

    // Sanity check
    if (!interface) {
        errno = EINVAL;
        return -1;
    }

    // Initialise ntfs-3g
    ntfsInit();

    // Find the specified device then find and mount all NTFS partitions on it
    for (i = 0; discs[i].name != NULL && discs[i].interface != NULL; i++) {
        if (discs[i].interface == interface) {
            disc = &discs[i];
            partition_count = ntfsFindPartitions(disc->interface, &partitions);
            if (partition_count > 0 && partitions) {
                for (j = 0, k = 0; j < partition_count; j++) {

                    // Find the next unused mount name
                    do {
                        sprintf(name, "%s%i", NTFS_MOUNT_PREFIX, k++);
                        if (k >= NTFS_MAX_MOUNTS) {
                            ntfs_free(partitions);
                            errno = EADDRNOTAVAIL;
                            return -1;
                        }
                    } while (ntfsGetDevice(name, false));

                    // Mount the partition
                    if (mount_count < NTFS_MAX_MOUNTS) {
                        if (ntfsMount(name, disc->interface, partitions[j], CACHE_DEFAULT_PAGE_SIZE, CACHE_DEFAULT_PAGE_COUNT, flags)) {
                            strcpy(mount_points[mount_count].name, name);
                            mount_points[mount_count].interface = disc->interface;
                            mount_points[mount_count].startSector = partitions[j];
                            mount_count++;
                        }
                    }

                }
                ntfs_free(partitions);
            }
            break;
        }
    }

    // If we couldn't find the device then return with error status
    if (!disc) {
        errno = ENODEV;
        return -1;
    }

    // Return the mounts (if any)
    if (mount_count > 0 && mounts) {
        *mounts = (ntfs_md*)ntfs_malloc(sizeof(ntfs_md) * mount_count);
        if (*mounts) {
            memcpy(*mounts, &mount_points, sizeof(ntfs_md) * mount_count);
            return mount_count;
        }
    }

    return 0;
}

bool ntfsMount (const char *name, DISC_INTERFACE *interface, sec_t startSector, u32 cachePageCount, u32 cachePageSize, u32 flags)
{
    ntfs_vd *vd = NULL;
    gekko_fd *fd = NULL;

    // Sanity check
    if (!name || !interface) {
        errno = EINVAL;
        return false;
    }

    // Initialise ntfs-3g
    ntfsInit();

    // Check that the requested mount name is free
    if (ntfsGetDevice(name, false)) {
        errno = EADDRINUSE;
        return false;
    }

    // Check that we can at least read from this device
    if (!(interface->features & FEATURE_MEDIUM_CANREAD)) {
        errno = EPERM;
        return false;
    }

    // Allocate the volume descriptor
    vd = (ntfs_vd*)ntfs_malloc(sizeof(ntfs_vd));
    if (!vd) {
        errno = ENOMEM;
        return false;
    }

    // Setup the volume descriptor
    vd->id = interface->ioType;
    vd->flags = 0;
    vd->uid = 0;
    vd->gid = 0;
    vd->fmask = 0;
    vd->dmask = 0;
    vd->atime = ((flags & NTFS_UPDATE_ACCESS_TIMES) ? ATIME_ENABLED : ATIME_DISABLED);

    // Allocate the device driver descriptor
    fd = (gekko_fd*)ntfs_malloc(sizeof(gekko_fd));
    if (!fd) {
        ntfs_free(vd);
        errno = ENOMEM;
        return false;
    }

    // Setup the device driver descriptor
    fd->interface = interface;
    fd->startSector = startSector;
    fd->sectorSize = 0;
    fd->sectorCount = 0;
    fd->cachePageCount = cachePageCount;
    fd->cachePageSize = cachePageSize;

    // Allocate the device driver
    vd->dev = ntfs_device_alloc(name, 0, &ntfs_device_gekko_io_ops, fd);
    if (!vd->dev) {
        ntfs_free(fd);
        ntfs_free(vd);
        return false;
    }

    // Build the mount flags
    if (flags & NTFS_READ_ONLY)
    	vd->flags |= NTFS_MNT_RDONLY;
    else
    {
	    if (!(interface->features & FEATURE_MEDIUM_CANWRITE))
	        vd->flags |= NTFS_MNT_RDONLY;
	    if ((interface->features & FEATURE_MEDIUM_CANREAD) && (interface->features & FEATURE_MEDIUM_CANWRITE))
	        vd->flags |= NTFS_MNT_EXCLUSIVE;
    }
    if (flags & NTFS_RECOVER)
        vd->flags |= NTFS_MNT_RECOVER;
    if (flags & NTFS_IGNORE_HIBERFILE)
        vd->flags |= NTFS_MNT_IGNORE_HIBERFILE;

    if (vd->flags & NTFS_MNT_RDONLY)
        ntfs_log_debug("Mounting \"%s\" as read-only\n", name);

    // Mount the device
    vd->vol = ntfs_device_mount(vd->dev, vd->flags);
    if (!vd->vol) {
        switch(ntfs_volume_error(errno)) {
            case NTFS_VOLUME_NOT_NTFS: errno = EINVALPART; break;
            case NTFS_VOLUME_CORRUPT: errno = EINVALPART; break;
            case NTFS_VOLUME_HIBERNATED: errno = EHIBERNATED; break;
            case NTFS_VOLUME_UNCLEAN_UNMOUNT: errno = EDIRTY; break;
            default: errno = EINVAL; break;
        }
        ntfs_device_free(vd->dev);
        ntfs_free(vd);
        return false;
    }

    ntfs_set_shown_files(vd->vol, flags & NTFS_SHOW_SYSTEM_FILES, flags & NTFS_SHOW_HIDDEN_FILES, TRUE);

    if (flags & NTFS_IGNORE_CASE)
        ntfs_set_ignore_case(vd->vol);

    // Initialise the volume descriptor
    if (ntfsInitVolume(vd)) {
        ntfs_umount(vd->vol, true);
        ntfs_free(vd);
        return false;
    }

    // Add the device to the devoptab table
    if (ntfsAddDevice(name, vd)) {
        ntfsDeinitVolume(vd);
        ntfs_umount(vd->vol, true);
        ntfs_free(vd);
        return false;
    }

    return true;
}

void ntfsUnmount (const char *name, bool force)
{
    ntfs_vd *vd = NULL;

    // Get the devices volume descriptor
    vd = ntfsGetVolume(name, false);
    if (!vd)
        return;

    // Remove the device from the devoptab table
    ntfsRemoveDevice(name);

    // Deinitialise the volume descriptor
    ntfsDeinitVolume(vd);

    // Unmount the volume
    ntfs_umount(vd->vol, force);

    // Free the volume descriptor
    ntfs_free(vd);

    return;
}

const char *ntfsGetVolumeName (const char *name)
{
    ntfs_vd *vd = NULL;

    // Sanity check
    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    // Get the devices volume descriptor
    vd = ntfsGetVolume(name, false);
    if (!vd) {
        errno = ENODEV;
        return NULL;
    }
    return vd->vol->vol_name;
}

bool ntfsSetVolumeName (const char *name, const char *volumeName)
{
    ntfs_vd *vd = NULL;
    ntfs_attr *na = NULL;
    ntfschar *ulabel = NULL;
    int ulabel_len;

    // Sanity check
    if (!name) {
        errno = EINVAL;
        return false;
    }

    // Get the devices volume descriptor
    vd = ntfsGetVolume(name, false);
    if (!vd) {
        errno = ENODEV;
        return false;
    }

    // Lock
    ntfsLock(vd);

    // Convert the new volume name to unicode
    ulabel_len = ntfsLocalToUnicode(volumeName, &ulabel) * sizeof(ntfschar);
    if (ulabel_len < 0) {
        ntfsUnlock(vd);
        errno = EINVAL;
        return false;
    }

    // Check if the volume name attribute exists
    na = ntfs_attr_open(vd->vol->vol_ni, AT_VOLUME_NAME, NULL, 0);
    if (na) {

        // It does, resize it to match the length of the new volume name
        if (ntfs_attr_truncate(na, ulabel_len)) {
            free(ulabel);
            ntfsUnlock(vd);
            return false;
        }

        // Write the new volume name
        if (ntfs_attr_pwrite(na, 0, ulabel_len, ulabel) != ulabel_len) {
            free(ulabel);
            ntfsUnlock(vd);
            return false;
        }

    } else {

        // It doesn't, create it now
        if (ntfs_attr_add(vd->vol->vol_ni, AT_VOLUME_NAME, NULL, 0, (u8*)ulabel, ulabel_len)) {
            free(ulabel);
            ntfsUnlock(vd);
            return false;
        }

    }

    // Reset the volumes name cache (as it has now been changed)
    vd->name[0] = '\0';

    // Close the volume name attribute
    if (na)
        ntfs_attr_close(na);

    // Sync the volume node
    if (ntfs_inode_sync(vd->vol->vol_ni)) {
        free(ulabel);
        ntfsUnlock(vd);
        return false;
    }

    // Clean up
    free(ulabel);

    // Unlock
    ntfsUnlock(vd);

    return true;
}

const devoptab_t *ntfsGetDevOpTab (void)
{
    return &devops_ntfs;
}
