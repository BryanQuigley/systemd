/*
 * namedev.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "list.h"
#include "udev.h"
#include "udev_version.h"
#include "namedev.h"
#include "libsysfs/libsysfs.h"
#include "klibc_fixups.h"

LIST_HEAD(config_device_list);
LIST_HEAD(perm_device_list);

/* compare string with pattern (supports * ? [0-9] [!A-Z]) */
static int strcmp_pattern(const char *p, const char *s)
{
	if (*s == '\0') {
		while (*p == '*')
			p++;
		return (*p != '\0');
	}
	switch (*p) {
	case '[':
		{
			int not = 0;
			p++;
			if (*p == '!') {
				not = 1;
				p++;
			}
			while (*p && (*p != ']')) {
				int match = 0;
				if (p[1] == '-') {
					if ((*s >= *p) && (*s <= p[2]))
						match = 1;
					p += 3;
				} else {
					match = (*p == *s);
					p++;
				}
				if (match ^ not) {
					while (*p && (*p != ']'))
						p++;
					return strcmp_pattern(p+1, s+1);
				}
			}
		}
		break;
	case '*':
		if (strcmp_pattern(p, s+1))
			return strcmp_pattern(p+1, s);
		return 0;
	case '\0':
		if (*s == '\0') {
			return 0;
		}
		break;
	default:
		if ((*p == *s) || (*p == '?'))
			return strcmp_pattern(p+1, s+1);
		break;
	}
	return 1;
}

#define copy_var(a, b, var)		\
	if (b->var)			\
		a->var = b->var;

#define copy_string(a, b, var)		\
	if (strlen(b->var))		\
		strcpy(a->var, b->var);

int add_perm_dev(struct perm_device *new_dev)
{
	struct list_head *tmp;
	struct perm_device *tmp_dev;

	/* update the values if we already have the device */
	list_for_each(tmp, &perm_device_list) {
		struct perm_device *dev = list_entry(tmp, struct perm_device, node);
		if (strcmp_pattern(new_dev->name, dev->name))
			continue;
		copy_var(dev, new_dev, mode);
		copy_string(dev, new_dev, owner);
		copy_string(dev, new_dev, group);
		return 0;
	}

	/* not found, add new structure to the perm list */
	tmp_dev = malloc(sizeof(*tmp_dev));
	if (!tmp_dev)
		return -ENOMEM;
	memcpy(tmp_dev, new_dev, sizeof(*tmp_dev));
	list_add_tail(&tmp_dev->node, &perm_device_list);
	//dump_perm_dev(tmp_dev);
	return 0;
}

static struct perm_device *find_perm(char *name)
{
	struct list_head *tmp;
	struct perm_device *perm = NULL;

	list_for_each(tmp, &perm_device_list) {
		perm = list_entry(tmp, struct perm_device, node);
		if (strcmp_pattern(perm->name, name))
			continue;
		return perm;
	}
	return NULL;
}

static mode_t get_default_mode(struct sysfs_class_device *class_dev)
{
	mode_t mode = 0600;	/* default to owner rw only */

	if (strlen(default_mode_str) != 0) {
		mode = strtol(default_mode_str, NULL, 8);
	}
	return mode;
}

