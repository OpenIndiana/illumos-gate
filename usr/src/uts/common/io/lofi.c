/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2004 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * lofi (loopback file) driver - allows you to attach a file to a device,
 * which can then be accessed through that device. The simple model is that
 * you tell lofi to open a file, and then use the block device you get as
 * you would any block device. lofi translates access to the block device
 * into I/O on the underlying file. This is mostly useful for
 * mounting images of filesystems.
 *
 * lofi is controlled through /dev/lofictl - this is the only device exported
 * during attach, and is minor number 0. lofiadm communicates with lofi through
 * ioctls on this device. When a file is attached to lofi, block and character
 * devices are exported in /dev/lofi and /dev/rlofi. Currently, these devices
 * are identified by their minor number, and the minor number is also used
 * as the name in /dev/lofi. If we ever decide to support virtual disks,
 * we'll have to divide the minor number space to identify fdisk partitions
 * and slices, and the name will then be the minor number shifted down a
 * few bits. Minor devices are tracked with state structures handled with
 * ddi_soft_state(9F) for simplicity.
 *
 * A file attached to lofi is opened when attached and not closed until
 * explicitly detached from lofi. This seems more sensible than deferring
 * the open until the /dev/lofi device is opened, for a number of reasons.
 * One is that any failure is likely to be noticed by the person (or script)
 * running lofiadm. Another is that it would be a security problem if the
 * file was replaced by another one after being added but before being opened.
 *
 * The only hard part about lofi is the ioctls. In order to support things
 * like 'newfs' on a lofi device, it needs to support certain disk ioctls.
 * So it has to fake disk geometry and partition information. More may need
 * to be faked if your favorite utility doesn't work and you think it should
 * (fdformat doesn't work because it really wants to know the type of floppy
 * controller to talk to, and that didn't seem easy to fake. Or possibly even
 * necessary, since we have mkfs_pcfs now).
 *
 * Known problems:
 *
 *	UFS logging. Mounting a UFS filesystem image "logging"
 *	works for basic copy testing but wedges during a build of ON through
 *	that image. Some deadlock in lufs holding the log mutex and then
 *	getting stuck on a buf. So for now, don't do that.
 *
 *	Direct I/O. Since the filesystem data is being cached in the buffer
 *	cache, _and_ again in the underlying filesystem, it's tempting to
 *	enable direct I/O on the underlying file. Don't, because that deadlocks.
 *	I think to fix the cache-twice problem we might need filesystem support.
 *
 *	lofi on itself. The simple lock strategy (lofi_lock) precludes this
 *	because you'll be in lofi_ioctl, holding the lock when you open the
 *	file, which, if it's lofi, will grab lofi_lock. We prevent this for
 *	now, though not using ddi_soft_state(9F) would make it possible to
 *	do. Though it would still be silly.
 *
 * Interesting things to do:
 *
 *	Allow multiple files for each device. A poor-man's metadisk, basically.
 *
 *	Pass-through ioctls on block devices. You can (though it's not
 *	documented), give lofi a block device as a file name. Then we shouldn't
 *	need to fake a geometry. But this is also silly unless you're replacing
 *	metadisk.
 *
 *	Encryption. tpm would like this. Apparently Windows 2000 has it, and
 *	so does Linux.
 */

#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/uio.h>
#include <sys/kmem.h>
#include <sys/cred.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include <sys/aio_req.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/modctl.h>
#include <sys/conf.h>
#include <sys/debug.h>
#include <sys/vnode.h>
#include <sys/lofi.h>
#include <sys/vol.h>
#include <sys/fcntl.h>
#include <sys/pathname.h>
#include <sys/filio.h>
#include <sys/fdio.h>
#include <sys/open.h>
#include <sys/disp.h>
#include <vm/seg_map.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>

/* seems safer than having to get the string right many times */
#define	NBLOCKS_PROP_NAME	"Nblocks"
#define	SIZE_PROP_NAME	"Size"

static dev_info_t *lofi_dip;
static void	*lofi_statep;
static kmutex_t lofi_lock;		/* state lock */

/*
 * Because lofi_taskq_nthreads limits the actual swamping of the device, the
 * maxalloc parameter (lofi_taskq_maxalloc) should be tuned conservatively
 * high.  If we want to be assured that the underlying device is always busy,
 * we must be sure that the number of bytes enqueued when the number of
 * enqueued tasks exceeds maxalloc is sufficient to keep the device busy for
 * the duration of the sleep time in taskq_ent_alloc().  That is, lofi should
 * set maxalloc to be the maximum throughput (in bytes per second) of the
 * underlying device divided by the minimum I/O size.  We assume a realistic
 * maximum throughput of one hundred megabytes per second; we set maxalloc on
 * the lofi task queue to be 104857600 divided by DEV_BSIZE.
 */
static int lofi_taskq_maxalloc = 104857600 / DEV_BSIZE;
static int lofi_taskq_nthreads = 4;	/* # of taskq threads per device */

uint32_t lofi_max_files = LOFI_MAX_FILES;

