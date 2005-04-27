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

/* define this to enable parsing debugging */
/* #define DEBUG_PARSER */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "list.h"
#include "udev.h"
#include "udev_version.h"
#include "namedev.h"
#include "libsysfs/libsysfs.h"
#include "klibc_fixups.h"

#define TYPE_LABEL	"LABEL"
#define TYPE_NUMBER	"NUMBER"
#define TYPE_TOPOLOGY	"TOPOLOGY"
#define TYPE_REPLACE	"REPLACE"
#define TYPE_CALLOUT	"CALLOUT"
#define CALLOUT_MAXARG	8

static LIST_HEAD(config_device_list);

/* s2 may end with '*' to match everything */
static int strncmp_wildcard(char *s1, char *s2, int max)
{
	int len = strlen(s2);
	if (len > max)
		len = max;
	if (s2[len-1] == '*')
		len--;
	else
		len = max;
	return strncmp(s1, s2, len);
}

static void dump_dev(struct config_device *dev)
{
	switch (dev->type) {
	case KERNEL_NAME:
		dbg_parse("KERNEL name='%s' ,"
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->owner, dev->group, dev->mode);
		break;
	case LABEL:
		dbg_parse("LABEL name='%s', bus='%s', sysfs_file='%s', sysfs_value='%s', "
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->bus, dev->sysfs_file, dev->sysfs_value,
			  dev->owner, dev->group, dev->mode);
		break;
	case NUMBER:
		dbg_parse("NUMBER name='%s', bus='%s', id='%s', "
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->bus, dev->id,
			  dev->owner, dev->group, dev->mode);
		break;
	case TOPOLOGY:
		dbg_parse("TOPOLOGY name='%s', bus='%s', place='%s', "
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->bus, dev->place,
			  dev->owner, dev->group, dev->mode);
		break;
	case REPLACE:
		dbg_parse("REPLACE name=%s, kernel_name=%s, "
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->kernel_name,
			  dev->owner, dev->group, dev->mode);
		break;
	case CALLOUT:
		dbg_parse("CALLOUT name='%s', bus='%s', program='%s', id='%s', "
			  "owner='%s', group='%s', mode=%#o",
			  dev->name, dev->bus, dev->exec_program, dev->id,
			  dev->owner, dev->group, dev->mode);
		break;
	default:
		dbg_parse("unknown type of method");
	}
}

#define copy_var(a, b, var)		\
	if (b->var)			\
		a->var = b->var;

#define copy_string(a, b, var)		\
	if (strlen(b->var))		\
		strcpy(a->var, b->var);

static int add_dev(struct config_device *new_dev)
{
	struct list_head *tmp;
	struct config_device *tmp_dev;

	/* update the values if we already have the device */
	list_for_each(tmp, &config_device_list) {
		struct config_device *dev = list_entry(tmp, struct config_device, node);
		if (strncmp_wildcard(dev->name, new_dev->name, sizeof(dev->name)))
			continue;
		if (strncmp(dev->bus, new_dev->bus, sizeof(dev->name)))
			continue;
		copy_var(dev, new_dev, type);
		copy_var(dev, new_dev, mode);
		copy_string(dev, new_dev, bus);
		copy_string(dev, new_dev, sysfs_file);
		copy_string(dev, new_dev, sysfs_value);
		copy_string(dev, new_dev, id);
		copy_string(dev, new_dev, place);
		copy_string(dev, new_dev, kernel_name);
		copy_string(dev, new_dev, exec_program);
		copy_string(dev, new_dev, owner);
		copy_string(dev, new_dev, group);
		return 0;
	}

	/* not found, add new structure to the device list */
	tmp_dev = malloc(sizeof(*tmp_dev));
	if (!tmp_dev)
		return -ENOMEM;
	memcpy(tmp_dev, new_dev, sizeof(*tmp_dev));
	list_add_tail(&tmp_dev->node, &config_device_list);
	//dump_dev(tmp_dev);
	return 0;
}

static void dump_dev_list(void)
{
	struct list_head *tmp;

	list_for_each(tmp, &config_device_list) {
		struct config_device *dev = list_entry(tmp, struct config_device, node);
		dump_dev(dev);
	}
}
	
