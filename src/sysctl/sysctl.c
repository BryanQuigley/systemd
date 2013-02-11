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

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <getopt.h>

#include "log.h"
#include "strv.h"
#include "util.h"
#include "strv.h"
#include "hashmap.h"
#include "path-util.h"
#include "conf-files.h"

static char **arg_prefixes = NULL;

static const char conf_file_dirs[] =
        "/etc/sysctl.d\0"
        "/run/sysctl.d\0"
        "/usr/local/lib/sysctl.d\0"
        "/usr/lib/sysctl.d\0"
#ifdef HAVE_SPLIT_USR
        "/lib/sysctl.d\0"
#endif
        ;

static char *normalize_sysctl(char *s) {
        char *n;

        for (n = s; *n; n++)
                if (*n == '.')
                        *n = '/';

        return s;
}

static int apply_sysctl(const char *property, const char *value) {
        _cleanup_free_ char *p = NULL;
        char *n;
        int r = 0, k;

        log_debug("Setting '%s' to '%s'", property, value);

        p = new(char, sizeof("/proc/sys/") + strlen(property));
        if (!p)
                return log_oom();

        n = stpcpy(p, "/proc/sys/");
        strcpy(n, property);

        if (!strv_isempty(arg_prefixes)) {
                char **i;
                bool good = false;

                STRV_FOREACH(i, arg_prefixes)
                        if (path_startswith(p, *i)) {
                                good = true;
                                break;
                        }

                if (!good) {
                        log_debug("Skipping %s", p);
                        return 0;
                }
        }

        k = write_one_line_file(p, value);
        if (k < 0) {
                log_full(k == -ENOENT ? LOG_DEBUG : LOG_WARNING,
                         "Failed to write '%s' to '%s': %s", value, p, strerror(-k));

                if (k != -ENOENT && r == 0)
                        r = k;
        }

        return r;
}

static int apply_all(Hashmap *sysctl_options) {
        int r = 0;
        char *property, *value;
        Iterator i;

        assert(sysctl_options);

        HASHMAP_FOREACH_KEY(value, property, sysctl_options, i) {
                int k;

                k = apply_sysctl(property, value);
                if (k < 0 && r == 0)
                        r = k;
        }
        return r;
}

static int parse_file(Hashmap *sysctl_options, const char *path, bool ignore_enoent) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        r = search_and_fopen_nulstr(path, "re", conf_file_dirs, &f);
        if (r < 0) {
                if (ignore_enoent && errno == -ENOENT)
                        return 0;

                log_error("Failed to open file '%s', ignoring: %s", path, strerror(-r));
                return r;
        }

        log_debug("parse: %s\n", path);
        while (!feof(f)) {
                char l[LINE_MAX], *p, *value, *new_value, *property, *existing;
                int k;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        log_error("Failed to read file '%s', ignoring: %m", path);
                        return -errno;
                }

                p = strstrip(l);
                if (!*p)
                        continue;

                if (strchr(COMMENTS, *p))
                        continue;

                value = strchr(p, '=');
                if (!value) {
                        log_error("Line is not an assignment in file '%s': %s", path, value);

                        if (r == 0)
                                r = -EINVAL;
                        continue;
                }

                *value = 0;
                value++;

                p = normalize_sysctl(strstrip(p));
                value = strstrip(value);

                existing = hashmap_get(sysctl_options, p);
                if (existing) {
                        if (!streq(value, existing))
                                log_warning("Two ore more conflicting assignments of %s, ignoring.", property);

                        continue;
                }

                property = strdup(p);
                if (!property)
                        return log_oom();

                new_value = strdup(value);
                if (!new_value) {
                        free(property);
                        return log_oom();
                }

                k = hashmap_put(sysctl_options, property, new_value);
                if (k < 0) {
                        log_error("Failed to add sysctl variable %s to hashmap: %s", property, strerror(-r));
                        free(property);
                        free(new_value);
                        return k;
                }
        }

        return r;
}

static int help(void) {

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Applies kernel sysctl settings.\n\n"
               "  -h --help             Show this help\n"
               "     --prefix=PATH      Only apply rules that apply to paths with the specified prefix\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_PREFIX
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "prefix",    required_argument, NULL, ARG_PREFIX    },
                { NULL,        0,                 NULL, 0             }
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_PREFIX: {
                        char *p;
                        char **l;

                        for (p = optarg; *p; p++)
                                if (*p == '.')
                                        *p = '/';

                        l = strv_append(arg_prefixes, optarg);
                        if (!l)
                                return log_oom();

                        strv_free(arg_prefixes);
                        arg_prefixes = l;

                        break;
                }

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        return 1;
}

int main(int argc, char *argv[]) {
        int r = 0, k;
        Hashmap *sysctl_options;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        sysctl_options = hashmap_new(string_hash_func, string_compare_func);
        if (!sysctl_options) {
                r = log_oom();
                goto finish;
        }

        r = 0;

        if (argc > optind) {
                int i;

                for (i = optind; i < argc; i++) {
                        k = parse_file(sysctl_options, argv[i], false);
                        if (k < 0 && r == 0)
                                r = k;
                }
        } else {
                _cleanup_strv_free_ char **files = NULL;
                char **f;

                r = conf_files_list_nulstr(&files, ".conf", NULL, conf_file_dirs);
                if (r < 0) {
                        log_error("Failed to enumerate sysctl.d files: %s", strerror(-r));
                        goto finish;
                }

                r = parse_file(sysctl_options, "/etc/sysctl.conf", true);

                STRV_FOREACH(f, files) {
                        k = parse_file(sysctl_options, *f, true);
                        if (k < 0 && r == 0)
                                r = k;
                }
        }

        k = apply_all(sysctl_options);
        if (k < 0 && r == 0)
                r = k;

finish:
        hashmap_free_free_free(sysctl_options);
        strv_free(arg_prefixes);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
