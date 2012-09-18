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

#include "systemd/sd-id128.h"
#include "unit.h"
#include "specifier.h"
#include "path-util.h"
#include "strv.h"
#include "unit-name.h"
#include "unit-printf.h"

static char *specifier_prefix_and_instance(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        return unit_name_to_prefix_and_instance(u->id);
}

static char *specifier_prefix(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        return unit_name_to_prefix(u->id);
}

static char *specifier_prefix_unescaped(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        char *p, *r;

        assert(u);

        p = unit_name_to_prefix(u->id);
        if (!p)
                return NULL;

        r = unit_name_unescape(p);
        free(p);

        return r;
}

static char *specifier_instance_unescaped(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        if (u->instance)
                return unit_name_unescape(u->instance);

        return strdup("");
}

static char *specifier_filename(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        if (u->instance)
                return unit_name_path_unescape(u->instance);

        return unit_name_to_path(u->id);
}

static char *specifier_cgroup(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        return unit_default_cgroup_path(u);
}

static char *specifier_cgroup_root(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        char *p;
        assert(u);

        if (specifier == 'r')
                return strdup(u->manager->cgroup_hierarchy);

        if (path_get_parent(u->manager->cgroup_hierarchy, &p) < 0)
                return strdup("");

        if (streq(p, "/")) {
                free(p);
                return strdup("");
        }

        return p;
}

static char *specifier_runtime(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        assert(u);

        if (u->manager->running_as == SYSTEMD_USER) {
                const char *e;

                e = getenv("XDG_RUNTIME_DIR");
                if (e)
                        return strdup(e);
        }

        return strdup("/run");
}

static char *specifier_user_name(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        ExecContext *c;
        int r;
        const char *username;

        assert(u);

        c = unit_get_exec_context(u);
        if (!c)
                return NULL;

        /* get USER env from our own env if set */
        if (!c->user)
                return getusername_malloc();

        /* fish username from passwd */
        username = c->user;
        r = get_user_creds(&username, NULL, NULL, NULL, NULL);
        if (r < 0)
                return NULL;

        return strdup(username);
}

static char *specifier_user_home(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        ExecContext *c;
        int r;
        const char *username, *home;

        assert(u);

        c = unit_get_exec_context(u);
        if (!c)
                return NULL;

        /* return HOME if set, otherwise from passwd */
        if (!c->user) {
                char *h;

                r = get_home_dir(&h);
                if (r < 0)
                        return NULL;

                return h;
        }

        username = c->user;
        r = get_user_creds(&username, NULL, NULL, &home, NULL);
        if (r < 0)
               return NULL;

        return strdup(home);
}

static char *specifier_user_shell(char specifier, void *data, void *userdata) {
        Unit *u = userdata;
        ExecContext *c;
        int r;
        const char *username, *shell;

        assert(u);

        c = unit_get_exec_context(u);
        if (!c)
                return NULL;

        /* return HOME if set, otherwise from passwd */
        if (!c->user) {
                char *sh;

                r = get_shell(&sh);
                if (r < 0)
                        return strdup("/bin/sh");

                return sh;
        }

        username = c->user;
        r = get_user_creds(&username, NULL, NULL, NULL, &shell);
        if (r < 0)
                return strdup("/bin/sh");

        return strdup(shell);
}

static char *specifier_machine_id(char specifier, void *data, void *userdata) {
        sd_id128_t id;
        char *buf;
        int r;

        r = sd_id128_get_machine(&id);
        if (r < 0)
                return NULL;

        buf = new(char, 33);
        if (!buf)
                return NULL;

        return sd_id128_to_string(id, buf);
}

static char *specifier_boot_id(char specifier, void *data, void *userdata) {
        sd_id128_t id;
        char *buf;
        int r;

        r = sd_id128_get_boot(&id);
        if (r < 0)
                return NULL;

        buf = new(char, 33);
        if (!buf)
                return NULL;

        return sd_id128_to_string(id, buf);
}

static char *specifier_host_name(char specifier, void *data, void *userdata) {
        return gethostname_malloc();
}

char *unit_name_printf(Unit *u, const char* format) {

        /*
         * This will use the passed string as format string and
         * replace the following specifiers:
         *
         * %n: the full id of the unit                 (foo@bar.waldo)
         * %N: the id of the unit without the suffix   (foo@bar)
         * %p: the prefix                              (foo)
         * %i: the instance                            (bar)
         */

        const Specifier table[] = {
                { 'n', specifier_string,              u->id },
                { 'N', specifier_prefix_and_instance, NULL },
                { 'p', specifier_prefix,              NULL },
                { 'i', specifier_string,              u->instance },
                { 0, NULL, NULL }
        };

        assert(u);
        assert(format);

        return specifier_printf(format, table, u);
}

char *unit_full_printf(Unit *u, const char *format) {

        /* This is similar to unit_name_printf() but also supports
         * unescaping. Also, adds a couple of additional codes:
         *
         * %f the the instance if set, otherwise the id
         * %c cgroup path of unit
         * %r root cgroup path of this systemd instance (e.g. "/user/lennart/shared/systemd-4711")
         * %R parent of root cgroup path (e.g. "/usr/lennart/shared")
         * %t the runtime directory to place sockets in (e.g. "/run" or $XDG_RUNTIME_DIR)
         * %u the username of the configured user or running user
         * %h the homedir of the configured user or running user
         * %s the shell of the configured user or running user
         * %m the machine ID of the running system
         * %b the boot ID of the running system
         * %H the host name of the running system
         */

        const Specifier table[] = {
                { 'n', specifier_string,              u->id },
                { 'N', specifier_prefix_and_instance, NULL },
                { 'p', specifier_prefix,              NULL },
                { 'P', specifier_prefix_unescaped,    NULL },
                { 'i', specifier_string,              u->instance },
                { 'I', specifier_instance_unescaped,  NULL },

                { 'f', specifier_filename,            NULL },
                { 'c', specifier_cgroup,              NULL },
                { 'r', specifier_cgroup_root,         NULL },
                { 'R', specifier_cgroup_root,         NULL },
                { 't', specifier_runtime,             NULL },
                { 'u', specifier_user_name,           NULL },
                { 'h', specifier_user_home,           NULL },
                { 's', specifier_user_shell,          NULL },

                { 'm', specifier_machine_id,          NULL },
                { 'H', specifier_host_name,           NULL },
                { 'b', specifier_boot_id,             NULL },
                { 0, NULL, NULL }
        };

        assert(u);
        assert(format);

        return specifier_printf(format, table, u);
}

char **unit_full_printf_strv(Unit *u, char **l) {
        size_t n;
        char **r, **i, **j;

        /* Applies unit_full_printf to every entry in l */

        assert(u);

        n = strv_length(l);
        r = new(char*, n+1);
        if (!r)
                return NULL;

        for (i = l, j = r; *i; i++, j++) {
                *j = unit_full_printf(u, *i);
                if (!*j)
                        goto fail;
        }

        *j = NULL;
        return r;

fail:
        for (j--; j >= r; j--)
                free(*j);

        free(r);

        return NULL;
}