static void apply_format(struct udevice *udev, unsigned char *string)
{
	char name[NAME_SIZE];
	char temp[NAME_SIZE];
	char *tail;
	char *pos;
	char *pos2;
	char *pos3;
	int num;

	while (1) {
		num = 0;
		pos = strchr(string, '%');

		if (pos) {
			*pos = '\0';
			tail = pos+1;
			if (isdigit(tail[0])) {
				num = (int) strtoul(&pos[1], &tail, 10);
				if (tail == NULL) {
					dbg("format parsing error '%s'", pos+1);
					break;
				}
			}
			strfieldcpy(name, tail+1);

			switch (tail[0]) {
			case 'b':
				if (strlen(udev->bus_id) == 0)
					break;
				strcat(pos, udev->bus_id);
				dbg("substitute bus_id '%s'", udev->bus_id);
				break;
			case 'k':
				if (strlen(udev->kernel_name) == 0)
					break;
				strcat(pos, udev->kernel_name);
				dbg("substitute kernel name '%s'", udev->kernel_name);
				break;
			case 'n':
				if (strlen(udev->kernel_number) == 0)
					break;
				strcat(pos, udev->kernel_number);
				dbg("substitute kernel number '%s'", udev->kernel_number);
				break;
			case 'D':
				if (strlen(udev->kernel_number) == 0) {
					strcat(pos, "disc");
					dbg("substitute devfs disc");
					break;
				}
				strcat(pos, "part");
				strcat(pos, udev->kernel_number);
				dbg("substitute devfs part '%s'", udev->kernel_number);
				break;
			case 'm':
				sprintf(pos, "%u", udev->minor);
				dbg("substitute minor number '%u'", udev->minor);
				break;
			case 'M':
				sprintf(pos, "%u", udev->major);
				dbg("substitute major number '%u'", udev->major);
				break;
			case 'c':
				if (strlen(udev->callout_value) == 0)
					break;
				if (num) {
					/* get part of return string */
					strncpy(temp, udev->callout_value, sizeof(temp));
					pos2 = temp;
					while (num) {
						num--;
						pos3 = strsep(&pos2, " ");
						if (pos3 == NULL) {
							dbg("requested part of callout string not found");
							break;
						}
					}
					if (pos3) {
						strcat(pos, pos3);
						dbg("substitute partial callout output '%s'", pos3);
					}
				} else {
					strcat(pos, udev->callout_value);
					dbg("substitute callout output '%s'", udev->callout_value);
				}
				break;
			default:
				dbg("unknown substitution type '%%%c'", pos[1]);
				break;
			}
			strcat(string, name);
		} else
			break;
	}
}

static struct bus_file {
	char *bus;
	char *file;
} bus_files[] = {
	{ .bus = "scsi",	.file = "vendor" },
	{ .bus = "usb",		.file = "idVendor" },
	{ .bus = "ide",		.file = "detach_state" },
	{ .bus = "pci",		.file = "vendor" },
	{}
};

#define SECONDS_TO_WAIT_FOR_FILE	10
static void wait_for_device_to_initialize(struct sysfs_device *sysfs_device)
{
	/* sleep until we see the file for this specific bus type show up this
	 * is needed because we can easily out-run the kernel in looking for
	 * these files before the paticular subsystem has created them in the
	 * sysfs tree properly.
	 *
	 * And people thought that the /sbin/hotplug event system was going to
	 * be slow, poo on you for arguing that before even testing it...
	 */
	struct bus_file *b = &bus_files[0];
	struct sysfs_attribute *tmpattr;
	int loop;

	while (1) {
		if (b->bus == NULL)
			break;
		if (strcmp(sysfs_device->bus, b->bus) == 0) {
			tmpattr = NULL;
			loop = SECONDS_TO_WAIT_FOR_FILE;
			while (loop--) {
				dbg("looking for file '%s' on bus '%s'", b->file, b->bus);
				tmpattr = sysfs_get_device_attr(sysfs_device, b->file);
				if (tmpattr) {
					/* found it! */
					goto exit;
				}
				/* sleep to give the kernel a chance to create the file */
				sleep(1);
			}
			dbg("Timed out waiting for '%s' file, continuing on anyway...", b->file);
			goto exit;
		}
		b++;
	}
	dbg("Did not find bus type '%s' on list of bus_id_files, contact greg@kroah.com", sysfs_device->bus);
exit:
	return; /* here to prevent compiler warning... */
}