static int
lofi_busy(void)
{
	minor_t	minor;

	/*
	 * We need to make sure no mappings exist - mod_remove won't
	 * help because the device isn't open.
	 */
	mutex_enter(&lofi_lock);
	for (minor = 1; minor <= lofi_max_files; minor++) {
		if (ddi_get_soft_state(lofi_statep, minor) != NULL) {
			mutex_exit(&lofi_lock);
			return (EBUSY);
		}
	}
	mutex_exit(&lofi_lock);
	return (0);
}

static int
is_opened(struct lofi_state *lsp)
{
	ASSERT(mutex_owned(&lofi_lock));
	return (lsp->ls_chr_open || lsp->ls_blk_open || lsp->ls_lyr_open_count);
}

static int
mark_opened(struct lofi_state *lsp, int otyp)
{
	ASSERT(mutex_owned(&lofi_lock));
	switch (otyp) {
	case OTYP_CHR:
		lsp->ls_chr_open = 1;
		break;
	case OTYP_BLK:
		lsp->ls_blk_open = 1;
		break;
	case OTYP_LYR:
		lsp->ls_lyr_open_count++;
		break;
	default:
		return (-1);
	}
	return (0);
}

static void
mark_closed(struct lofi_state *lsp, int otyp)
{
	ASSERT(mutex_owned(&lofi_lock));
	switch (otyp) {
	case OTYP_CHR:
		lsp->ls_chr_open = 0;
		break;
	case OTYP_BLK:
		lsp->ls_blk_open = 0;
		break;
	case OTYP_LYR:
		lsp->ls_lyr_open_count--;
		break;
	default:
		break;
	}
}

/*ARGSUSED3*/
static int
lofi_open(dev_t *devp, int flag, int otyp, struct cred *credp)
{
	minor_t	minor;
	struct lofi_state *lsp;

	mutex_enter(&lofi_lock);
	minor = getminor(*devp);
	if (minor == 0) {
		/* master control device */
		/* must be opened exclusively */
		if (((flag & FEXCL) != FEXCL) || (otyp != OTYP_CHR)) {
			mutex_exit(&lofi_lock);
			return (EINVAL);
		}
		lsp = ddi_get_soft_state(lofi_statep, 0);
		if (lsp == NULL) {
			mutex_exit(&lofi_lock);
			return (ENXIO);
		}
		if (is_opened(lsp)) {
			mutex_exit(&lofi_lock);
			return (EBUSY);
		}
		(void) mark_opened(lsp, OTYP_CHR);
		mutex_exit(&lofi_lock);
		return (0);
	}

	/* otherwise, the mapping should already exist */
	lsp = ddi_get_soft_state(lofi_statep, minor);
	if (lsp == NULL) {
		mutex_exit(&lofi_lock);
		return (EINVAL);
	}

	if (mark_opened(lsp, otyp) == -1) {
		mutex_exit(&lofi_lock);
		return (EINVAL);
	}

	mutex_exit(&lofi_lock);
	return (0);
}

/*ARGSUSED3*/
static int
lofi_close(dev_t dev, int flag, int otyp, struct cred *credp)
{
	minor_t	minor;
	struct lofi_state *lsp;

#ifdef lint
	flag = flag;
#endif
	mutex_enter(&lofi_lock);
	minor = getminor(dev);
	lsp = ddi_get_soft_state(lofi_statep, minor);
	if (lsp == NULL) {
		mutex_exit(&lofi_lock);
		return (EINVAL);
	}
	mark_closed(lsp, otyp);
	mutex_exit(&lofi_lock);
	return (0);
}

/*
 * This is basically what strategy used to be before we found we
 * needed task queues.
 */
