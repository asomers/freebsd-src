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

/*
 * GEOM_ZONED presents a plain (non-zoned) provider to the rest of the system
 * as a host-managed zoned block device. The medium is divided into equal
 * sequential-write-required zones. This GEOM class tracks a write pointer per
 * zone and answers BIO_ZONE management commands similarly to a real ZBC/ZAC
 * drive would.
 *
 * Persistence:
 *   - The provider's last sector holds a metadata block, written by the
 *     userland "create" command. The kernel tastes it on every provider
 *     arrival and re-creates the zoned device automatically.
 *   - The sectors just before it hold the per-zone state (condition + write
 *     pointer). That table is read at taste time and rewritten lazily.
 *     Zone-state changes are committed to disk on BIO_FLUSH (and the table is
 *     also flushed when zone-management commands run), mirroring a drive whose
 *     zone state is volatile until a cache flush. Changes since the last
 *     flush may be rolled back by an unclean shutdown.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/disk_zone.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>

#include <geom/geom.h>
#include <geom/geom_dbg.h>
#include <geom/zoned/g_zoned.h>

FEATURE(geom_zoned, "GEOM Zoned Storage Medium emulation");

static MALLOC_DEFINE(M_ZONED, "zoned_data", "GEOM_ZONED Data");

SYSCTL_DECL(_kern_geom);
static SYSCTL_NODE(_kern_geom, OID_AUTO, zoned, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "GEOM_ZONED stuff");
static u_int g_zoned_debug = 0;
SYSCTL_UINT(_kern_geom_zoned, OID_AUTO, debug, CTLFLAG_RW, &g_zoned_debug, 0,
    "Debug level");

static g_access_t g_zoned_access;
static g_dumpconf_t g_zoned_dumpconf;
static g_orphan_t g_zoned_orphan;
static g_provgone_t g_zoned_providergone;
static g_start_t g_zoned_start;
static g_taste_t g_zoned_taste;

static struct g_class g_zoned_class = {
	.name = G_ZONED_CLASS_NAME,
	.version = G_VERSION,
	.taste = g_zoned_taste,
	.access = g_zoned_access,
	.dumpconf = g_zoned_dumpconf,
	.orphan = g_zoned_orphan,
	.providergone = g_zoned_providergone,
	.spoiled = g_zoned_orphan,
	.start = g_zoned_start,
};

/*
 * Index of the zone that contains the given LBA. The caller must have
 * range-checked the LBA against sc_maxlba.
 */
static __inline uint32_t
g_zoned_zoneno(struct g_zoned_softc *sc, uint64_t lba)
{
	return ((uint32_t)(lba / sc->sc_zonesecs));
}

/*
 * Repack zone zno into the on-disk table image and extend the dirty range to
 * cover the sector it lives in. Must be called with sc_lock held.
 */
static void
g_zoned_mark_dirty(struct g_zoned_softc *sc, uint32_t zno)
{
	uint32_t sec;

	mtx_assert(&sc->sc_lock, MA_OWNED);

	g_zoned_entry_encode(&sc->sc_zones[zno],
	    sc->sc_tab + sc->sc_secsize + (off_t)zno * G_ZONED_ENTRY_SIZE);
	/* Sector 0 contains the header; entries start at sector 1. */
	sec = 1 +
	    (uint32_t)(((off_t)zno * G_ZONED_ENTRY_SIZE) / sc->sc_secsize);
	if (!sc->sc_dirty) {
		sc->sc_dirty = true;
		sc->sc_dirtylo = sc->sc_dirtyhi = sec;
	} else {
		if (sec < sc->sc_dirtylo)
			sc->sc_dirtylo = sec;
		if (sec > sc->sc_dirtyhi)
			sc->sc_dirtyhi = sec;
	}
}

/*
 * Initialise the on-disk table image from the (totally-empty) live zone array
 * and mark the whole thing dirty so the first BIO_FLUSH writes it out. The call
 * hapepns from from the create path, before the provider goes live, thus no
 * locking is needed.
 */
