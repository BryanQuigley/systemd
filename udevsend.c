/*
 * udevsend.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2004 Ling, Xiaofeng <xiaofeng.ling@intel.com>
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
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

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "udev.h"
#include "udevd.h"
#include "logging.h"

static inline char *get_action(void)
{
	char *action;

	action = getenv("ACTION");
	return action;
}

static inline char *get_devpath(void)
{
	char *devpath;

	devpath = getenv("DEVPATH");
	return devpath;
}

static inline char *get_seqnum(void)
{
	char *seqnum;

	seqnum = getenv("SEQNUM");
	return seqnum;
}

static int build_hotplugmsg(struct hotplug_msg **ppmsg, char *action,
			    char *devpath, char *subsystem, int seqnum)
{
	struct hotplug_msg *pmsg;

	pmsg = malloc(sizeof(struct hotplug_msg));
	pmsg->mtype = HOTPLUGMSGTYPE;
	pmsg->seqnum = seqnum;
	strncpy(pmsg->action, action, 8);
	strncpy(pmsg->devpath, devpath, 128);
	strncpy(pmsg->subsystem, subsystem, 16);
	*ppmsg = pmsg;
	return sizeof(struct hotplug_msg);
}

static void free_hotplugmsg(struct hotplug_msg *pmsg)
{
	free(pmsg);
}

int main(int argc, char* argv[])
{
	int msgid;
	key_t key;
	struct msqid_ds  msg_queue;
	struct msgbuf *pmsg;
	char *action;
	char *devpath;
	char *subsystem;
	char *seqnum;
	int seq;
	int retval = -EINVAL;
	int size;

	subsystem = argv[1];
	if (subsystem == NULL) {
		dbg("no subsystem");
		goto exit;
	}

	devpath = get_devpath();
	if (devpath == NULL) {
		dbg("no devpath");
		goto exit;
	}

	action = get_action();
	if (action == NULL) {
		dbg("no action");
		goto exit;
	}

	seqnum = get_seqnum();
	if (seqnum == NULL) {
		dbg("no seqnum");
		goto exit;
	}
	seq = atoi(seqnum);

	/* create ipc message queue or get id of our existing one */
	key = ftok(DEFAULT_EXEC_PROGRAM, IPC_KEY_ID);
	size =  build_hotplugmsg( (struct hotplug_msg**) &pmsg, action, devpath, subsystem, seq);
	msgid = msgget(key, IPC_CREAT);
	if (msgid == -1) {
		dbg("error open ipc queue");
		goto exit;
	}

	/* get state of ipc queue */
	retval = msgctl(msgid, IPC_STAT, &msg_queue);
	if (retval == -1) {
		dbg("error getting info on ipc queue");
		goto exit;
	}
	if (msg_queue.msg_qnum > 0)
		dbg("%li messages already in the ipc queue", msg_queue.msg_qnum);

	/* send ipc message to the daemon */
	retval = msgsnd(msgid, pmsg, size, 0);
	free_hotplugmsg( (struct hotplug_msg*) pmsg);
	if (retval == -1) {
		dbg("error sending ipc message");
		goto exit;
	}
	return 0;

exit:
	if (retval > 0)
		retval = 0;

	return retval;
}