static int get_pair(char **orig_string, char **left, char **right)
{
	char *temp;
	char *string = *orig_string;

	if (!string)
		return -ENODEV;

	/* eat any whitespace */
	while (isspace(*string))
		++string;

	/* split based on '=' */
	temp = strsep(&string, "=");
	*left = temp;
	if (!string)
		return -ENODEV;

	/* take the right side and strip off the '"' */
	while (isspace(*string))
		++string;
	if (*string == '"')
		++string;
	else
		return -ENODEV;

	temp = strsep(&string, "\"");
	if (!string || *temp == '\0')
		return -ENODEV;
	*right = temp;
	*orig_string = string;
	
	return 0;
}

static int get_value(const char *left, char **orig_string, char **ret_string)
{
	int retval;
	char *left_string;

	retval = get_pair(orig_string, &left_string, ret_string);
	if (retval)
		return retval;
	if (strcasecmp(left_string, left) != 0)
		return -ENODEV;
	return 0;
}

static int namedev_init_config(void)
{
	char line[255];
	int lineno;
	char *temp;
	char *temp2;
	char *temp3;
	FILE *fd;
	int retval = 0;
	struct config_device dev;

	dbg("opening '%s' to read as config", udev_config_filename);
	fd = fopen(udev_config_filename, "r");
	if (fd == NULL) {
		dbg("can't open '%s'", udev_config_filename);
		return -ENODEV;
	}

	/* loop through the whole file */
	lineno = 0;
	while (1) {
		/* get a line */
		temp = fgets(line, sizeof(line), fd);
		if (temp == NULL)
			goto exit;
		lineno++;

		dbg_parse("read '%s'", temp);

		/* eat the whitespace at the beginning of the line */
		while (isspace(*temp))
			++temp;

		/* empty line? */
		if (*temp == 0x00)
			continue;

		/* see if this is a comment */
		if (*temp == COMMENT_CHARACTER)
			continue;

		memset(&dev, 0x00, sizeof(struct config_device));

		/* parse the line */
		temp2 = strsep(&temp, ",");
		if (strcasecmp(temp2, TYPE_LABEL) == 0) {
			/* label type */
			dev.type = LABEL;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.bus, temp3);

			/* file="value" */
			temp2 = strsep(&temp, ",");
			retval = get_pair(&temp, &temp2, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.sysfs_file, temp2);
			strfieldcpy(dev.sysfs_value, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.name, temp3);

			dbg_parse("LABEL name='%s', bus='%s', "
				  "sysfs_file='%s', sysfs_value='%s'",
				  dev.name, dev.bus, dev.sysfs_file,
				  dev.sysfs_value);
		}

		if (strcasecmp(temp2, TYPE_NUMBER) == 0) {
			/* number type */
			dev.type = NUMBER;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.bus, temp3);

			/* ID="id" */
			temp2 = strsep(&temp, ",");
			retval = get_value("ID", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.id, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.name, temp3);

			dbg_parse("NUMBER name='%s', bus='%s', id='%s'",
				  dev.name, dev.bus, dev.id);
		}

		if (strcasecmp(temp2, TYPE_TOPOLOGY) == 0) {
			/* number type */
			dev.type = TOPOLOGY;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.bus, temp3);

			/* PLACE="place" */
			temp2 = strsep(&temp, ",");
			retval = get_value("PLACE", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.place, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.name, temp3);

			dbg_parse("TOPOLOGY name='%s', bus='%s', place='%s'",
				  dev.name, dev.bus, dev.place);
		}

		if (strcasecmp(temp2, TYPE_REPLACE) == 0) {
			/* number type */
			dev.type = REPLACE;

			/* KERNEL="kernel_name" */
			retval = get_value("KERNEL", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.kernel_name, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.name, temp3);
			dbg_parse("REPLACE name='%s', kernel_name='%s'",
				  dev.name, dev.kernel_name);
		}
		if (strcasecmp(temp2, TYPE_CALLOUT) == 0) {
			/* number type */
			dev.type = CALLOUT;

			/* BUS="bus" */
			retval = get_value("BUS", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.bus, temp3);

			/* PROGRAM="executable" */
			temp2 = strsep(&temp, ",");
			retval = get_value("PROGRAM", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.exec_program, temp3);

			/* ID="id" */
			temp2 = strsep(&temp, ",");
			retval = get_value("ID", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.id, temp3);

			/* NAME="new_name" */
			temp2 = strsep(&temp, ",");
			retval = get_value("NAME", &temp, &temp3);
			if (retval)
				break;
			strfieldcpy(dev.name, temp3);
			dbg_parse("CALLOUT name='%s', program='%s'",
				  dev.name, dev.exec_program);
		}

		retval = add_dev(&dev);
		if (retval) {
			dbg("add_dev returned with error %d", retval);
			goto exit;
		}
	}
	dbg_parse("%s:%d:%Zd: error parsing '%s'", udev_config_filename,
		  lineno, temp - line, temp);