static void
g_zoned_init_table(struct g_zoned_softc *sc)
{
	uint32_t i;

	bzero(sc->sc_tab, (size_t)sc->sc_tabsecs * sc->sc_secsize);
	bcopy(G_ZONED_TABLE_MAGIC, sc->sc_tab, sizeof(G_ZONED_TABLE_MAGIC) - 1);
	le32enc(sc->sc_tab + 16, G_ZONED_VERSION);
	le32enc(sc->sc_tab + 20, sc->sc_nzones);
	for (i = 0; i < sc->sc_nzones; i++)
		g_zoned_entry_encode(&sc->sc_zones[i],
		    sc->sc_tab + sc->sc_secsize +
			(off_t)i * G_ZONED_ENTRY_SIZE);
	sc->sc_dirty = true;
	sc->sc_dirtylo = 0;
	sc->sc_dirtyhi = sc->sc_tabsecs - 1;
}

/*
 * Read the existing, persistent zone table from the provider into the live
 * array, otherwise initialise an empty one if no valid table is present. Runs
 * in the create path with topology are held.
 */
static void
g_zoned_load_table(struct g_zoned_softc *sc, struct g_consumer *cp)
{
	u_char *buf, *dst;
	off_t off, resid, chunk;
	uint32_t i;
	int error;
	bool valid;

	g_topology_assert();

	error = g_access(cp, 1, 0, 0);
	if (error != 0) {
		G_ZONED_DEBUG(0,
		    "Cannot open %s to read zone table (error=%d);"
		    " assuming empty.",
		    cp->provider->name, error);
		g_zoned_init_table(sc);
		return;
	}
	g_topology_unlock();

	valid = false;
	buf = g_read_data(cp, sc->sc_taboff, sc->sc_secsize, &error);
	if (buf != NULL &&
	    strncmp((char *)buf, G_ZONED_TABLE_MAGIC,
		sizeof(G_ZONED_TABLE_MAGIC) - 1) == 0 &&
	    le32dec(buf + 20) == sc->sc_nzones) {
		bcopy(buf, sc->sc_tab, sc->sc_secsize);
		valid = true;
	}
	if (buf != NULL)
		g_free(buf);

	if (valid) {
		off = sc->sc_taboff + sc->sc_secsize;
		dst = sc->sc_tab + sc->sc_secsize;
		resid = (off_t)(sc->sc_tabsecs - 1) * sc->sc_secsize;
		while (resid > 0) {
			chunk = MIN(resid, (off_t)maxphys);
			chunk -= chunk % sc->sc_secsize;
			if (chunk == 0)
				chunk = sc->sc_secsize;
			buf = g_read_data(cp, off, chunk, &error);
			if (buf == NULL) {
				valid = false;
				break;
			}
			bcopy(buf, dst, chunk);
			g_free(buf);
			off += chunk;
			dst += chunk;
			resid -= chunk;
		}
	}

	g_topology_lock();
	g_access(cp, -1, 0, 0);

	if (!valid) {
		G_ZONED_DEBUG(1, "No valid zone table on %s; initialising.",
		    cp->provider->name);
		g_zoned_init_table(sc);
		return;
	}

	for (i = 0; i < sc->sc_nzones; i++) {
		struct disk_zone_rep_entry *z = &sc->sc_zones[i];

		g_zoned_entry_decode(sc->sc_tab + sc->sc_secsize +
			(off_t)i * G_ZONED_ENTRY_SIZE,
		    z);
		z->zone_start_lba = (uint64_t)i * sc->sc_zonesecs;
		z->zone_length = sc->sc_zonesecs;
		/* Guard against a corrupt write pointer. */
		if (z->write_pointer_lba < z->zone_start_lba ||
		    z->write_pointer_lba > z->zone_start_lba + z->zone_length) {
			z->write_pointer_lba = z->zone_start_lba;
			z->zone_condition = DISK_ZONE_COND_EMPTY;
		}
	}
	G_ZONED_DEBUG(1, "Restored zone table from %s.", cp->provider->name);
}

/*
 * "Flush the dirty table sectors, then forward the cache flush" thing. We
 * allocate one of those per BIO_FLUSH that finds dirty state to commit.
 */
struct g_zoned_flush {
	struct bio *fl_orig;	  /* Original BIO_FLUSH. */
	struct g_consumer *fl_cp; /* Where to send the I/O. */
	u_char *fl_buf;		  /* Snapshot of dirty sectors. */
	off_t fl_off;		  /* Disk offset of first sector. */
	off_t fl_total;		  /* Bytes to write. */
	off_t fl_done;		  /* Bytes written so far. */
	u_int fl_secsize;
};

static void g_zoned_flush_step(struct g_zoned_flush *fc);