static int exec_callout(struct config_device *dev, char *value, int len)
{
	int retval;
	int res;
	int status;
	int fds[2];
	pid_t pid;
	int value_set = 0;
	char buffer[256];
	char *pos;
	char *args[CALLOUT_MAXARG];
	int i;

	dbg("callout to '%s'", dev->exec_program);
	retval = pipe(fds);
	if (retval != 0) {
		dbg("pipe failed");
		return -1;
	}
	pid = fork();
	if (pid == -1) {
		dbg("fork failed");
		return -1;
	}

	if (pid == 0) {
		/* child */
		close(STDOUT_FILENO);
		dup(fds[1]);	/* dup write side of pipe to STDOUT */
		if (strchr(dev->exec_program, ' ')) {
			/* callout with arguments */
			pos = dev->exec_program;
			for (i=0; i < CALLOUT_MAXARG-1; i++) {
				args[i] = strsep(&pos, " ");
				if (args[i] == NULL)
					break;
			}
			if (args[i]) {
				dbg("too many args - %d", i);
				args[i] = NULL;
			}
			retval = execve(args[0], args, main_envp);
		} else {
			retval = execve(dev->exec_program, main_argv, main_envp);
		}
		if (retval != 0) {
			dbg("child execve failed");
			exit(1);
		}
		return -1; /* avoid compiler warning */
	} else {
		/* parent reads from fds[0] */
		close(fds[1]);
		retval = 0;
		while (1) {
			res = read(fds[0], buffer, sizeof(buffer) - 1);
			if (res <= 0)
				break;
			buffer[res] = '\0';
			if (res > len) {
				dbg("callout len %d too short", len);
				retval = -1;
			}
			if (value_set) {
				dbg("callout value already set");
				retval = -1;
			} else {
				value_set = 1;
				strncpy(value, buffer, len);
				pos = value + strlen(value)-1;
				if (pos[0] == '\n')
				pos[0] = '\0';
				dbg("callout returned '%s'", value);
			}
		}
		close(fds[0]);
		res = wait(&status);
		if (res < 0) {
			dbg("wait failed result %d", res);
			retval = -1;
		}

#ifndef __KLIBC__
		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			dbg("callout program status 0x%x", status);
			retval = -1;
		}
#endif
	}
	return retval;
}

static int do_callout(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct config_device *dev;
	struct list_head *tmp;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != CALLOUT)
			continue;

		if (dev->bus[0] != '\0') {
			/* as the user specified a bus, we must match it up */
			if (!sysfs_device)
				continue;
			dbg("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
			if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
				continue;
		}

		/* substitute anything that needs to be in the program name */
		apply_format(udev, dev->exec_program);
		if (exec_callout(dev, udev->callout_value, NAME_SIZE))
			continue;
		if (strcmp_pattern(dev->id, udev->callout_value) != 0)
			continue;
		strfieldcpy(udev->name, dev->name);
		strfieldcpy(udev->symlink, dev->symlink);
		dbg("callout returned matching value '%s', '%s' becomes '%s'",
		    dev->id, class_dev->name, udev->name);
		return 0;
	}
	return -ENODEV;
}

static int match_pair(struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device, struct sysfs_pair *pair)
{
	struct sysfs_attribute *tmpattr = NULL;
	char *c;

	if ((pair == NULL) || (pair->file[0] == '\0') || (pair->value == '\0'))
		return -ENODEV;

	dbg("look for device attribute '%s'", pair->file);
	/* try to find the attribute in the class device directory */
	tmpattr = sysfs_get_classdev_attr(class_dev, pair->file);
	if (tmpattr)
		goto label_found;

	/* look in the class device directory if present */
	if (sysfs_device) {
		tmpattr = sysfs_get_device_attr(sysfs_device, pair->file);
		if (tmpattr)
			goto label_found;
	}

	return -ENODEV;

label_found:
	c = tmpattr->value + strlen(tmpattr->value)-1;
	if (*c == '\n')
		*c = 0x00;
	dbg("compare attribute '%s' value '%s' with '%s'",
		  pair->file, tmpattr->value, pair->value);
	if (strcmp_pattern(pair->value, tmpattr->value) != 0)
		return -ENODEV;

	dbg("found matching attribute '%s' with value '%s'",
	    pair->file, pair->value);
	return 0;
}

static int do_label(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct sysfs_pair *pair;
	struct config_device *dev;
	struct list_head *tmp;
	int i;
	int match;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != LABEL)
			continue;

		if (dev->bus[0] != '\0') {
			/* as the user specified a bus, we must match it up */
			if (!sysfs_device)
				continue;
			dbg("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
			if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
				continue;
		}

		match = 1;
		for (i = 0; i < MAX_SYSFS_PAIRS; ++i) {
			pair = &dev->sysfs_pair[i];
			if ((pair->file[0] == '\0') || (pair->value[0] == '\0'))
				break;
			if (match_pair(class_dev, sysfs_device, pair) != 0) {
				match = 0;
				break;
			}
		}
		if (match == 0)
			continue;

		/* found match */
		strfieldcpy(udev->name, dev->name);
		strfieldcpy(udev->symlink, dev->symlink);
		dbg("found matching attribute, '%s' becomes '%s' ",
		    class_dev->name, udev->name);

		return 0;
	}
	return -ENODEV;
}