exit:
	fclose(fd);
	return retval;
}	


static int namedev_init_permissions(void)
{
	char line[255];
	char *temp;
	char *temp2;
	FILE *fd;
	int retval = 0;
	struct config_device dev;

	dbg("opening '%s' to read as permissions config", udev_config_permission_filename);
	fd = fopen(udev_config_permission_filename, "r");
	if (fd == NULL) {
		dbg("can't open '%s'", udev_config_permission_filename);
		return -ENODEV;
	}

	/* loop through the whole file */
	while (1) {
		temp = fgets(line, sizeof(line), fd);
		if (temp == NULL)
			break;

		dbg_parse("read '%s'", temp);

		/* eat the whitespace at the beginning of the line */
		while (isspace(*temp))
			++temp;

		/* empty line? */
		if (*temp == 0x00)
			continue;

		/* see if this is a comment */
		if (*temp == COMMENT_CHARACTER)
			continue;

		memset(&dev, 0x00, sizeof(dev));

		/* parse the line */
		temp2 = strsep(&temp, ":");
		if (!temp2) {
			dbg("cannot parse line '%s'", line);
			continue;
		}
		strncpy(dev.name, temp2, sizeof(dev.name));

		temp2 = strsep(&temp, ":");
		if (!temp2) {
			dbg("cannot parse line '%s'", line);
			continue;
		}
		strncpy(dev.owner, temp2, sizeof(dev.owner));

		temp2 = strsep(&temp, ":");
		if (!temp2) {
			dbg("cannot parse line '%s'", line);
			continue;
		}
		strncpy(dev.group, temp2, sizeof(dev.owner));

		if (!temp) {
			dbg("cannot parse line: %s", line);
			continue;
		}
		dev.mode = strtol(temp, NULL, 8);

		dbg_parse("name='%s', owner='%s', group='%s', mode=%#o",
			  dev.name, dev.owner, dev.group,
			  dev.mode);
		retval = add_dev(&dev);
		if (retval) {
			dbg("add_dev returned with error %d", retval);
			goto exit;
		}
	}

exit:
	fclose(fd);
	return retval;
}	

static mode_t get_default_mode(struct sysfs_class_device *class_dev)
{
	/* just default everyone to rw for the world! */
	return 0666;
}

static void build_kernel_number(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	char *dig;

	/* FIXME, figure out how to handle stuff like sdaj which will not work right now. */
	dig = class_dev->name + strlen(class_dev->name);
	while (isdigit(*(dig-1)))
		dig--;
	strfieldcpy(udev->kernel_number, dig);
	dbg("kernel_number='%s'", udev->kernel_number);
}