static void
g_zoned_flush_final(struct bio *bp)
{
	struct g_zoned_flush *fc = bp->bio_caller1;
	struct bio *orig = fc->fl_orig;
	int error = bp->bio_error;

	g_destroy_bio(bp);
	g_free(fc->fl_buf);
	g_free(fc);
	g_io_deliver(orig, error);
}

static void
g_zoned_flush_write_done(struct bio *bp)
{
	struct g_zoned_flush *fc = bp->bio_caller1;
	int error = bp->bio_error;

	g_destroy_bio(bp);
	if (error != 0) {
		G_ZONED_DEBUG(0, "Zone table write failed (error=%d).", error);
		g_free(fc->fl_buf);
		g_io_deliver(fc->fl_orig, error);
		g_free(fc);
		return;
	}
	g_zoned_flush_step(fc);
}

static void
g_zoned_flush_step(struct g_zoned_flush *fc)
{
	struct bio *cbp;
	off_t chunk;

	if (fc->fl_done < fc->fl_total) {
		chunk = fc->fl_total - fc->fl_done;
		if (chunk > (off_t)maxphys) {
			chunk = maxphys;
			chunk -= chunk % fc->fl_secsize;
		}
		cbp = g_alloc_bio();
		cbp->bio_cmd = BIO_WRITE;
		cbp->bio_offset = fc->fl_off + fc->fl_done;
		cbp->bio_data = fc->fl_buf + fc->fl_done;
		cbp->bio_length = chunk;
		cbp->bio_done = g_zoned_flush_write_done;
		cbp->bio_caller1 = fc;
		fc->fl_done += chunk;
		g_io_request(cbp, fc->fl_cp);
		return;
	}
	/* Table is on its way down; now forward the real cache flush. */
	cbp = g_alloc_bio();
	cbp->bio_cmd = BIO_FLUSH;
	cbp->bio_done = g_zoned_flush_final;
	cbp->bio_caller1 = fc;
	g_io_request(cbp, fc->fl_cp);
}

/*
 * Attempt commiting the dirty zone-table state for BIO_FLUSH. Returns true if
 * it took ownership of bp i.e. an async chain is running, false if the caller
 * should forward the flush normally.
 */
static bool
g_zoned_flush_begin(struct g_zoned_softc *sc, struct g_geom *gp, struct bio *bp)
{
	struct g_zoned_flush *fc;
	struct g_consumer *cp;
	u_char *buf;
	off_t off, total;
	uint32_t lo, hi;

	cp = LIST_FIRST(&gp->consumer);
	/* Without write access persistence is not possible; thus forward the flush. */
	if (cp->acw == 0)
		return (false);

	mtx_lock(&sc->sc_lock);
	if (!sc->sc_dirty) {
		mtx_unlock(&sc->sc_lock);
		return (false);
	}
	lo = sc->sc_dirtylo;
	hi = sc->sc_dirtyhi;
	total = (off_t)(hi - lo + 1) * sc->sc_secsize;
	buf = g_malloc(total, M_NOWAIT);
	if (buf == NULL) {
		/* Leave table dirty; later flush will retry. */
		mtx_unlock(&sc->sc_lock);
		return (false);
	}
	bcopy(sc->sc_tab + (off_t)lo * sc->sc_secsize, buf, total);
	sc->sc_dirty = false;
	off = sc->sc_taboff + (off_t)lo * sc->sc_secsize;
	mtx_unlock(&sc->sc_lock);

	fc = g_malloc(sizeof(*fc), M_NOWAIT);
	if (fc == NULL) {
		g_free(buf);
		/* Reload the dirty range; nothing was written. */
		mtx_lock(&sc->sc_lock);
		if (!sc->sc_dirty) {
			sc->sc_dirty = true;
			sc->sc_dirtylo = lo;
			sc->sc_dirtyhi = hi;
		} else {
			if (lo < sc->sc_dirtylo)
				sc->sc_dirtylo = lo;
			if (hi > sc->sc_dirtyhi)
				sc->sc_dirtyhi = hi;
		}
		mtx_unlock(&sc->sc_lock);
		return (false);
	}
	fc->fl_orig = bp;
	fc->fl_cp = cp;
	fc->fl_buf = buf;
	fc->fl_off = off;
	fc->fl_total = total;
	fc->fl_done = 0;
	fc->fl_secsize = sc->sc_secsize;
	g_zoned_flush_step(fc);
	return (true);
}