static void
lofi_strategy_task(void *arg)
{
	struct buf *bp = (struct buf *)arg;
	int error;
	struct lofi_state *lsp;
	offset_t	offset, alignedoffset;
	offset_t	mapoffset;
	caddr_t	bufaddr;
	caddr_t	mapaddr;
	size_t	xfersize;
	size_t	len;
	int	isread;
	int	smflags;
	enum seg_rw srw;

	lsp = ddi_get_soft_state(lofi_statep, getminor(bp->b_edev));
	if (lsp->ls_kstat) {
		mutex_enter(lsp->ls_kstat->ks_lock);
		kstat_waitq_to_runq(KSTAT_IO_PTR(lsp->ls_kstat));
		mutex_exit(lsp->ls_kstat->ks_lock);
	}
	bp_mapin(bp);
	bufaddr = bp->b_un.b_addr;
	offset = bp->b_lblkno * DEV_BSIZE;	/* offset within file */

	/*
	 * We used to always use vn_rdwr here, but we cannot do that because
	 * we might decide to read or write from the the underlying
	 * file during this call, which would be a deadlock because
	 * we have the rw_lock. So instead we page, unless it's not
	 * mapable or it's a character device.
	 */
	if (((lsp->ls_vp->v_flag & VNOMAP) == 0) &&
	    (lsp->ls_vp->v_type != VCHR)) {
		/*
		 * segmap always gives us an 8K (MAXBSIZE) chunk, aligned on
		 * an 8K boundary, but the buf transfer address may not be
		 * aligned on more than a 512-byte boundary (we don't
		 * enforce that, though we could). This matters since the
		 * initial part of the transfer may not start at offset 0
		 * within the segmap'd chunk. So we have to compensate for
		 * that with 'mapoffset'. Subsequent chunks always start
		 * off at the beginning, and the last is capped by b_resid.
		 */
		mapoffset = offset & MAXBOFFSET;
		alignedoffset = offset - mapoffset;	/* now map-aligned */
		bp->b_resid = bp->b_bcount;
		isread = bp->b_flags & B_READ;
		srw = isread ? S_READ : S_WRITE;
		do {
			xfersize = MIN(lsp->ls_vp_size - offset,
			    MIN(MAXBSIZE - mapoffset, bp->b_resid));
			len = roundup(mapoffset + xfersize, PAGESIZE);
			mapaddr = segmap_getmapflt(segkmap, lsp->ls_vp,
			    alignedoffset, MAXBSIZE, 1, srw);
			/*
			 * Now fault in the pages. This lets us check
			 * for errors before we reference mapaddr and
			 * try to resolve the fault in bcopy (which would
			 * panic instead). And this can easily happen,
			 * particularly if you've lofi'd a file over NFS
			 * and someone deletes the file on the server.
			 */
			error = segmap_fault(kas.a_hat, segkmap, mapaddr,
			    len, F_SOFTLOCK, srw);
			if (error) {
				(void) segmap_release(segkmap, mapaddr, 0);
				if (FC_CODE(error) == FC_OBJERR)
					error = FC_ERRNO(error);
				else
					error = EIO;
				break;
			}
			smflags = 0;
			if (isread) {
				bcopy(mapaddr + mapoffset, bufaddr, xfersize);
			} else {
				smflags |= SM_WRITE;
				bcopy(bufaddr, mapaddr + mapoffset, xfersize);
			}
			bp->b_resid -= xfersize;
			bufaddr += xfersize;
			offset += xfersize;
			(void) segmap_fault(kas.a_hat, segkmap, mapaddr,
			    len, F_SOFTUNLOCK, srw);
			error = segmap_release(segkmap, mapaddr, smflags);
			/* only the first map may start partial */
			mapoffset = 0;
			alignedoffset += MAXBSIZE;
		} while ((error == 0) && (bp->b_resid > 0) &&
		    (offset < lsp->ls_vp_size));
	} else {
		ssize_t	resid;
		enum uio_rw rw;

		if (bp->b_flags & B_READ)
			rw = UIO_READ;
		else
			rw = UIO_WRITE;
		error = vn_rdwr(rw, lsp->ls_vp, bufaddr, bp->b_bcount,
		    offset, UIO_SYSSPACE, 0, RLIM64_INFINITY, kcred, &resid);
		bp->b_resid = resid;
	}

	if (lsp->ls_kstat) {
		size_t n_done = bp->b_bcount - bp->b_resid;
		kstat_io_t *kioptr;

		mutex_enter(lsp->ls_kstat->ks_lock);
		kioptr = KSTAT_IO_PTR(lsp->ls_kstat);
		if (bp->b_flags & B_READ) {
			kioptr->nread += n_done;
			kioptr->reads++;
		} else {
			kioptr->nwritten += n_done;
			kioptr->writes++;
		}
		kstat_runq_exit(kioptr);
		mutex_exit(lsp->ls_kstat->ks_lock);
	}
	bioerror(bp, error);
	biodone(bp);
}

static int
lofi_strategy(struct buf *bp)
{
	struct lofi_state *lsp;
	offset_t	offset;

	/*
	 * We cannot just do I/O here, because the current thread
	 * _might_ end up back in here because the underlying filesystem
	 * wants a buffer, which eventually gets into bio_recycle and
	 * might call into lofi to write out a delayed-write buffer.
	 * This is bad if the filesystem above lofi is the same as below.
	 *
	 * We could come up with a complex strategy using threads to
	 * do the I/O asynchronously, or we could use task queues. task
	 * queues were incredibly easy so they win.
	 */
	lsp = ddi_get_soft_state(lofi_statep, getminor(bp->b_edev));
	offset = bp->b_lblkno * DEV_BSIZE;	/* offset within file */
	if (offset == lsp->ls_vp_size) {
		/* EOF */
		if ((bp->b_flags & B_READ) != 0) {
			bp->b_resid = bp->b_bcount;
			bioerror(bp, 0);
		} else {
			/* writes should fail */
			bioerror(bp, ENXIO);
		}
		biodone(bp);
		return (0);
	}
	if (offset > lsp->ls_vp_size) {
		bioerror(bp, ENXIO);
		biodone(bp);
		return (0);
	}
	if (lsp->ls_kstat) {
		mutex_enter(lsp->ls_kstat->ks_lock);
		kstat_waitq_enter(KSTAT_IO_PTR(lsp->ls_kstat));
		mutex_exit(lsp->ls_kstat->ks_lock);
	}
	(void) taskq_dispatch(lsp->ls_taskq, lofi_strategy_task, bp, KM_SLEEP);
	return (0);
}

/*ARGSUSED2*/
static int
lofi_read(dev_t dev, struct uio *uio, struct cred *credp)
{
	if (getminor(dev) == 0)
		return (EINVAL);
	return (physio(lofi_strategy, NULL, dev, B_READ, minphys, uio));
}

/*ARGSUSED2*/
static int
lofi_write(dev_t dev, struct uio *uio, struct cred *credp)
{
	if (getminor(dev) == 0)
		return (EINVAL);
	return (physio(lofi_strategy, NULL, dev, B_WRITE, minphys, uio));
}