static void apply_format(struct udevice *udev, unsigned char *string)
{
	char name[NAME_SIZE];
	char *pos;

	while (1) {
		pos = strchr(string, '%');

		if (pos) {
			strfieldcpy(name, pos+2);
			*pos = 0x00;
			switch (pos[1]) {
			case 'b':
				if (strlen(udev->bus_id) == 0)
					break;
				strcat(pos, udev->bus_id);
				dbg("substitute bus_id '%s'", udev->bus_id);
				break;
			case 'n':
				if (strlen(udev->kernel_number) == 0)
					break;
				strcat(pos, udev->kernel_number);
				dbg("substitute kernel number '%s'", udev->kernel_number);
				break;
			case 'D':
				if (strlen(udev->kernel_number) == 0) {
					strcat(pos, "disk");
					break;
				}
				strcat(pos, "part");
				strcat(pos, udev->kernel_number);
				dbg("substitute kernel number '%s'", udev->kernel_number);
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
				strcat(pos, udev->callout_value);
				dbg("substitute callout output '%s'", udev->callout_value);
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


static int exec_callout(struct config_device *dev, char *value, int len)
{
	int retval;
	int res;
	int status;
	int fds[2];
	pid_t pid;
	int value_set = 0;
	char buffer[256];
	char *arg;
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
			arg = dev->exec_program;
			for (i=0; i < CALLOUT_MAXARG-1; i++) {
				args[i] = strsep(&arg, " ");
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
			}
		}
		dbg("callout returned '%s'", value);
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

		if (sysfs_device) {
			dbg_parse("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
			if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
				continue;
		}

		/* substitute anything that needs to be in the program name */
		apply_format(udev, dev->exec_program);
		if (exec_callout(dev, udev->callout_value, NAME_SIZE))
			continue;
		if (strncmp_wildcard(udev->callout_value, dev->id, NAME_SIZE) != 0)
			continue;
		strfieldcpy(udev->name, dev->name);
		if (dev->mode != 0) {
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
		dbg_parse("callout returned matching value '%s', '%s' becomes '%s'"
			  " - owner='%s', group='%s', mode=%#o",
			  dev->id, class_dev->name, udev->name,
			  dev->owner, dev->group, dev->mode);
		return 0;
	}
	return -ENODEV;
}

static int do_label(struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	struct sysfs_attribute *tmpattr = NULL;
	struct config_device *dev;
	struct list_head *tmp;

	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		if (dev->type != LABEL)
			continue;

		if (sysfs_device) {
			dbg_parse("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
			if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
				continue;
		}

		dbg_parse("look for device attribute '%s'", dev->sysfs_file);
		/* try to find the attribute in the class device directory */
		tmpattr = sysfs_get_classdev_attr(class_dev, dev->sysfs_file);
		if (tmpattr)
			goto label_found;

		/* look in the class device directory if present */
		if (sysfs_device) {
			tmpattr = sysfs_get_device_attr(sysfs_device, dev->sysfs_file);
			if (tmpattr)
				goto label_found;
		}

		continue;

label_found:
		tmpattr->value[strlen(tmpattr->value)-1] = 0x00;
		dbg_parse("compare attribute '%s' value '%s' with '%s'",
			  dev->sysfs_file, tmpattr->value, dev->sysfs_value);
		if (strcmp(dev->sysfs_value, tmpattr->value) != 0)
			continue;

		strfieldcpy(udev->name, dev->name);
		if (dev->mode != 0) {
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
		dbg_parse("found matching attribute '%s', '%s' becomes '%s' "
			  "- owner='%s', group='%s', mode=%#o",
			  dev->sysfs_file, class_dev->name, udev->name,
			  dev->owner, dev->group, dev->mode);

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

		dbg_parse("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
		if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
			continue;

		found = 0;
		strfieldcpy(path, sysfs_device->path);
		temp = strrchr(path, '/');
		dbg_parse("search '%s' in '%s', path='%s'", dev->id, temp, path);
		if (strstr(temp, dev->id) != NULL) {
			found = 1;
		} else {
			*temp = 0x00;
			temp = strrchr(path, '/');
			dbg_parse("search '%s' in '%s', path='%s'", dev->id, temp, path);
			if (strstr(temp, dev->id) != NULL)
				found = 1;
		}
		if (!found)
			continue;
		strfieldcpy(udev->name, dev->name);
		if (dev->mode != 0) {
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
		dbg_parse("found matching id '%s', '%s' becomes '%s'"
			  " - owner='%s', group ='%s', mode=%#o",
			  dev->id, class_dev->name, udev->name,
			  dev->owner, dev->group, dev->mode);
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

		dbg_parse("dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
		if (strcasecmp(dev->bus, sysfs_device->bus) != 0)
			continue;

		found = 0;
		strfieldcpy(path, sysfs_device->path);
		temp = strrchr(path, '/');
		dbg_parse("search '%s' in '%s', path='%s'", dev->place, temp, path);
		if (strstr(temp, dev->place) != NULL) {
			found = 1;
		} else {
			*temp = 0x00;
			temp = strrchr(path, '/');
			dbg_parse("search '%s' in '%s', path='%s'", dev->place, temp, path);
			if (strstr(temp, dev->place) != NULL)
				found = 1;
		}
		if (!found)
			continue;

		strfieldcpy(udev->name, dev->name);
		if (dev->mode != 0) {
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
		dbg_parse("found matching place '%s', '%s' becomes '%s'"
			  " - owner='%s', group ='%s', mode=%#o",
			  dev->place, class_dev->name, udev->name,
			  dev->owner, dev->group, dev->mode);
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

		dbg_parse("compare name '%s' with '%s'",
			  dev->kernel_name, class_dev->name);
		if (strncmp_wildcard(class_dev->name, dev->kernel_name, NAME_SIZE) != 0)
			continue;

		strfieldcpy(udev->name, dev->name);
		if (dev->mode != 0) {
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
		dbg_parse("found name, '%s' becomes '%s' - owner='%s', group='%s', mode = %#o",
			  dev->kernel_name, udev->name, 
			  dev->owner, dev->group, dev->mode);
		
		return 0;
	}
	return -ENODEV;
}

static void do_kernelname(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	struct config_device *dev;
	struct list_head *tmp;
	int len;

	strfieldcpy(udev->name, class_dev->name);
	/* look for permissions */
	list_for_each(tmp, &config_device_list) {
		dev = list_entry(tmp, struct config_device, node);
		len = strlen(dev->name);
		if (strncmp_wildcard(class_dev->name, dev->name, sizeof(dev->name)))
			continue;
		if (dev->mode != 0) {
			dbg_parse("found permissions for '%s'", class_dev->name);
			udev->mode = dev->mode;
			strfieldcpy(udev->owner, dev->owner);
			strfieldcpy(udev->group, dev->group);
		}
	}
}

static int get_attr(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	struct sysfs_device *sysfs_device = NULL;
	struct sysfs_class_device *class_dev_parent = NULL;
	int retval = 0;
	char *temp = NULL;

	udev->mode = 0;

	/* find the sysfs_device for this class device */
	/* Wouldn't it really be nice if libsysfs could do this for us? */
	if (class_dev->sysdevice) {
		sysfs_device = class_dev->sysdevice;
	} else {
		/* bah, let's go backwards up a level to see if the device is there,
		 * as block partitions don't point to the physical device.  Need to fix that
		 * up in the kernel...
		 */
		if (strstr(class_dev->path, "block")) {
			dbg_parse("looking at block device");
			if (isdigit(class_dev->path[strlen(class_dev->path)-1])) {
				char path[SYSFS_PATH_MAX];

				dbg_parse("really is a partition");
				strfieldcpy(path, class_dev->path);
				temp = strrchr(path, '/');
				*temp = 0x00;
				dbg_parse("looking for a class device at '%s'", path);
				class_dev_parent = sysfs_open_class_device(path);
				if (class_dev_parent == NULL) {
					dbg("sysfs_open_class_device at '%s' failed", path);
				} else {
					dbg_parse("class_dev_parent->name='%s'", class_dev_parent->name);
					if (class_dev_parent->sysdevice)
						sysfs_device = class_dev_parent->sysdevice;
				}
			}
		}
	}
		
	if (sysfs_device) {
		dbg_parse("sysfs_device->path='%s'", sysfs_device->path);
		dbg_parse("sysfs_device->bus_id='%s'", sysfs_device->bus_id);
		dbg_parse("sysfs_device->bus='%s'", sysfs_device->bus);
		strfieldcpy(udev->bus_id, sysfs_device->bus_id);
	} else {
		dbg_parse("class_dev->name = '%s'", class_dev->name);
	}

	build_kernel_number(class_dev, udev);

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
	/* substitute placeholder in NAME  */
	apply_format(udev, udev->name);

done:
	/* mode was never set above */
	if (!udev->mode) {
		udev->mode = get_default_mode(class_dev);
		udev->owner[0] = 0x00;
		udev->group[0] = 0x00;
	}

	if (class_dev_parent)
		sysfs_close_class_device(class_dev_parent);

	return 0;
}

int namedev_name_device(struct sysfs_class_device *class_dev, struct udevice *dev)
{
	int retval;

	retval = get_attr(class_dev, dev);
	if (retval)
		dbg("get_attr failed");

	return retval;
}

int namedev_init(void)
{
	int retval;
	
	retval = namedev_init_config();
	if (retval)
		return retval;

	retval = namedev_init_permissions();
	if (retval)
		return retval;

	dump_dev_list();
	return retval;
}