/*
 * Emulate BIO_ZONE management commands.
 */
static void
g_zoned_zonecmd(struct bio *bp, struct g_zoned_softc *sc)
{
	struct disk_zone_args *args = &bp->bio_zone;
	uint32_t i, first, limit;

	switch (args->zone_cmd) {
	case DISK_ZONE_GET_PARAMS: {
		struct disk_zone_disk_params *p =
		    &args->zone_params.disk_params;

		p->zone_mode = DISK_ZONE_MODE_HOST_MANAGED;
		/*
		 * Reads are passed straight through, so advertise unrestricted
		 * reads in sequential-write-required zones.
		 */
		p->flags = DISK_ZONE_DISK_URSWRZ | DISK_ZONE_RZ_SUP |
		    DISK_ZONE_OPEN_SUP | DISK_ZONE_CLOSE_SUP |
		    DISK_ZONE_FINISH_SUP | DISK_ZONE_RWP_SUP;
		p->optimal_seq_zones = 0;
		p->optimal_nonseq_zones = 0;
		p->max_seq_zones = 0;
		g_io_deliver(bp, 0);
		return;
	}
	case DISK_ZONE_REPORT_ZONES: {
		struct disk_zone_report *rep = &args->zone_params.report;
		uint32_t filled, zno;

		mtx_lock(&sc->sc_lock);
		sc->sc_zonecmds++;
		rep->header.same = DISK_ZONE_SAME_ALL_SAME;
		rep->header.maximum_lba = sc->sc_maxlba - 1;

		if (rep->starting_id >= sc->sc_maxlba)
			zno = sc->sc_nzones;
		else
			zno = g_zoned_zoneno(sc, rep->starting_id);

		rep->entries_available = sc->sc_nzones - zno;
		filled = rep->entries_available;
		if (filled > rep->entries_allocated)
			filled = rep->entries_allocated;
		if (filled > 0 && rep->entries != NULL)
			bcopy(&sc->sc_zones[zno], rep->entries,
			    (size_t)filled * sizeof(*rep->entries));
		rep->entries_filled = filled;
		mtx_unlock(&sc->sc_lock);
		g_io_deliver(bp, 0);
		return;
	}
	case DISK_ZONE_OPEN:
	case DISK_ZONE_CLOSE:
	case DISK_ZONE_FINISH:
	case DISK_ZONE_RWP: {
		struct disk_zone_rwp *rwp = &args->zone_params.rwp;

		if ((rwp->flags & DISK_ZONE_RWP_FLAG_ALL) != 0) {
			first = 0;
			limit = sc->sc_nzones;
		} else {
			if (rwp->id >= sc->sc_maxlba) {
				g_io_deliver(bp, EINVAL);
				return;
			}
			first = g_zoned_zoneno(sc, rwp->id);
			limit = first + 1;
		}

		mtx_lock(&sc->sc_lock);
		sc->sc_zonecmds++;
		for (i = first; i < limit; i++) {
			struct disk_zone_rep_entry *z = &sc->sc_zones[i];

			switch (args->zone_cmd) {
			case DISK_ZONE_OPEN:
				if (z->zone_condition != DISK_ZONE_COND_FULL)
					z->zone_condition =
					    DISK_ZONE_COND_EXPLICIT_OPEN;
				break;
			case DISK_ZONE_CLOSE:
				if (z->write_pointer_lba == z->zone_start_lba)
					z->zone_condition =
					    DISK_ZONE_COND_EMPTY;
				else if (z->zone_condition !=
				    DISK_ZONE_COND_FULL)
					z->zone_condition =
					    DISK_ZONE_COND_CLOSED;
				break;
			case DISK_ZONE_FINISH:
				z->write_pointer_lba = z->zone_start_lba +
				    z->zone_length;
				z->zone_condition = DISK_ZONE_COND_FULL;
				break;
			case DISK_ZONE_RWP:
				z->write_pointer_lba = z->zone_start_lba;
				z->zone_condition = DISK_ZONE_COND_EMPTY;
				break;
			}
			g_zoned_mark_dirty(sc, i);
		}
		mtx_unlock(&sc->sc_lock);
		g_io_deliver(bp, 0);
		return;
	}
	default:
		G_ZONED_LOGREQ(bp, "Unsupported zone command %u.",
		    args->zone_cmd);
		g_io_deliver(bp, EOPNOTSUPP);
		return;
	}
}