/*ARGSUSED2*/
static int
lofi_aread(dev_t dev, struct aio_req *aio, struct cred *credp)
{
	if (getminor(dev) == 0)
		return (EINVAL);
	return (aphysio(lofi_strategy, anocancel, dev, B_READ, minphys, aio));
}

/*ARGSUSED2*/
static int
lofi_awrite(dev_t dev, struct aio_req *aio, struct cred *credp)
{
	if (getminor(dev) == 0)
		return (EINVAL);
	return (aphysio(lofi_strategy, anocancel, dev, B_WRITE, minphys, aio));
}

/*ARGSUSED*/
static int
lofi_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = lofi_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = 0;
		return (DDI_SUCCESS);
	}
	return (DDI_FAILURE);
}

static int
lofi_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int	error;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);
	error = ddi_soft_state_zalloc(lofi_statep, 0);
	if (error == DDI_FAILURE) {
		return (DDI_FAILURE);
	}
	error = ddi_create_minor_node(dip, LOFI_CTL_NODE, S_IFCHR, 0,
	    DDI_PSEUDO, NULL);
	if (error == DDI_FAILURE) {
		ddi_soft_state_free(lofi_statep, 0);
		return (DDI_FAILURE);
	}
	lofi_dip = dip;
	ddi_report_dev(dip);
	return (DDI_SUCCESS);
}

static int
lofi_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);
	if (lofi_busy())
		return (DDI_FAILURE);
	lofi_dip = NULL;
	ddi_remove_minor_node(dip, NULL);
	ddi_soft_state_free(lofi_statep, 0);
	return (DDI_SUCCESS);
}

/*
 * These two just simplify the rest of the ioctls that need to copyin/out
 * the lofi_ioctl structure.
 */
struct lofi_ioctl *
copy_in_lofi_ioctl(const struct lofi_ioctl *ulip)
{
	struct lofi_ioctl *klip;
	int	error;

	klip = kmem_alloc(sizeof (struct lofi_ioctl), KM_SLEEP);
	error = copyin(ulip, klip, sizeof (struct lofi_ioctl));
	if (error) {
		kmem_free(klip, sizeof (struct lofi_ioctl));
		return (NULL);
	}

	/* make sure filename is always null-terminated */
	klip->li_filename[MAXPATHLEN] = '\0';

	/* validate minor number */
	if (klip->li_minor > lofi_max_files) {
		kmem_free(klip, sizeof (struct lofi_ioctl));
		return (NULL);
	}
	return (klip);
}

int
copy_out_lofi_ioctl(const struct lofi_ioctl *klip, struct lofi_ioctl *ulip)
{
	int	error;

	error = copyout(klip, ulip, sizeof (struct lofi_ioctl));
	if (error)
		return (EFAULT);
	return (0);
}

void
free_lofi_ioctl(struct lofi_ioctl *klip)
{
	kmem_free(klip, sizeof (struct lofi_ioctl));
}

/*
 * Return the minor number 'filename' is mapped to, if it is.
 */
static int
file_to_minor(char *filename)
{
	minor_t	minor;
	struct lofi_state *lsp;

	ASSERT(mutex_owned(&lofi_lock));
	for (minor = 1; minor <= lofi_max_files; minor++) {
		lsp = ddi_get_soft_state(lofi_statep, minor);
		if (lsp == NULL)
			continue;
		if (strcmp(lsp->ls_filename, filename) == 0)
			return (minor);
	}
	return (0);
}

/*
 * lofiadm does some validation, but since Joe Random (or crashme) could
 * do our ioctls, we need to do some validation too.
 */
static int
valid_filename(const char *filename)
{
	static char *blkprefix = "/dev/" LOFI_BLOCK_NAME "/";
	static char *charprefix = "/dev/" LOFI_CHAR_NAME "/";

	/* must be absolute path */
	if (filename[0] != '/')
		return (0);
	/* must not be lofi */
	if (strncmp(filename, blkprefix, strlen(blkprefix)) == 0)
		return (0);
	if (strncmp(filename, charprefix, strlen(charprefix)) == 0)
		return (0);
	return (1);
}

/*
 * Fakes up a disk geometry, and one big partition, based on the size
 * of the file. This is needed because we allow newfs'ing the device,
 * and newfs will do several disk ioctls to figure out the geometry and
 * partition information. It uses that information to determine the parameters
 * to pass to mkfs. Geometry is pretty much irrelevent these days, but we
 * have to support it.
 */
