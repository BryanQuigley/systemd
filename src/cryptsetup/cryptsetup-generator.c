/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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

#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "log.h"
#include "util.h"
#include "unit-name.h"
#include "mkdir.h"
#include "virt.h"
#include "strv.h"
#include "fileio.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;
static bool arg_read_crypttab = true;

static bool has_option(const char *haystack, const char *needle) {
        const char *f = haystack;
        size_t l;

        assert(needle);

        if (!haystack)
                return false;

        l = strlen(needle);

        while ((f = strstr(f, needle))) {

                if (f > haystack && f[-1] != ',') {
                        f++;
                        continue;
                }

                if (f[l] != 0 && f[l] != ',') {
                        f++;
                        continue;
                }

                return true;
        }

        return false;
}

static int create_disk(
                const char *name,
                const char *device,
                const char *password,
                const char *options) {

        _cleanup_free_ char *p = NULL, *n = NULL, *d = NULL, *u = NULL, *from = NULL, *to = NULL, *e = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        bool noauto, nofail;

        assert(name);
        assert(device);

        noauto = has_option(options, "noauto");
        nofail = has_option(options, "nofail");

        n = unit_name_from_path_instance("systemd-cryptsetup", name, ".service");
        if (!n)
                return log_oom();

        p = strjoin(arg_dest, "/", n, NULL);
        if (!p)
                return log_oom();

        u = fstab_node_to_udev_node(device);
        if (!u)
                return log_oom();

        d = unit_name_from_path(u, ".device");
        if (!d)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f) {
                log_error("Failed to create unit file %s: %m", p);
                return -errno;
        }

        fputs(
                "# Automatically generated by systemd-cryptsetup-generator\n\n"
                "[Unit]\n"
                "Description=Cryptography Setup for %I\n"
                "Documentation=man:systemd-cryptsetup@.service(8) man:crypttab(5)\n"
                "SourcePath=/etc/crypttab\n"
                "Conflicts=umount.target\n"
                "DefaultDependencies=no\n"
                "BindsTo=dev-mapper-%i.device\n"
                "After=systemd-readahead-collect.service systemd-readahead-replay.service\n",
                f);

        if (!nofail)
                fprintf(f,
                        "Before=cryptsetup.target\n");

        if (password) {
                if (streq(password, "/dev/urandom") ||
                    streq(password, "/dev/random") ||
                    streq(password, "/dev/hw_random"))
                        fputs("After=systemd-random-seed-load.service\n", f);
                else if (!streq(password, "-") &&
                         !streq(password, "none"))
                        fprintf(f,
                                "RequiresMountsFor=%s\n",
                                password);
        }

        if (is_device_path(u))
                fprintf(f,
                        "BindsTo=%s\n"
                        "After=%s\n"
                        "Before=umount.target\n",
                        d, d);
        else
                fprintf(f,
                        "RequiresMountsFor=%s\n",
                        u);

        fprintf(f,
                "\n[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n" /* the binary handles timeouts anyway */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '%s' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                name, u, strempty(password), strempty(options),
                name);

        if (has_option(options, "tmp"))
                fprintf(f,
                        "ExecStartPost=/sbin/mke2fs '/dev/mapper/%s'\n",
                        name);

        if (has_option(options, "swap"))
                fprintf(f,
                        "ExecStartPost=/sbin/mkswap '/dev/mapper/%s'\n",
                        name);

        fflush(f);

        if (ferror(f)) {
                log_error("Failed to write file %s: %m", p);
                return -errno;
        }

        if (asprintf(&from, "../%s", n) < 0)
                return log_oom();

        if (!noauto) {

                to = strjoin(arg_dest, "/", d, ".wants/", n, NULL);
                if (!to)
                        return log_oom();

                mkdir_parents_label(to, 0755);
                if (symlink(from, to) < 0) {
                        log_error("Failed to create symlink '%s' to '%s': %m", from, to);
                        return -errno;
                }

                free(to);
                if (!nofail)
                        to = strjoin(arg_dest, "/cryptsetup.target.requires/", n, NULL);
                else
                        to = strjoin(arg_dest, "/cryptsetup.target.wants/", n, NULL);
                if (!to)
                        return log_oom();

                mkdir_parents_label(to, 0755);
                if (symlink(from, to) < 0) {
                        log_error("Failed to create symlink '%s' to '%s': %m", from, to);
                        return -errno;
                }
        }

        e = unit_name_escape(name);
        if (!e)
                return log_oom();

        free(to);
        to = strjoin(arg_dest, "/dev-mapper-", e, ".device.requires/", n, NULL);
        if (!to)
                return log_oom();

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0) {
                log_error("Failed to create symlink '%s' to '%s': %m", from, to);
                return -errno;
        }

        if (!noauto && !nofail) {
                int r;
                free(p);
                p = strjoin(arg_dest, "/dev-mapper-", e, ".device.d/50-job-timeout-sec-0.conf", NULL);
                if (!p)
                        return log_oom();

                mkdir_parents_label(p, 0755);

                r = write_string_file(p,
                                "# Automatically generated by systemd-cryptsetup-generator\n\n"
                                "[Unit]\n"
                                "JobTimeoutSec=0\n"); /* the binary handles timeouts anyway */
                if (r)
                        return r;
        }

        return 0;
}