/*
 * Validate and account a write against the zone model, advancing the write
 * pointer. Returns 0 if the write may proceed or an errno for failure.
 */
static int
g_zoned_write_check(struct g_zoned_softc *sc, struct bio *bp)
{
	struct disk_zone_rep_entry *z;
	uint64_t lba, end;
	uint32_t zno;

	mtx_assert(&sc->sc_lock, MA_OWNED);

	lba = bp->bio_offset / sc->sc_secsize;
	end = (bp->bio_offset + bp->bio_length) / sc->sc_secsize;
	if (end > sc->sc_maxlba)
		return (EIO);

	zno = g_zoned_zoneno(sc, lba);
	z = &sc->sc_zones[zno];

	/* No request shall span more than one zone. */
	if (end > z->zone_start_lba + z->zone_length) {
		G_ZONED_LOGREQ(bp, "Write crosses a zone boundary.");
		return (EIO);
	}

	/* Sequential-write-required zones only accept writes at the WP. */
	if (z->zone_condition == DISK_ZONE_COND_FULL ||
	    lba != z->write_pointer_lba) {
		G_ZONED_LOGREQ(bp,
		    "Out-of-order write to zone %u (lba %ju, wp %ju).", zno,
		    (uintmax_t)lba, (uintmax_t)z->write_pointer_lba);
		return (EIO);
	}

	z->write_pointer_lba = end;
	if (z->write_pointer_lba >= z->zone_start_lba + z->zone_length)
		z->zone_condition = DISK_ZONE_COND_FULL;
	else
		z->zone_condition = DISK_ZONE_COND_IMPLICIT_OPEN;
	g_zoned_mark_dirty(sc, zno);

	sc->sc_writes++;
	sc->sc_wrotebytes += bp->bio_length;
	return (0);
}

static void
g_zoned_start(struct bio *bp)
{
	struct g_zoned_softc *sc;
	struct g_geom *gp;
	struct bio *cbp;
	int error;

	gp = bp->bio_to->geom;
	sc = gp->softc;
	G_ZONED_LOGREQ(bp, "Request received.");

	switch (bp->bio_cmd) {
	case BIO_ZONE:
		g_zoned_zonecmd(bp, sc);
		return;
	case BIO_WRITE:
		mtx_lock(&sc->sc_lock);
		error = g_zoned_write_check(sc, bp);
		mtx_unlock(&sc->sc_lock);
		if (error != 0) {
			g_io_deliver(bp, error);
			return;
		}
		break;
	case BIO_READ:
		mtx_lock(&sc->sc_lock);
		sc->sc_reads++;
		sc->sc_readbytes += bp->bio_length;
		mtx_unlock(&sc->sc_lock);
		break;
	case BIO_FLUSH:
		if (g_zoned_flush_begin(sc, gp, bp))
			return;
		break;
	default:
		break;
	}

	cbp = g_clone_bio(bp);
	if (cbp == NULL) {
		g_io_deliver(bp, ENOMEM);
		return;
	}
	cbp->bio_done = g_std_done;
	G_ZONED_LOGREQ(cbp, "Sending request.");
	g_io_request(cbp, LIST_FIRST(&gp->consumer));
}

static int
g_zoned_access(struct g_provider *pp, int dr, int dw, int de)
{
	struct g_geom *gp;
	struct g_consumer *cp;

	gp = pp->geom;
	cp = LIST_FIRST(&gp->consumer);
	return (g_access(cp, dr, dw, de));
}

static struct g_geom *
g_zoned_create(struct g_class *mp, const struct g_zoned_metadata *md,
    struct g_provider *pp)
{
	struct g_zoned_softc *sc;
	struct g_geom *gp;
	struct g_provider *newpp;
	struct g_consumer *cp;
	uint64_t i, nzones, zonesecs;
	int error;

	g_topology_assert();

	nzones = md->md_nzones;
	zonesecs = md->md_zonesize / pp->sectorsize;
	if (nzones == 0 || zonesecs == 0) {
		G_ZONED_DEBUG(0, "Bogus metadata on %s.", pp->name);
		return (NULL);
	}

	LIST_FOREACH(gp, &mp->geom, geom) {
		if (strcmp(gp->name, md->md_name) == 0) {
			G_ZONED_DEBUG(0, "Device %s already exists.",
			    md->md_name);
			return (NULL);
		}
	}