static int do_number(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct config_device *dev;
	struct list_head *tmp;
	char path[SYSFS_PATH_MAX];
	int found;
	char *temp = NULL;

	/* we have to have a sysfs device for NUMBER to work */
	if (!sysfs_device)
		return -ENODEV;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != NUMBER)
			continue;

		dbg("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
		if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
			continue;

		found = 0;
		strfieldcpy(path, sysfs_device->path);
		temp = strrchr(path, '/');
		dbg("search '%s' in '%s', path='%s'", dev->id, temp, path);
		if (strstr(temp, dev->id) != NULL) {
			found = 1;
		} else {
			*temp = 0x00;
			temp = strrchr(path, '/');
			dbg("search '%s' in '%s', path='%s'", dev->id, temp, path);
			if (strstr(temp, dev->id) != NULL)
				found = 1;
		}
		if (!found)
			continue;
		strfieldcpy(udev->name, dev->name);
		strfieldcpy(udev->symlink, dev->symlink);
		dbg("found matching id '%s', '%s' becomes '%s'",
		    dev->id, class_dev->name, udev->name);
		return 0;
	}
	return -ENODEV;
}

static int do_topology(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct config_device *dev;
	struct list_head *tmp;
	char path[SYSFS_PATH_MAX];
	int found;
	char *temp = NULL;

	/* we have to have a sysfs device for TOPOLOGY to work */
	if (!sysfs_device)
		return -ENODEV;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != TOPOLOGY)
			continue;

		dbg("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
		if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
			continue;

		found = 0;
		strfieldcpy(path, sysfs_device->path);
		temp = strrchr(path, '/');
		dbg("search '%s' in '%s', path='%s'", dev->place, temp, path);
		if (strstr(temp, dev->place) != NULL) {
			found = 1;
		} else {
			*temp = 0x00;
			temp = strrchr(path, '/');
			dbg("search '%s' in '%s', path='%s'", dev->place, temp, path);
			if (strstr(temp, dev->place) != NULL)
				found = 1;
		}
		if (!found)
			continue;

		strfieldcpy(udev->name, dev->name);
		strfieldcpy(udev->symlink, dev->symlink);
		dbg("found matching place '%s', '%s' becomes '%s'",
		    dev->place, class_dev->name, udev->name);
		return 0;
	}
	return -ENODEV;
}

static int do_replace(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct config_device *dev;
	struct list_head *tmp;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != REPLACE)
			continue;

		dbg("compare name '%s' with '%s'", dev->kernel_name, class_dev->name);
		if (strcmp_pattern(dev->kernel_name, class_dev->name) != 0)
			continue;

		strfieldcpy(udev->name, dev->name);
		strfieldcpy(udev->symlink, dev->symlink);
		dbg("found name, '%s' becomes '%s'", dev->kernel_name, udev->name);
		
		return 0;
	}
	return -ENODEV;
}

static void do_kernelname(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	/* heh, this is pretty simple... */
	strfieldcpy(udev->name, class_dev->name);
}

static struct sysfs_device *get_sysfs_device(struct sysfs_class_device *class_dev)
{
	struct sysfs_device *sysfs_device;
	struct sysfs_class_device *class_dev_parent;
	int loop;
	char filename[SYSFS_PATH_MAX + 6];
	int retval;
	char *temp;
	int partition = 0;

	/* Figure out where the device symlink is at.  For char devices this will
	 * always be in the class_dev->path.  But for block devices, it's different.
	 * The main block device will have the device symlink in it's path, but
	 * all partitions have the symlink in its parent directory.
	 * But we need to watch out for block devices that do not have parents, yet
	 * look like a partition (fd0, loop0, etc.)  They all do not have a device
	 * symlink yet.  We do sit and spin on waiting for them right now, we should
	 * possibly have a whitelist for these devices here...
	 */
	strcpy(filename, class_dev->path);
	dbg("filename = %s", filename);
	if (strcmp(class_dev->classname, SYSFS_BLOCK_NAME) == 0) {
		if (isdigit(class_dev->path[strlen(class_dev->path)-1])) {
			temp = strrchr(filename, '/');
			if (temp) {
				partition = 1;
				*temp = 0x00;
				char *temp2 = strrchr(filename, '/');
				dbg("temp2 = %s", temp2);
				if (temp2 && (strcmp(temp2, "/block") == 0)) {
					/* oops, we have no parent block device, so go back to original directory */
					strcpy(filename, class_dev->path);
					partition = 0;
				}
			}
		}
	}
	strcat(filename, "/device");