static int parse_proc_cmdline(char ***arg_proc_cmdline_disks, char **arg_proc_cmdline_keyfile) {
        _cleanup_free_ char *line = NULL;
        char *w = NULL, *state = NULL;
        int r;
        size_t l;

        if (detect_container(NULL) > 0)
                return 0;

        r = read_one_line_file("/proc/cmdline", &line);
        if (r < 0) {
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, line, state) {
                _cleanup_free_ char *word = NULL;

                word = strndup(w, l);
                if (!word)
                        return log_oom();

                if (startswith(word, "luks=")) {
                        r = parse_boolean(word + 5);
                        if (r < 0)
                                log_warning("Failed to parse luks switch %s. Ignoring.", word + 5);
                        else
                                arg_enabled = r;

                } else if (startswith(word, "rd.luks=")) {

                        if (in_initrd()) {
                                r = parse_boolean(word + 8);
                                if (r < 0)
                                        log_warning("Failed to parse luks switch %s. Ignoring.", word + 8);
                                else
                                        arg_enabled = r;
                        }

                } else if (startswith(word, "luks.crypttab=")) {
                        r = parse_boolean(word + 14);
                        if (r < 0)
                                log_warning("Failed to parse luks crypttab switch %s. Ignoring.", word + 14);
                        else
                                arg_read_crypttab = r;

                } else if (startswith(word, "rd.luks.crypttab=")) {

                        if (in_initrd()) {
                                r = parse_boolean(word + 17);
                                if (r < 0)
                                        log_warning("Failed to parse luks crypttab switch %s. Ignoring.", word + 17);
                                else
                                        arg_read_crypttab = r;
                        }

                } else if (startswith(word, "luks.uuid=")) {
                        if (strv_extend(arg_proc_cmdline_disks, word + 10) < 0)
                                return log_oom();

                } else if (startswith(word, "rd.luks.uuid=")) {

                        if (in_initrd()) {
                                if (strv_extend(arg_proc_cmdline_disks, word + 13) < 0)
                                        return log_oom();
                        }

                } else if (startswith(word, "luks.key=")) {
                        *arg_proc_cmdline_keyfile = strdup(word + 9);
                        if (!*arg_proc_cmdline_keyfile)
                                return log_oom();

                } else if (startswith(word, "rd.luks.key=")) {

                        if (in_initrd()) {
                                if (*arg_proc_cmdline_keyfile)
                                        free(*arg_proc_cmdline_keyfile);
                                *arg_proc_cmdline_keyfile = strdup(word + 12);
                                if (!*arg_proc_cmdline_keyfile)
                                        return log_oom();
                        }

                } else if (startswith(word, "luks.") ||
                           (in_initrd() && startswith(word, "rd.luks."))) {

                        log_warning("Unknown kernel switch %s. Ignoring.", word);
                }
        }

        strv_uniq(*arg_proc_cmdline_disks);

        return 0;
}

int main(int argc, char *argv[]) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned n = 0;
        int r = EXIT_SUCCESS;
        char **i;
        _cleanup_strv_free_ char **arg_proc_cmdline_disks_done = NULL;
        _cleanup_strv_free_ char **arg_proc_cmdline_disks = NULL;
        _cleanup_free_ char *arg_proc_cmdline_keyfile = NULL;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (parse_proc_cmdline(&arg_proc_cmdline_disks, &arg_proc_cmdline_keyfile) < 0)
                return EXIT_FAILURE;

        if (!arg_enabled)
                return EXIT_SUCCESS;

        if (arg_read_crypttab) {
                f = fopen("/etc/crypttab", "re");

                if (!f) {
                        if (errno == ENOENT)
                                r = EXIT_SUCCESS;
                        else {
                                r = EXIT_FAILURE;
                                log_error("Failed to open /etc/crypttab: %m");
                        }
                } else for (;;) {
                        char line[LINE_MAX], *l;
                        _cleanup_free_ char *name = NULL, *device = NULL, *password = NULL, *options = NULL;
                        int k;

                        if (!fgets(line, sizeof(line), f))
                                break;

                        n++;

                        l = strstrip(line);
                        if (*l == '#' || *l == 0)
                                continue;

                        k = sscanf(l, "%ms %ms %ms %ms", &name, &device, &password, &options);
                        if (k < 2 || k > 4) {
                                log_error("Failed to parse /etc/crypttab:%u, ignoring.", n);
                                r = EXIT_FAILURE;
                                continue;
                        }

                        if (arg_proc_cmdline_disks) {
                                /*
                                  If luks UUIDs are specified on the kernel command line, use them as a filter
                                  for /etc/crypttab and only generate units for those.
                                */
                                STRV_FOREACH(i, arg_proc_cmdline_disks) {
                                        _cleanup_free_ char *proc_device = NULL, *proc_name = NULL;
                                        const char *p = *i;

                                        if (startswith(p, "luks-"))
                                                p += 5;

                                        proc_name = strappend("luks-", p);
                                        proc_device = strappend("UUID=", p);

                                        if (!proc_name || !proc_device)
                                                return log_oom();

                                        if (streq(proc_device, device) || streq(proc_name, name)) {
                                                if (create_disk(name, device, password, options) < 0)
                                                        r = EXIT_FAILURE;

                                                if (strv_extend(&arg_proc_cmdline_disks_done, p) < 0)
                                                        return log_oom();
                                        }
                                }
                        } else {
                                if (create_disk(name, device, password, options) < 0)
                                        r = EXIT_FAILURE;
                        }
                }
        }

        STRV_FOREACH(i, arg_proc_cmdline_disks) {
                /*
                  Generate units for those UUIDs, which were specified
                  on the kernel command line and not yet written.
                */

                _cleanup_free_ char *name = NULL, *device = NULL;
                const char *p = *i;

                if (startswith(p, "luks-"))
                        p += 5;

                if (strv_contains(arg_proc_cmdline_disks_done, p))
                        continue;

                name = strappend("luks-", p);
                device = strappend("UUID=", p);

                if (!name || !device)
                        return log_oom();

                if (create_disk(name, device, arg_proc_cmdline_keyfile, "timeout=0") < 0)
                        r = EXIT_FAILURE;
        }

        return r;
}