static void
fake_disk_geometry(struct lofi_state *lsp)
{
	/* dk_geom - see dkio(7I) */
	/*
	 * dkg_ncyl _could_ be set to one here (one big cylinder with gobs
	 * of sectors), but that breaks programs like fdisk which want to
	 * partition a disk by cylinder. With one cylinder, you can't create
	 * an fdisk partition and put pcfs on it for testing (hard to pick
	 * a number between one and one).
	 *
	 * The cheezy floppy test is an attempt to not have too few cylinders
	 * for a small file, or so many on a big file that you waste space
	 * for backup superblocks or cylinder group structures.
	 */
	if (lsp->ls_vp_size < (2 * 1024 * 1024)) /* floppy? */
		lsp->ls_dkg.dkg_ncyl = lsp->ls_vp_size / (100 * 1024);
	else
		lsp->ls_dkg.dkg_ncyl = lsp->ls_vp_size / (300 * 1024);
	/* in case file file is < 100k */
	if (lsp->ls_dkg.dkg_ncyl == 0)
		lsp->ls_dkg.dkg_ncyl = 1;
	lsp->ls_dkg.dkg_acyl = 0;
	lsp->ls_dkg.dkg_bcyl = 0;
	lsp->ls_dkg.dkg_nhead = 1;
	lsp->ls_dkg.dkg_obs1 = 0;
	lsp->ls_dkg.dkg_intrlv = 0;
	lsp->ls_dkg.dkg_obs2 = 0;
	lsp->ls_dkg.dkg_obs3 = 0;
	lsp->ls_dkg.dkg_apc = 0;
	lsp->ls_dkg.dkg_rpm = 7200;
	lsp->ls_dkg.dkg_pcyl = lsp->ls_dkg.dkg_ncyl + lsp->ls_dkg.dkg_acyl;
	lsp->ls_dkg.dkg_nsect = lsp->ls_vp_size /
	    (DEV_BSIZE * lsp->ls_dkg.dkg_ncyl);
	lsp->ls_dkg.dkg_write_reinstruct = 0;
	lsp->ls_dkg.dkg_read_reinstruct = 0;

	/* vtoc - see dkio(7I) */
	bzero(&lsp->ls_vtoc, sizeof (struct vtoc));
	lsp->ls_vtoc.v_sanity = VTOC_SANE;
	lsp->ls_vtoc.v_version = V_VERSION;
	bcopy(LOFI_DRIVER_NAME, lsp->ls_vtoc.v_volume, 7);
	lsp->ls_vtoc.v_sectorsz = DEV_BSIZE;
	lsp->ls_vtoc.v_nparts = 1;
	lsp->ls_vtoc.v_part[0].p_tag = V_UNASSIGNED;
	lsp->ls_vtoc.v_part[0].p_flag = V_UNMNT;
	lsp->ls_vtoc.v_part[0].p_start = (daddr_t)0;
	/*
	 * The partition size cannot just be the number of sectors, because
	 * that might not end on a cylinder boundary. And if that's the case,
	 * newfs/mkfs will print a scary warning. So just figure the size
	 * based on the number of cylinders and sectors/cylinder.
	 */
	lsp->ls_vtoc.v_part[0].p_size = lsp->ls_dkg.dkg_pcyl *
	    lsp->ls_dkg.dkg_nsect * lsp->ls_dkg.dkg_nhead;

	/* dk_cinfo - see dkio(7I) */
	bzero(&lsp->ls_ci, sizeof (struct dk_cinfo));
	(void) strcpy(lsp->ls_ci.dki_cname, LOFI_DRIVER_NAME);
	lsp->ls_ci.dki_ctype = DKC_MD;
	lsp->ls_ci.dki_flags = 0;
	lsp->ls_ci.dki_cnum = 0;
	lsp->ls_ci.dki_addr = 0;
	lsp->ls_ci.dki_space = 0;
	lsp->ls_ci.dki_prio = 0;
	lsp->ls_ci.dki_vec = 0;
	(void) strcpy(lsp->ls_ci.dki_dname, LOFI_DRIVER_NAME);
	lsp->ls_ci.dki_unit = 0;
	lsp->ls_ci.dki_slave = 0;
	lsp->ls_ci.dki_partition = 0;
	/*
	 * newfs uses this to set maxcontig. Must not be < 16, or it
	 * will be 0 when newfs multiplies it by DEV_BSIZE and divides
	 * it by the block size. Then tunefs doesn't work because
	 * maxcontig is 0.
	 */
	lsp->ls_ci.dki_maxtransfer = 16;
}

/*
 * map a file to a minor number. Return the minor number.
 */