	loop = 2;
	while (loop--) {
		struct stat buf;
		dbg("looking for '%s'", filename);
		retval = stat(filename, &buf);
		if (!retval)
			break;
		/* sleep to give the kernel a chance to create the device file */
		sleep(1);
	}

	loop = 1;	/* FIXME put a real value in here for when everything is fixed... */
	while (loop--) {
		/* find the sysfs_device for this class device */
		/* Wouldn't it really be nice if libsysfs could do this for us? */
		sysfs_device = sysfs_get_classdev_device(class_dev);
		if (sysfs_device != NULL)
			goto exit;

		/* if it's a partition, we need to get the parent device */
		if (partition) {
			/* FIXME  HACK HACK HACK HACK
			 * for some reason partitions need this extra sleep here, in order
			 * to wait for the device properly.  Once the libsysfs code is
			 * fixed properly, this sleep should go away, and we can just loop above.
			 */
			sleep(1);
			dbg("really is a partition");
			class_dev_parent = sysfs_get_classdev_parent(class_dev);
			if (class_dev_parent == NULL) {
				dbg("sysfs_get_classdev_parent for class device '%s' failed", class_dev->name);
			} else {
				dbg("class_dev_parent->name='%s'", class_dev_parent->name);
				sysfs_device = sysfs_get_classdev_device(class_dev_parent);
				if (sysfs_device != NULL)
					goto exit;
			}
		}
		/* sleep to give the kernel a chance to create the link */
		/* FIXME remove comment...
		sleep(1); */
	}
	dbg("Timed out waiting for device symlink, continuing on anyway...");
exit:
	return sysfs_device;
}

int namedev_name_device(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	struct sysfs_device *sysfs_device = NULL;
	int retval = 0;
	struct perm_device *perm;
	char *pos;

	udev->mode = 0;

	/* find the sysfs_device associated with this class device */
	sysfs_device = get_sysfs_device(class_dev);
	if (sysfs_device) {
		dbg("sysfs_device->path='%s'", sysfs_device->path);
		dbg("sysfs_device->bus_id='%s'", sysfs_device->bus_id);
		dbg("sysfs_device->bus='%s'", sysfs_device->bus);
		strfieldcpy(udev->bus_id, sysfs_device->bus_id);
		wait_for_device_to_initialize(sysfs_device);
	} else {
		dbg("class_dev->name = '%s'", class_dev->name);
	}

	strfieldcpy(udev->kernel_name, class_dev->name);

	/* get kernel number */
	pos = class_dev->name + strlen(class_dev->name);
	while (isdigit(*(pos-1)))
		pos--;
	strfieldcpy(udev->kernel_number, pos);
	dbg("kernel_number='%s'", udev->kernel_number);

	/* rules are looked at in priority order */
	retval = do_callout(class_dev, udev, sysfs_device);
	if (retval == 0)
		goto found;

	retval = do_label(class_dev, udev, sysfs_device);
	if (retval == 0)
		goto found;

	retval = do_number(class_dev, udev, sysfs_device);
	if (retval == 0)
		goto found;

	retval = do_topology(class_dev, udev, sysfs_device);
	if (retval == 0)
		goto found;

	retval = do_replace(class_dev, udev, sysfs_device);
	if (retval == 0)
		goto found;

	do_kernelname(class_dev, udev);
	goto done;

found:
	/* substitute placeholder */
	apply_format(udev, udev->name);
	apply_format(udev, udev->symlink);

done:
	perm = find_perm(udev->name);
	if (perm) {
		udev->mode = perm->mode;
		strfieldcpy(udev->owner, perm->owner);
		strfieldcpy(udev->group, perm->group);
	} else {
		/* no matching perms found :( */
		udev->mode = get_default_mode(class_dev);
		udev->owner[0] = 0x00;
		udev->group[0] = 0x00;
	}
	dbg("name, '%s' is going to have owner='%s', group='%s', mode = %#o",
	    udev->name, udev->owner, udev->group, udev->mode);

	return 0;
}

int namedev_init(void)
{
	int retval;

	retval = namedev_init_rules();
	if (retval)
		return retval;

	retval = namedev_init_permissions();
	if (retval)
		return retval;

	dump_config_dev_list();
	dump_perm_dev_list();
	return retval;
}