	gp = g_new_geom(mp, md->md_name);
	sc = g_malloc(sizeof(*sc), M_WAITOK | M_ZERO);
	strlcpy(sc->sc_name, md->md_name, sizeof(sc->sc_name));

	sc->sc_id = md->md_id;
	sc->sc_zonesize = md->md_zonesize;
	sc->sc_secsize = pp->sectorsize;
	sc->sc_zonesecs = zonesecs;
	sc->sc_nzones = (uint32_t)nzones;
	sc->sc_maxlba = nzones * zonesecs;
	sc->sc_zones = malloc(nzones * sizeof(*sc->sc_zones), M_ZONED,
	    M_WAITOK | M_ZERO);
	sc->sc_tabsecs = 1 +
	    howmany(nzones * G_ZONED_ENTRY_SIZE, pp->sectorsize);
	sc->sc_taboff = (pp->mediasize - pp->sectorsize) -
	    (off_t)sc->sc_tabsecs * pp->sectorsize;
	sc->sc_tab = malloc((size_t)sc->sc_tabsecs * pp->sectorsize, M_ZONED,
	    M_WAITOK | M_ZERO);
	for (i = 0; i < nzones; i++) {
		sc->sc_zones[i].zone_type = DISK_ZONE_TYPE_SEQ_REQUIRED;
		sc->sc_zones[i].zone_condition = DISK_ZONE_COND_EMPTY;
		sc->sc_zones[i].zone_length = zonesecs;
		sc->sc_zones[i].zone_start_lba = i * zonesecs;
		sc->sc_zones[i].write_pointer_lba = i * zonesecs;
	}
	mtx_init(&sc->sc_lock, "gzoned lock", NULL, MTX_DEF);
	gp->softc = sc;

	newpp = g_new_providerf(gp, "%s", gp->name);
	newpp->flags |= G_PF_DIRECT_SEND | G_PF_DIRECT_RECEIVE;
	newpp->mediasize = (off_t)nzones * md->md_zonesize;
	newpp->sectorsize = pp->sectorsize;
	newpp->stripesize = pp->stripesize;
	newpp->stripeoffset = pp->stripeoffset;

	cp = g_new_consumer(gp);
	cp->flags |= G_CF_DIRECT_SEND | G_CF_DIRECT_RECEIVE;
	error = g_attach(cp, pp);
	if (error != 0) {
		G_ZONED_DEBUG(0, "Cannot attach to provider %s.", pp->name);
		goto fail;
	}

	/* Restore the persistent zone state or initialise an empty table. */
	g_zoned_load_table(sc, cp);

	newpp->flags |= pp->flags & G_PF_ACCEPT_UNMAPPED;
	g_error_provider(newpp, 0);
	G_ZONED_DEBUG(0, "Device %s created (%u zones of %jd bytes).", gp->name,
	    sc->sc_nzones, (intmax_t)sc->sc_zonesize);
	return (gp);
fail:
	if (cp->provider != NULL)
		g_detach(cp);
	g_destroy_consumer(cp);
	g_destroy_provider(newpp);
	mtx_destroy(&sc->sc_lock);
	free(sc->sc_tab, M_ZONED);
	free(sc->sc_zones, M_ZONED);
	g_free(sc);
	g_destroy_geom(gp);
	return (NULL);
}

static int
g_zoned_read_metadata(struct g_consumer *cp, struct g_zoned_metadata *md)
{
	struct g_provider *pp;
	u_char *buf;
	int error;

	g_topology_assert();

	error = g_access(cp, 1, 0, 0);
	if (error != 0)
		return (error);
	pp = cp->provider;
	g_topology_unlock();
	buf = g_read_data(cp, pp->mediasize - pp->sectorsize, pp->sectorsize,
	    &error);
	g_topology_lock();
	g_access(cp, -1, 0, 0);
	if (buf == NULL)
		return (error);
	zoned_metadata_decode(buf, md);
	g_free(buf);
	return (0);
}