static int
lofi_map_file(dev_t dev, struct lofi_ioctl *ulip, int pickminor,
    int *rvalp, struct cred *credp)
{
	minor_t	newminor;
	struct lofi_state *lsp;
	struct lofi_ioctl *klip;
	int	error;
	char	namebuf[50];
	struct vnode *vp;
	int64_t	Nblocks_prop_val;
	int64_t	Size_prop_val;
	vattr_t	vattr;
	int	flag;
	enum vtype v_type;
	dev_t	newdev;
	int zalloced = 0;

	klip = copy_in_lofi_ioctl(ulip);
	if (klip == NULL)
		return (EFAULT);

	mutex_enter(&lofi_lock);

	if (!valid_filename(klip->li_filename)) {
		error = EINVAL;
		goto out;
	}

	if (file_to_minor(klip->li_filename) != 0) {
		error = EBUSY;
		goto out;
	}

	if (pickminor) {
		/* Find a free one */
		for (newminor = 1; newminor <= lofi_max_files; newminor++)
			if (ddi_get_soft_state(lofi_statep, newminor) == NULL)
				break;
		if (newminor >= lofi_max_files) {
			error = EAGAIN;
			goto out;
		}
	} else {
		newminor = klip->li_minor;
		if (ddi_get_soft_state(lofi_statep, newminor) != NULL) {
			error = EEXIST;
			goto out;
		}
	}

	/* make sure it's valid */
	error = lookupname(klip->li_filename, UIO_SYSSPACE, FOLLOW,
	    NULLVPP, &vp);
	if (error) {
		goto out;
	}
	v_type = vp->v_type;
	VN_RELE(vp);
	if (!V_ISLOFIABLE(v_type)) {
		error = EINVAL;
		goto out;
	}
	flag = FREAD | FWRITE | FOFFMAX | FEXCL;
	error = vn_open(klip->li_filename, UIO_SYSSPACE, flag, 0, &vp, 0, 0);
	if (error) {
		/* try read-only */
		flag &= ~FWRITE;
		error = vn_open(klip->li_filename, UIO_SYSSPACE, flag, 0,
		    &vp, 0, 0);
		if (error) {
			goto out;
		}
	}
	vattr.va_mask = AT_SIZE;
	error = VOP_GETATTR(vp, &vattr, 0, credp);
	if (error) {
		goto closeout;
	}
	/* the file needs to be a multiple of the block size */
	if ((vattr.va_size % DEV_BSIZE) != 0) {
		error = EINVAL;
		goto closeout;
	}
	newdev = makedevice(getmajor(dev), newminor);
	Size_prop_val = vattr.va_size;
	if ((ddi_prop_update_int64(newdev, lofi_dip,
	    SIZE_PROP_NAME, Size_prop_val)) != DDI_PROP_SUCCESS) {
		error = EINVAL;
		goto closeout;
	}
	Nblocks_prop_val = vattr.va_size / DEV_BSIZE;
	if ((ddi_prop_update_int64(newdev, lofi_dip,
	    NBLOCKS_PROP_NAME, Nblocks_prop_val)) != DDI_PROP_SUCCESS) {
		error = EINVAL;
		goto propout;
	}
	error = ddi_soft_state_zalloc(lofi_statep, newminor);
	if (error == DDI_FAILURE) {
		error = ENOMEM;
		goto propout;
	}
	zalloced = 1;
	(void) snprintf(namebuf, sizeof (namebuf), "%d", newminor);
	(void) ddi_create_minor_node(lofi_dip, namebuf, S_IFBLK, newminor,
	    DDI_PSEUDO, NULL);
	if (error != DDI_SUCCESS) {
		error = ENXIO;
		goto propout;
	}
	(void) snprintf(namebuf, sizeof (namebuf), "%d,raw", newminor);
	error = ddi_create_minor_node(lofi_dip, namebuf, S_IFCHR, newminor,
	    DDI_PSEUDO, NULL);
	if (error != DDI_SUCCESS) {
		/* remove block node */
		(void) snprintf(namebuf, sizeof (namebuf), "%d", newminor);
		ddi_remove_minor_node(lofi_dip, namebuf);
		error = ENXIO;
		goto propout;
	}
	lsp = ddi_get_soft_state(lofi_statep, newminor);
	lsp->ls_filename_sz = strlen(klip->li_filename) + 1;
	lsp->ls_filename = kmem_alloc(lsp->ls_filename_sz, KM_SLEEP);
	(void) snprintf(namebuf, sizeof (namebuf), "%s_taskq_%d",
	    LOFI_DRIVER_NAME, newminor);
	lsp->ls_taskq = taskq_create(namebuf, lofi_taskq_nthreads,
	    minclsyspri, 1, lofi_taskq_maxalloc, 0);
	lsp->ls_kstat = kstat_create(LOFI_DRIVER_NAME, newminor,
	    NULL, "disk", KSTAT_TYPE_IO, 1, 0);
	if (lsp->ls_kstat) {
		mutex_init(&lsp->ls_kstat_lock, NULL, MUTEX_DRIVER, NULL);
		lsp->ls_kstat->ks_lock = &lsp->ls_kstat_lock;
		kstat_install(lsp->ls_kstat);
	}
	/*
	 * save open mode so file can be closed properly and vnode counts
	 * updated correctly.
	 */
	lsp->ls_openflag = flag;

	/*
	 * Try to handle stacked lofs vnodes.
	 */
	if (vp->v_type == VREG) {
		if (VOP_REALVP(vp, &lsp->ls_vp) != 0) {
			lsp->ls_vp = vp;
		} else {
			/*
			 * Even though vp was obtained via vn_open(), we
			 * can't call vn_close() on it, since lofs will
			 * pass the VOP_CLOSE() on down to the realvp
			 * (which we are about to use). Hence we merely
			 * drop the reference to the lofs vnode and hold
			 * the realvp so things behave as if we've
			 * opened the realvp without any interaction
			 * with lofs.
			 */
			VN_HOLD(lsp->ls_vp);
			VN_RELE(vp);
		}
	} else {
		lsp->ls_vp = vp;
	}
	lsp->ls_vp_size = vattr.va_size;
	(void) strcpy(lsp->ls_filename, klip->li_filename);
	if (rvalp)
		*rvalp = (int)newminor;
	klip->li_minor = newminor;

	fake_disk_geometry(lsp);
	mutex_exit(&lofi_lock);
	(void) copy_out_lofi_ioctl(klip, ulip);
	free_lofi_ioctl(klip);
	return (0);

propout:
	(void) ddi_prop_remove(newdev, lofi_dip, SIZE_PROP_NAME);
	(void) ddi_prop_remove(newdev, lofi_dip, NBLOCKS_PROP_NAME);
closeout:
	(void) VOP_CLOSE(vp, flag, 1, 0, credp);
	VN_RELE(vp);
out:
	if (zalloced)
		ddi_soft_state_free(lofi_statep, newminor);
	mutex_exit(&lofi_lock);
	free_lofi_ioctl(klip);
	return (error);
}

