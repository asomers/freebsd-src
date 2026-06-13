/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 voidanix <voidanix@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef	_G_ZONED_H_
#define	_G_ZONED_H_

#include <sys/endian.h>

#define	G_ZONED_CLASS_NAME	"ZONED"
#define	G_ZONED_MAGIC		"GEOM::ZONED"
#define	G_ZONED_TABLE_MAGIC	"GEOM::ZONEDTBL"

/*
 * Version history:
 * 1 - Initial version number.
 */
#define	G_ZONED_VERSION		1

/*
 * On-disk layout, carved out of the end of the backing provider:
 *
 *   [ .... usable (zoned) region .... | table header | zone entries | metadata ]
 *                                       (1 sector)     (N entries)    (last sec)
 *
 * The metadata sector is written by the userland "create" command and read by
 * the kernel taster. The table header + zone entries hold the live per-zone
 * state and are maintained entirely by the kernel.
 */
#define	G_ZONED_ENTRY_SIZE	16	/* On-disk size of one zone entry. */

/*
 * Metadata stored in the provider's last sector. Decoded/encoded via the
 * below helpers for the on-disk format to be endianness-independent.
 */
struct g_zoned_metadata {
	char		md_magic[16];	/* Magic value. */
	uint32_t	md_version;	/* Version number. */
	char		md_name[16];	/* Zoned device name. */
	uint32_t	md_id;		/* Unique ID. */
	uint64_t	md_zonesize;	/* Zone size, in bytes. */
	uint32_t	md_nzones;	/* Number of zones. */
	uint32_t	md_sectorsize;	/* Provider sector size, in bytes. */
	uint64_t	md_provsize;	/* Provider size, in bytes. */
};

static __inline void
zoned_metadata_encode(const struct g_zoned_metadata *md, u_char *data)
{

	bcopy(md->md_magic, data, sizeof(md->md_magic));
	le32enc(data + 16, md->md_version);
	bcopy(md->md_name, data + 20, sizeof(md->md_name));
	le32enc(data + 36, md->md_id);
	le64enc(data + 40, md->md_zonesize);
	le32enc(data + 48, md->md_nzones);
	le32enc(data + 52, md->md_sectorsize);
	le64enc(data + 56, md->md_provsize);
}

static __inline void
zoned_metadata_decode(const u_char *data, struct g_zoned_metadata *md)
{

	bcopy(data, md->md_magic, sizeof(md->md_magic));
	md->md_version = le32dec(data + 16);
	bcopy(data + 20, md->md_name, sizeof(md->md_name));
	md->md_id = le32dec(data + 36);
	md->md_zonesize = le64dec(data + 40);
	md->md_nzones = le32dec(data + 48);
	md->md_sectorsize = le32dec(data + 52);
	md->md_provsize = le64dec(data + 56);
}

/*
 * Number of zones a provider of the given geometry can hold, after reserving
 * room at the tail for the metadata sector and the zone-state table. Returns 0
 * if the zone size does not leave room for even a single zone.
 */
static __inline uint32_t
g_zoned_nzones(off_t mediasize, off_t zonesize, u_int secsize)
{
	uint64_t nmax, tbl_sectors, reserve;

	if (zonesize <= 0 || secsize == 0 || mediasize <= 0)
		return (0);
	/* Over-reserve using the zone count that ignores the reservation. */
	nmax = (uint64_t)mediasize / (uint64_t)zonesize;
	if (nmax == 0)
		return (0);
	tbl_sectors = 1 + howmany(nmax * G_ZONED_ENTRY_SIZE, secsize);
	reserve = (1 + tbl_sectors) * secsize;	/* metadata + table */
	if ((uint64_t)mediasize <= reserve)
		return (0);
	return ((uint32_t)(((uint64_t)mediasize - reserve) /
	    (uint64_t)zonesize));
}

#ifdef _KERNEL
#define	G_ZONED_DEBUG(lvl, ...) \
    _GEOM_DEBUG("GEOM_ZONED", g_zoned_debug, (lvl), NULL, __VA_ARGS__)
#define	G_ZONED_LOGREQLVL(lvl, bp, ...) \
    _GEOM_DEBUG("GEOM_ZONED", g_zoned_debug, (lvl), (bp), __VA_ARGS__)
#define	G_ZONED_LOGREQ(bp, ...)	G_ZONED_LOGREQLVL(2, bp, __VA_ARGS__)

/* Packed image of the on-disk table; kept and flushed lazily. */
struct g_zoned_softc {
	struct mtx			 sc_lock;
	char				 sc_name[16];	/* Device name. */
	uint32_t			 sc_id;		/* Unique ID. */
	off_t				 sc_zonesize;	/* Zone size in bytes. */
	u_int				 sc_secsize;	/* Sector size in bytes. */
	uint64_t			 sc_zonesecs;	/* Zone size in sectors. */
	uint32_t			 sc_nzones;	/* Number of zones. */
	uint64_t			 sc_maxlba;	/* Last usable LBA + 1. */
	struct disk_zone_rep_entry	*sc_zones;	/* Entries of sc_nzones. */
	/* Zone-state table image and placement + dirty tracking. */
	u_char				*sc_tab;	/* sc_tabsecs * secsize. */
	uint32_t			 sc_tabsecs;	/* Table size in sectors. */
	off_t				 sc_taboff;	/* Table byte offset. */
	bool				 sc_dirty;	/* Pending table writes. */
	uint32_t			 sc_dirtylo;	/* First dirty tab sector. */
	uint32_t			 sc_dirtyhi;	/* Last dirty tab sector. */
	/* Statistics. */
	uintmax_t			 sc_reads;
	uintmax_t			 sc_writes;
	uintmax_t			 sc_readbytes;
	uintmax_t			 sc_wrotebytes;
	uintmax_t			 sc_zonecmds;
};

static __inline void
g_zoned_entry_encode(const struct disk_zone_rep_entry *z, u_char *data)
{

	data[0] = z->zone_type;
	data[1] = z->zone_condition;
	data[2] = z->zone_flags;
	data[3] = 0;
	le32enc(data + 4, 0);
	le64enc(data + 8, z->write_pointer_lba);
}

static __inline void
g_zoned_entry_decode(const u_char *data, struct disk_zone_rep_entry *z)
{

	z->zone_type = data[0];
	z->zone_condition = data[1];
	z->zone_flags = data[2];
	z->write_pointer_lba = le64dec(data + 8);
}
#endif	/* _KERNEL */

#endif	/* _G_ZONED_H_ */
