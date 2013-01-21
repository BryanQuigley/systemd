/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <stdlib.h>

#include "efivars.h"
#include "path-util.h"
#include "util.h"
#include "mkdir.h"

static const char *arg_dest = "/tmp";

int main(int argc, char *argv[]) {
        int r = EXIT_SUCCESS;
        sd_id128_t id;
        _cleanup_free_ char *name = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (!is_efiboot())
                return EXIT_SUCCESS;

        if (dir_is_empty("/boot") <= 0)
                return EXIT_SUCCESS;

        r = efi_get_loader_device_part_uuid(&id);
        if (r == -ENOENT)
                return EXIT_SUCCESS;
        if (r < 0) {
                log_error("Failed to read ESP partition UUID: %s", strerror(-r));
                return EXIT_FAILURE;
        }

        name = strjoin(arg_dest, "/boot.mount", NULL);
        if (!name) {
                log_oom();
                return EXIT_FAILURE;
        }

        f = fopen(name, "wxe");
        if (!f) {
                log_error("Failed to create mount unit file %s: %m", name);
                return EXIT_FAILURE;
        }

        fprintf(f,
                "# Automatially generated by systemd-efi-boot-generator\n\n"
                "[Unit]\n"
                "Description=EFI System Partition\n\n"
                "[Mount]\n"
                "Where=/boot\n"
                "What=/dev/disk/by-partuuid/%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n"
                "Options=umask=0077\n",
                SD_ID128_FORMAT_VAL(id));

        free(name);
        name = strjoin(arg_dest, "/boot.automount", NULL);
        if (!name) {
                log_oom();
                return EXIT_FAILURE;
        }

        fclose(f);
        f = fopen(name, "wxe");
        if (!f) {
                log_error("Failed to create automount unit file %s: %m", name);
                return EXIT_FAILURE;
        }

        fputs("# Automatially generated by systemd-efi-boot-generator\n\n"
              "[Unit]\n"
              "Description=EFI System Partition Automount\n\n"
              "[Automount]\n"
              "Where=/boot\n", f);

        free(name);
        name = strjoin(arg_dest, "/local-fs.target.wants/boot.automount", NULL);
        if (!name) {
                log_oom();
                return EXIT_FAILURE;
        }

        mkdir_parents(name, 0755);

        if (symlink("../boot.automount", name) < 0) {
                log_error("Failed to create symlink: %m");
                return EXIT_FAILURE;
        }

        return 0;
}