/*
 * unmap a file.
 */
static int
lofi_unmap_file(dev_t dev, struct lofi_ioctl *ulip, int byfilename,
    struct cred *credp)
{
	struct lofi_state *lsp;
	struct lofi_ioctl *klip;
	minor_t	minor;
	char	namebuf[20];
	dev_t	newdev;

	klip = copy_in_lofi_ioctl(ulip);
	if (klip == NULL)
		return (EFAULT);

	mutex_enter(&lofi_lock);
	if (byfilename) {
		minor = file_to_minor(klip->li_filename);
	} else {
		minor = klip->li_minor;
	}
	if (minor == 0) {
		mutex_exit(&lofi_lock);
		free_lofi_ioctl(klip);
		return (ENXIO);
	}
	lsp = ddi_get_soft_state(lofi_statep, minor);
	if (lsp == NULL) {
		mutex_exit(&lofi_lock);
		free_lofi_ioctl(klip);
		return (ENXIO);
	}
	if (is_opened(lsp)) {
		mutex_exit(&lofi_lock);
		free_lofi_ioctl(klip);
		return (EBUSY);
	}
	/*
	 * Use saved open mode to properly update vnode counts
	 */
	(void) VOP_CLOSE(lsp->ls_vp, lsp->ls_openflag, 1, 0, credp);
	VN_RELE(lsp->ls_vp);
	lsp->ls_vp = NULL;
	newdev = makedevice(getmajor(dev), minor);
	(void) ddi_prop_remove(newdev, lofi_dip, SIZE_PROP_NAME);
	(void) ddi_prop_remove(newdev, lofi_dip, NBLOCKS_PROP_NAME);

	(void) snprintf(namebuf, sizeof (namebuf), "%d", minor);
	ddi_remove_minor_node(lofi_dip, namebuf);
	(void) snprintf(namebuf, sizeof (namebuf), "%d,raw", minor);
	ddi_remove_minor_node(lofi_dip, namebuf);

	kmem_free(lsp->ls_filename, lsp->ls_filename_sz);
	taskq_destroy(lsp->ls_taskq);
	if (lsp->ls_kstat) {
		kstat_delete(lsp->ls_kstat);
		mutex_destroy(&lsp->ls_kstat_lock);
	}
	ddi_soft_state_free(lofi_statep, minor);
	klip->li_minor = minor;
	mutex_exit(&lofi_lock);
	(void) copy_out_lofi_ioctl(klip, ulip);
	free_lofi_ioctl(klip);
	return (0);
}

/*
 * get the filename given the minor number, or the minor number given
 * the name.
 */
/*ARGSUSED3*/
static int
lofi_get_info(dev_t dev, struct lofi_ioctl *ulip, int which,
    struct cred *credp)
{
	struct lofi_state *lsp;
	struct lofi_ioctl *klip;
	int	error;
	minor_t	minor;

#ifdef lint
	dev = dev;
#endif
	klip = copy_in_lofi_ioctl(ulip);
	if (klip == NULL)
		return (EFAULT);

	switch (which) {
	case LOFI_GET_FILENAME:
		minor = klip->li_minor;
		if (minor == 0) {
			free_lofi_ioctl(klip);
			return (EINVAL);
		}

		mutex_enter(&lofi_lock);
		lsp = ddi_get_soft_state(lofi_statep, minor);
		if (lsp == NULL) {
			mutex_exit(&lofi_lock);
			free_lofi_ioctl(klip);
			return (ENXIO);
		}
		(void) strcpy(klip->li_filename, lsp->ls_filename);
		mutex_exit(&lofi_lock);
		error = copy_out_lofi_ioctl(klip, ulip);
		free_lofi_ioctl(klip);
		return (error);
	case LOFI_GET_MINOR:
		mutex_enter(&lofi_lock);
		klip->li_minor = file_to_minor(klip->li_filename);
		mutex_exit(&lofi_lock);
		if (klip->li_minor == 0) {
			free_lofi_ioctl(klip);
			return (ENOENT);
		}
		error = copy_out_lofi_ioctl(klip, ulip);
		free_lofi_ioctl(klip);
		return (error);
	default:
		free_lofi_ioctl(klip);
		return (EINVAL);
	}

}

