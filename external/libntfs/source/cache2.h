/*
 cache2.h
 The cache is not visible to the user. It should be flushed
 when any file is closed or changes are made to the filesystem.

 This cache implements a least-used-page replacement policy. This will
 distribute sectors evenly over the pages, so if less than the maximum
 pages are used at once, they should all eventually remain in the cache.
 This also has the benefit of throwing out old sectors, so as not to keep
 too many stale pages around.

 Copyright (c) 2006 Michael "Chishm" Chisholm
 Copyright (c) 2009 shareese, rodries
 Copyright (c) 2024 Extrems

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.
  3. The name of the author may not be used to endorse or promote products derived
     from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _CACHE2_H
#define _CACHE2_H

#include <stddef.h>
#include <stdint.h>
#include <gctypes.h>
#include <ogc/disc_io.h>
#include <gccore.h>

typedef struct {
	sec_t        sector;
	unsigned int count;
	unsigned int last_access;
	uint64_t     dirty;
	uint8_t*     cache;
} NTFS_CACHE_ENTRY;

typedef struct {
	DISC_INTERFACE*   disc;
	sec_t             endOfPartition;
	unsigned int      numberOfPages;
	unsigned int      sectorsPerPage;
	unsigned int      bytesPerSector;
	NTFS_CACHE_ENTRY* cacheEntries;
} NTFS_CACHE;

/*
Read data from a sector in the cache
If the sector is not in the cache, it will be swapped in
offset is the position to start reading from
size is the amount of data to read
Precondition: offset + size <= BYTES_PER_READ
*/
bool _NTFS_cache_readPartialSector (NTFS_CACHE* cache, void* buffer, sec_t sector, unsigned int offset, size_t size);

bool _NTFS_cache_readLittleEndianValue (NTFS_CACHE* cache, uint32_t *value, sec_t sector, unsigned int offset, int num_bytes);

/*
Write data to a sector in the cache
If the sector is not in the cache, it will be swapped in.
When the sector is swapped out, the data will be written to the disc
offset is the position to start writing to
size is the amount of data to write
Precondition: offset + size <= BYTES_PER_READ
*/
bool _NTFS_cache_writePartialSector (NTFS_CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size);

bool _NTFS_cache_writeLittleEndianValue (NTFS_CACHE* cache, const uint32_t value, sec_t sector, unsigned int offset, int num_bytes);

/*
Write data to a sector in the cache, zeroing the sector first
If the sector is not in the cache, it will be swapped in.
When the sector is swapped out, the data will be written to the disc
offset is the position to start writing to
size is the amount of data to write
Precondition: offset + size <= BYTES_PER_READ
*/
bool _NTFS_cache_eraseWritePartialSector (NTFS_CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size);

/*
Read several sectors from the cache
*/
bool _NTFS_cache_readSectors (NTFS_CACHE* cache, sec_t sector, sec_t numSectors, void* buffer);

/*
Read a full sector from the cache
*/
static inline bool _NTFS_cache_readSector (NTFS_CACHE* cache, void* buffer, sec_t sector) {
	return _NTFS_cache_readPartialSector (cache, buffer, sector, 0, cache->bytesPerSector);
}

/*
Write a full sector to the cache
*/
static inline bool _NTFS_cache_writeSector (NTFS_CACHE* cache, const void* buffer, sec_t sector) {
	return _NTFS_cache_writePartialSector (cache, buffer, sector, 0, cache->bytesPerSector);
}

bool _NTFS_cache_writeSectors (NTFS_CACHE* cache, sec_t sector, sec_t numSectors, const void* buffer);

/*
Write any dirty sectors back to disc and clear out the contents of the cache
*/
bool _NTFS_cache_flush (NTFS_CACHE* cache);

/*
Clear out the contents of the cache without writing any dirty sectors first
*/
void _NTFS_cache_invalidate (NTFS_CACHE* cache);

NTFS_CACHE* _NTFS_cache_constructor (unsigned int numberOfPages, unsigned int sectorsPerPage, DISC_INTERFACE* discInterface, sec_t endOfPartition, unsigned int bytesPerSector);

void _NTFS_cache_destructor (NTFS_CACHE* cache);

#endif // _CACHE_H

