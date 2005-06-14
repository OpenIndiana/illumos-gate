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
 * namespace utilities
 */

#include <meta.h>

typedef struct deviceinfo {
	char	*bname;		/* block name of the device */
	char	*dname;		/* driver for the device */
	minor_t	mnum;		/* minor number for the device */
} deviceinfo_t;

static	deviceinfo_t	devlist[MD_MNMAXSIDES];

/*
 * Ask the driver for the device name, driver name, and minor number;
 * which has been stored in the metadevice state database
 * (on behalf of the utilities).
 * (by key)
 */
char *
meta_getnmentbykey(
	set_t		setno,
	side_t		sideno,
	mdkey_t		key,
	char		**drvnm,
	minor_t		*mnum,
	md_dev64_t	*dev,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;
	static char		device_name[MAXPATHLEN];

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = setno;
	nm.side = sideno;
	nm.key = key;
	nm.devname = (uint64_t)device_name;

	if (metaioctl(MD_IOCGET_NM, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (NULL);
	}

	if (drvnm != NULL)
		*drvnm = Strdup(nm.drvnm);

	if (mnum != NULL)
		*mnum = nm.mnum;

	if (dev != NULL)
		*dev = meta_expldev(makedevice(nm.major, nm.mnum));

	return (Strdup(device_name));
}

/*
 * Ask the driver for the minor name which has been stored in the
 * metadevice state database.
 * (by key)
 */
char *
meta_getdidminorbykey(
	set_t		setno,
	side_t		sideno,
	mdkey_t		key,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;
	static char		minorname[MAXPATHLEN];

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = setno;
	nm.side = sideno;
	nm.key = key;
	nm.minorname = (uint64_t)minorname;

	if (metaioctl(MD_IOCGET_DIDMIN, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (NULL);
	}

	return (Strdup(minorname));
}

/*
 * Ask the driver for the device id string which has been stored in the
 * metadevice state database (on behalf of the utilities).
 * (by key)
 */
ddi_devid_t
meta_getdidbykey(
	set_t		setno,
	side_t		sideno,
	mdkey_t		key,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = setno;
	nm.side = sideno;
	nm.key = key;

	/*
	 * First ask the driver for the size of the device id string.  This is
	 * signaled by passing the driver a devid_size of zero.
	 */
	nm.devid_size = 0;
	if (metaioctl(MD_IOCGET_DID, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (NULL);
	}

	/*
	 * If the devid_size is still zero then something is wrong.
	 */
	if (nm.devid_size == 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (NULL);
	}

	/*
	 * Now go get the actual device id string.  Caller is responsible for
	 * free'ing device id memory buffer.
	 */
	if ((nm.devid = (uintptr_t)malloc(nm.devid_size)) == NULL) {
		return (NULL);
	}
	if (metaioctl(MD_IOCGET_DID, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		(void) free((void *)nm.devid);
		return (NULL);
	}

	return ((void *)nm.devid);
}

/*
 * set the devid.
 */
int
meta_setdid(
	set_t		setno,
	side_t		sideno,
	mdkey_t		key,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;
	int			i;

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = setno;
	nm.side = sideno;
	nm.key = key;

	if (metaioctl(MD_IOCSET_DID, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (-1);
	}

	if (setno == MD_LOCAL_SET) {
		/*
		 * If this is the local set then we are adding in the devids
		 * for the disks in the diskset and so this means adding
		 * a reference count for each side. Need to do this after
		 * the initial add so that the correct devid is picked up.
		 * The key is the key of the drive record and as such this
		 * means the minor number of the device which is used to
		 * get the devid. If the wrong side is used then it would
		 * be possible to get the wrong devid in the namespace, hence
		 * the requirement to process the local side first of all.
		 */
		for (i = 0 + SKEW; i < MD_MAXSIDES; i++) {
			/*
			 * We can just call the ioctl again because it will
			 * fail with ENOENT if the side does not exist, and
			 * more importantly does not increment the usage count
			 * on the devid.
			 */
			nm.side = (side_t)i;
			if (nm.side == sideno)
				continue;
			if (metaioctl(MD_IOCSET_DID, &nm, &nm.mde, NULL) != 0) {
				if (mdissyserror(&nm.mde, ENODEV)) {
					mdclrerror(&nm.mde);
				} else {
					(void) mdstealerror(ep, &nm.mde);
					return (-1);
				}
			}
		}
	}
	return (0);
}
/*
 * Ask the driver for the name, which has been stored in the
 * metadevice state database (on behalf of the utilities).
 * (by key)
 */
char *
meta_getnmbykey(
	set_t		setno,
	side_t		sideno,
	mdkey_t		key,
	md_error_t	*ep
)
{
	return (meta_getnmentbykey(setno, sideno, key, NULL, NULL, NULL, ep));
}

/*
 * Ask the driver for the device name, driver name, minor number, and key;
 * which has been stored in the metadevice state database
 * (on behalf of the utilities).
 * (by md_dev64_t)
 */
char *
meta_getnmentbydev(
	set_t		setno,
	side_t		sideno,
	md_dev64_t	dev,
	char		**drvnm,
	minor_t		*mnum,
	mdkey_t		*key,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;
	static char		device_name[MAXPATHLEN];

	/* must have a dev */
	assert(dev != NODEV64);

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = setno;
	nm.side = sideno;
	nm.key = MD_KEYWILD;
	nm.major = meta_getmajor(dev);
	nm.mnum = meta_getminor(dev);
	nm.devname = (uint64_t)device_name;

	if (metaioctl(MD_IOCGET_NM, &nm, &nm.mde, NULL) != 0) {
		(void) mdstealerror(ep, &nm.mde);
		return (NULL);
	}

	if (drvnm != NULL)
		*drvnm = Strdup(nm.drvnm);
	if (mnum != NULL)
		*mnum = nm.mnum;

	if (key != NULL)
		*key = nm.retkey;

	return (Strdup(device_name));
}

int
add_name(
	mdsetname_t	*sp,
	side_t		sideno,
	mdkey_t		key,
	char		*dname,
	minor_t		mnum,
	char		*bname,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = sp->setno;
	nm.side = sideno;
	nm.key = key;
	nm.mnum = mnum;
	(void) strncpy(nm.drvnm, dname, sizeof (nm.drvnm));
	nm.devname_len = strlen(bname) + 1;
	nm.devname = (uintptr_t)bname;

	if (metaioctl(MD_IOCSET_NM, &nm, &nm.mde, bname) < 0)
		return (mdstealerror(ep, &nm.mde));

	return (nm.key);
}

/*
 * Remove the device name which corresponds to the given device number.
 */
int
del_name(
	mdsetname_t	*sp,
	side_t		sideno,
	mdkey_t		key,
	md_error_t	*ep
)
{
	struct mdnm_params	nm;

	(void) memset(&nm, '\0', sizeof (nm));
	nm.setno = sp->setno;
	nm.side = sideno;
	nm.key = key;

	if (metaioctl(MD_IOCREM_NM, &nm, &nm.mde, NULL) != 0)
		return (mdstealerror(ep, &nm.mde));

	return (0);
}

static void
empty_devicelist()
{
	side_t	sideno;

	for (sideno = 0; sideno < MD_MNMAXSIDES; sideno++) {
		if (devlist[sideno].bname != (char *)NULL) {
			Free(devlist[sideno].bname);
			Free(devlist[sideno].dname);
			devlist[sideno].mnum = NODEV;
		}
	}
}

static void
add_to_devicelist(
	side_t		sideno,
	char		*bname,
	char		*dname,
	minor_t		mnum
)
{
	devlist[sideno].bname = Strdup(bname);
	devlist[sideno].dname = Strdup(dname);

	devlist[sideno].mnum = mnum;
}

/*
 * Build a list of the names on the systems, if this fails the caller
 * will tidy up the entries in the devlist.
 */
static int
build_sidenamelist(
	mdsetname_t	*sp,
	mdname_t	*np,
	md_error_t	*ep
)
{
	side_t		sideno = MD_SIDEWILD;
	minor_t		mnum = NODEV;
	char		*bname = NULL;
	char		*dname = NULL;
	int		err;

	/*CONSTCOND*/
	while (1) {

		if ((err = meta_getnextside_devinfo(sp, np->bname, &sideno,
		    &bname, &dname, &mnum, ep)) == -1)
			return (-1);

		if (err == 0)
			break;

		/* the sideno gives us the index into the array */
		add_to_devicelist(sideno, bname, dname, mnum);
	}
	return (0);
}

/*
 * add name key
 * the meta_create* functions should be the only ones using this. The
 * adding of a name to the namespace must be done in a particular order
 * to devid support for the disksets. The order is: add the 'local' side
 * first of all, so the devid lookup in the kernel will use the correct
 * device information and then add in the other sides.
 */
int
add_key_name(
	mdsetname_t	*sp,
	mdname_t	*np,
	mdnamelist_t	**nlpp,
	md_error_t	*ep
)
{
	int		err;
	side_t		sideno = MD_SIDEWILD;
	side_t		thisside;
	mdkey_t		key = MD_KEYWILD;
	md_set_desc	*sd;
	int		maxsides;

	/* should have a set */
	assert(sp != NULL);

	if (! metaislocalset(sp)) {
		if ((sd = metaget_setdesc(sp, ep)) == NULL) {
			return (-1);
		}
	}

	if (build_sidenamelist(sp, np, ep) == -1) {
		empty_devicelist();
		return (-1);
	}

	/*
	 * When a disk is added into the namespace the local information for
	 * that disk is added in first of all. For the local set this is not
	 * a concern and for the host that owns the diskset it is not a concern
	 * but when a disk is added in the remote namespace we *must* use the
	 * local information for that disk first of all. This is because when
	 * in the kernel (md_setdevname) the passed in dev_t is used to find
	 * the devid of the disk. This means we have to cater for the following:
	 *
	 * - a disk on the remote host having the dev_t that has been passed
	 *   into the kernel and this disk is not actually the disk that is
	 *   being added into the diskset.
	 * - the dev_t does not exist on this node
	 *
	 * So putting in the local information first of all makes sure that the
	 * dev_t passed into the kernel is correct with respect to that node
	 * and then any further additions for that name match on the key
	 * passed back.
	 */
	thisside = getmyside(sp, ep);

	if (devlist[thisside].dname == NULL ||
	    strlen(devlist[thisside].dname) == 0) {
		/*
		 * Did not find the disk information for the disk. This can
		 * be because of an inconsistancy in the namespace: that is the
		 * devid we have in the namespace does not exist on the
		 * system and thus when looking up the disk information
		 * using this devid we fail to find anything.
		 */
		(void) mdcomperror(ep, MDE_SP_COMP_OPEN_ERR, 0, np->dev,
		    np->cname);
		empty_devicelist();
		return (-1);
	}

	if ((err = add_name(sp, thisside, key, devlist[thisside].dname,
	    devlist[thisside].mnum, devlist[thisside].bname, ep)) == -1) {
		empty_devicelist();
		return (-1);
	}

	/* We now have a 'key' so add in the other sides */
	key = (mdkey_t)err;

	if (metaislocalset(sp))
		goto done;

	if (MD_MNSET_DESC(sd))
		maxsides = MD_MNMAXSIDES;
	else
		maxsides = MD_MAXSIDES;

	for (sideno = 0; sideno < maxsides; sideno++) {
		/* ignore thisside, as it has been added above */
		if (sideno == thisside)
			continue;

		if (devlist[sideno].dname != NULL) {
			err = add_name(sp, sideno, key, devlist[sideno].dname,
			    devlist[sideno].mnum, devlist[sideno].bname, ep);
			if (err == -1) {
				empty_devicelist();
				return (-1);
			}
		}
	}

done:
	empty_devicelist();
	/* save key, return success */
	np->key = key;
	if (nlpp != NULL)
		(void) metanamelist_append(nlpp, np);
	return (0);
}

/*
 * delete name key
 * the meta_create* functions should be the only ones using this. The
 * removal of the names must be done in a particular order: remove the
 * non-local entries first of all and then finally the local entry.
 */
int
del_key_name(
	mdsetname_t	*sp,
	mdname_t	*np,
	md_error_t	*ep
)
{
	side_t		sideno = MD_SIDEWILD;
	int		err;
	int		retval = 0;
	side_t		thisside;

	/* should have a set */
	assert(sp != NULL);

	/* should have a key */
	assert((np->key != MD_KEYWILD) && (np->key != MD_KEYBAD));

	thisside = getmyside(sp, ep);

	/* remove the remote sides first of all */
	for (;;) {
		if ((err = meta_getnextside_devinfo(sp, np->bname, &sideno,
		    NULL, NULL, NULL, ep)) == -1)
			return (-1);

		if (err == 0)
			break;

		/* ignore thisside */
		if (thisside == sideno) {
			continue;
		}
		if ((err = del_name(sp, sideno, np->key, ep)) == -1)
			retval = -1;
	}

	/* now remove this side */
	if (retval == 0)
		if ((err = del_name(sp, thisside, np->key, ep)) == -1)
			retval = -1;

	np->key = MD_KEYBAD;
	return (retval);
}

/*
 * delete namelist keys
 * the meta_create* functions should be the only ones using this
 */
int
del_key_names(
	mdsetname_t	*sp,
	mdnamelist_t	*nlp,
	md_error_t	*ep
)
{
	mdnamelist_t	*p;
	md_error_t	status = mdnullerror;
	int		rval = 0;

	/* if ignoring errors */
	if (ep == NULL)
		ep = &status;

	/* delete names */
	for (p = nlp; (p != NULL); p = p->next) {
		mdname_t	*np = p->namep;

		if (del_key_name(sp, np, ep) != 0)
			rval = -1;
	}

	/* cleanup, return success */
	if (ep == &status)
		mdclrerror(&status);
	return (rval);
}