static struct g_geom *
g_zoned_taste(struct g_class *mp, struct g_provider *pp, int flags __unused)
{
	struct g_zoned_metadata md;
	struct g_zoned_softc *sc;
	struct g_consumer *cp;
	struct g_geom *gp;
	int error;

	g_topology_assert();

	/* Skip providers that are already open for writing. */
	if (pp->acw > 0)
		return (NULL);

	G_ZONED_DEBUG(3, "Tasting %s.", pp->name);

	gp = g_new_geom(mp, "zoned:taste");
	cp = g_new_consumer(gp);
	cp->flags |= G_CF_DIRECT_SEND | G_CF_DIRECT_RECEIVE;
	error = g_attach(cp, pp);
	if (error == 0) {
		error = g_zoned_read_metadata(cp, &md);
		g_detach(cp);
	}
	g_destroy_consumer(cp);
	g_destroy_geom(gp);
	if (error != 0)
		return (NULL);

	if (strcmp(md.md_magic, G_ZONED_MAGIC) != 0)
		return (NULL);
	if (md.md_version > G_ZONED_VERSION) {
		printf("geom_zoned.ko module is too old to handle %s.\n",
		    pp->name);
		return (NULL);
	}
	if (md.md_provsize != (uint64_t)pp->mediasize)
		return (NULL);
	if (md.md_sectorsize != pp->sectorsize)
		return (NULL);
	if (md.md_nzones == 0 || md.md_zonesize == 0)
		return (NULL);

	/* Already running? */
	LIST_FOREACH(gp, &mp->geom, geom) {
		sc = gp->softc;
		if (sc == NULL)
			continue;
		if (strcmp(sc->sc_name, md.md_name) == 0 &&
		    sc->sc_id == md.md_id)
			return (NULL);
	}

	gp = g_zoned_create(mp, &md, pp);
	if (gp == NULL)
		G_ZONED_DEBUG(0, "Cannot create device %s.", md.md_name);
	return (gp);
}

static int
g_zoned_destroy(struct g_geom *gp, boolean_t force)
{
	struct g_zoned_softc *sc;
	struct g_provider *pp;

	g_topology_assert();
	sc = gp->softc;
	if (sc == NULL)
		return (ENXIO);
	pp = LIST_FIRST(&gp->provider);
	if (pp != NULL && (pp->acr != 0 || pp->acw != 0 || pp->ace != 0)) {
		if (force) {
			G_ZONED_DEBUG(0,
			    "Device %s is still open, so it "
			    "can't be definitely removed.",
			    pp->name);
		} else {
			G_ZONED_DEBUG(1, "Device %s is still open (r%dw%de%d).",
			    pp->name, pp->acr, pp->acw, pp->ace);
			return (EBUSY);
		}
	} else {
		G_ZONED_DEBUG(0, "Device %s removed.", gp->name);
	}

	g_wither_geom(gp, ENXIO);
	return (0);
}

static void
g_zoned_orphan(struct g_consumer *cp)
{
	g_topology_assert();
	g_zoned_destroy(cp->geom, 1);
}

static void
g_zoned_providergone(struct g_provider *pp)
{
	struct g_geom *gp = pp->geom;
	struct g_zoned_softc *sc = gp->softc;

	gp->softc = NULL;
	free(sc->sc_tab, M_ZONED);
	free(sc->sc_zones, M_ZONED);
	mtx_destroy(&sc->sc_lock);
	g_free(sc);
}

static void
g_zoned_dumpconf(struct sbuf *sb, const char *indent, struct g_geom *gp,
    struct g_consumer *cp, struct g_provider *pp)
{
	struct g_zoned_softc *sc;

	if (pp != NULL || cp != NULL)
		return;
	sc = gp->softc;
	sbuf_printf(sb, "%s<ZoneSize>%jd</ZoneSize>\n", indent,
	    (intmax_t)sc->sc_zonesize);
	sbuf_printf(sb, "%s<Zones>%u</Zones>\n", indent, sc->sc_nzones);
	sbuf_printf(sb, "%s<Mode>Host Managed</Mode>\n", indent);
	sbuf_printf(sb, "%s<Reads>%ju</Reads>\n", indent, sc->sc_reads);
	sbuf_printf(sb, "%s<Writes>%ju</Writes>\n", indent, sc->sc_writes);
	sbuf_printf(sb, "%s<ReadBytes>%ju</ReadBytes>\n", indent,
	    sc->sc_readbytes);
	sbuf_printf(sb, "%s<WroteBytes>%ju</WroteBytes>\n", indent,
	    sc->sc_wrotebytes);
	sbuf_printf(sb, "%s<ZoneCommands>%ju</ZoneCommands>\n", indent,
	    sc->sc_zonecmds);
}

DECLARE_GEOM_CLASS(g_zoned_class, g_zoned);
MODULE_VERSION(geom_zoned, 0);