static int
lofi_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *credp,
    int *rvalp)
{
	int	error;
	enum dkio_state dkstate;
	struct lofi_state *lsp;
	minor_t	minor;

#ifdef lint
	credp = credp;
#endif

	minor = getminor(dev);
	/* lofi ioctls only apply to the master device */
	if (minor == 0) {
		struct lofi_ioctl *lip = (struct lofi_ioctl *)arg;

		/*
		 * the query command only need read-access - i.e., normal
		 * users are allowed to do those on the ctl device as
		 * long as they can open it read-only.
		 */
		switch (cmd) {
		case LOFI_MAP_FILE:
			if ((flag & FWRITE) == 0)
				return (EPERM);
			return (lofi_map_file(dev, lip, 1, rvalp, credp));
		case LOFI_MAP_FILE_MINOR:
			if ((flag & FWRITE) == 0)
				return (EPERM);
			return (lofi_map_file(dev, lip, 0, rvalp, credp));
		case LOFI_UNMAP_FILE:
			if ((flag & FWRITE) == 0)
				return (EPERM);
			return (lofi_unmap_file(dev, lip, 1, credp));
		case LOFI_UNMAP_FILE_MINOR:
			if ((flag & FWRITE) == 0)
				return (EPERM);
			return (lofi_unmap_file(dev, lip, 0, credp));
		case LOFI_GET_FILENAME:
			return (lofi_get_info(dev, lip, LOFI_GET_FILENAME,
			    credp));
		case LOFI_GET_MINOR:
			return (lofi_get_info(dev, lip, LOFI_GET_MINOR,
			    credp));
		case LOFI_GET_MAXMINOR:
			error = copyout(&lofi_max_files, &lip->li_minor,
			    sizeof (lofi_max_files));
			if (error)
				return (EFAULT);
			return (0);
		default:
			break;
		}
	}

	lsp = ddi_get_soft_state(lofi_statep, minor);
	if (lsp == NULL)
		return (ENXIO);

	/* these are for faking out utilities like newfs */
	switch (cmd) {
	case VOLIOCINFO:
		/* pcfs does this to see if it needs to set PCFS_NOCHK */
		/* 0 means it should set it */
		return (0);
	case DKIOCGVTOC:
		switch (ddi_model_convert_from(flag & FMODELS)) {
		case DDI_MODEL_ILP32: {
			struct vtoc32 vtoc32;

			vtoctovtoc32(lsp->ls_vtoc, vtoc32);
			if (ddi_copyout(&vtoc32, (void *)arg,
			    sizeof (struct vtoc32), flag))
				return (EFAULT);
				break;
			}

		case DDI_MODEL_NONE:
			if (ddi_copyout(&lsp->ls_vtoc, (void *)arg,
			    sizeof (struct vtoc), flag))
				return (EFAULT);
			break;
		}
		return (0);
	case DKIOCINFO:
		error = copyout(&lsp->ls_ci, (void *)arg,
		    sizeof (struct dk_cinfo));
		if (error)
			return (EFAULT);
		return (0);
	case DKIOCG_VIRTGEOM:
	case DKIOCG_PHYGEOM:
	case DKIOCGGEOM:
		error = copyout(&lsp->ls_dkg, (void *)arg,
		    sizeof (struct dk_geom));
		if (error)
			return (EFAULT);
		return (0);
	case DKIOCSTATE:
		/* the file is always there */
		dkstate = DKIO_INSERTED;
		error = copyout(&dkstate, (void *)arg,
		    sizeof (enum dkio_state));
		if (error)
			return (EFAULT);
		return (0);
	default:
		return (ENOTTY);
	}
}

static struct cb_ops lofi_cb_ops = {
	lofi_open,		/* open */
	lofi_close,		/* close */
	lofi_strategy,		/* strategy */
	nodev,			/* print */
	nodev,			/* dump */
	lofi_read,		/* read */
	lofi_write,		/* write */
	lofi_ioctl,		/* ioctl */
	nodev,			/* devmap */
	nodev,			/* mmap */
	nodev,			/* segmap */
	nochpoll,		/* poll */
	ddi_prop_op,		/* prop_op */
	0,			/* streamtab  */
	D_64BIT | D_NEW | D_MP,	/* Driver compatibility flag */
	CB_REV,
	lofi_aread,
	lofi_awrite
};

static struct dev_ops lofi_ops = {
	DEVO_REV,		/* devo_rev, */
	0,			/* refcnt  */
	lofi_info,		/* info */
	nulldev,		/* identify */
	nulldev,		/* probe */
	lofi_attach,		/* attach */
	lofi_detach,		/* detach */
	nodev,			/* reset */
	&lofi_cb_ops,		/* driver operations */
	NULL			/* no bus operations */
};

static struct modldrv modldrv = {
	&mod_driverops,
	"loopback file driver (%I%)",
	&lofi_ops,
};

static struct modlinkage modlinkage = {
	MODREV_1,
	&modldrv,
	NULL
};

int
_init(void)
{
	int error;

	error = ddi_soft_state_init(&lofi_statep,
	    sizeof (struct lofi_state), 0);
	if (error)
		return (error);

	mutex_init(&lofi_lock, NULL, MUTEX_DRIVER, NULL);
	error = mod_install(&modlinkage);
	if (error) {
		mutex_destroy(&lofi_lock);
		ddi_soft_state_fini(&lofi_statep);
	}

	return (error);
}

int
_fini(void)
{
	int	error;

	if (lofi_busy())
		return (EBUSY);

	error = mod_remove(&modlinkage);
	if (error)
		return (error);

	mutex_destroy(&lofi_lock);
	ddi_soft_state_fini(&lofi_statep);

	return (error);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
