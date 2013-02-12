/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering
  Copyright 2012 Holger Hans Peter Freyther

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

#include <linux/oom.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "unit.h"
#include "strv.h"
#include "conf-parser.h"
#include "load-fragment.h"
#include "log.h"
#include "ioprio.h"
#include "securebits.h"
#include "missing.h"
#include "unit-name.h"
#include "unit-printf.h"
#include "bus-errors.h"
#include "utf8.h"
#include "path-util.h"
#include "syscall-list.h"
#include "env-util.h"

#ifndef HAVE_SYSV_COMPAT
int config_parse_warn_compat(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        log_debug("[%s:%u] Support for option %s= has been disabled at compile time and is ignored", filename, line, lvalue);
        return 0;
}
#endif

int config_parse_unit_deps(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        UnitDependency d = ltype;
        Unit *u = userdata;
        char *w;
        size_t l;
        char *state;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char _cleanup_free_ *t = NULL, *k = NULL;
                int r;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                k = unit_name_printf(u, t);
                if (!k)
                        return log_oom();

                r = unit_add_dependency_by_name(u, d, k, NULL, true);
                if (r < 0)
                        log_error("[%s:%u] Failed to add dependency on %s, ignoring: %s",
                                  filename, line, k, strerror(-r));
        }

        return 0;
}

int config_parse_unit_string_printf(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        _cleanup_free_ char *k = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        k = unit_full_printf(u, rvalue);
        if (!k)
                return log_oom();

        return config_parse_string(filename, line, section, lvalue, ltype, k, data, userdata);
}

int config_parse_unit_strv_printf(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        _cleanup_free_ char *k = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        k = unit_full_printf(u, rvalue);
        if (!k)
                return log_oom();

        return config_parse_strv(filename, line, section, lvalue, ltype, k, data, userdata);
}

int config_parse_unit_path_printf(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        _cleanup_free_ char *k = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        k = unit_full_printf(u, rvalue);
        if (!k)
                return log_oom();

        return config_parse_path(filename, line, section, lvalue, ltype, k, data, userdata);
}

int config_parse_socket_listen(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        SocketPort *p, *tail;
        Socket *s;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        s = SOCKET(data);

        if (isempty(rvalue)) {
                /* An empty assignment removes all ports */
                socket_free_ports(s);
                return 0;
        }

        p = new0(SocketPort, 1);
        if (!p)
                return log_oom();

        if (ltype != SOCKET_SOCKET) {

                p->type = ltype;
                p->path = unit_full_printf(UNIT(s), rvalue);
                if (!p->path) {
                        free(p);
                        return log_oom();
                }

                path_kill_slashes(p->path);

        } else if (streq(lvalue, "ListenNetlink")) {
                _cleanup_free_ char  *k = NULL;
                int r;

                p->type = SOCKET_SOCKET;
                k = unit_full_printf(UNIT(s), rvalue);
                if (!k) {
                        free(p);
                        return log_oom();
                }

                r = socket_address_parse_netlink(&p->address, k);
                if (r < 0) {
                        log_error("[%s:%u] Failed to parse address value, ignoring: %s", filename, line, rvalue);
                        free(p);
                        return 0;
                }

        } else {
                _cleanup_free_ char *k = NULL;
                int r;

                p->type = SOCKET_SOCKET;
                k = unit_full_printf(UNIT(s), rvalue);
                if (!k) {
                        free(p);
                        return log_oom();
                }

                r = socket_address_parse(&p->address, k);
                if (r < 0) {
                        log_error("[%s:%u] Failed to parse address value, ignoring: %s", filename, line, rvalue);
                        free(p);
                        return 0;
                }

                if (streq(lvalue, "ListenStream"))
                        p->address.type = SOCK_STREAM;
                else if (streq(lvalue, "ListenDatagram"))
                        p->address.type = SOCK_DGRAM;
                else {
                        assert(streq(lvalue, "ListenSequentialPacket"));
                        p->address.type = SOCK_SEQPACKET;
                }

                if (socket_address_family(&p->address) != AF_LOCAL && p->address.type == SOCK_SEQPACKET) {
                        log_error("[%s:%u] Address family not supported, ignoring: %s", filename, line, rvalue);
                        free(p);
                        return 0;
                }
        }

        p->fd = -1;

        if (s->ports) {
                LIST_FIND_TAIL(SocketPort, port, s->ports, tail);
                LIST_INSERT_AFTER(SocketPort, port, s->ports, tail, p);
        } else
                LIST_PREPEND(SocketPort, port, s->ports, p);

        return 0;
}

int config_parse_socket_bind(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Socket *s;
        SocketAddressBindIPv6Only b;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        s = SOCKET(data);

        b = socket_address_bind_ipv6_only_from_string(rvalue);
        if (b < 0) {
                int r;

                r = parse_boolean(rvalue);
                if (r < 0) {
                        log_error("[%s:%u] Failed to parse bind IPv6 only value, ignoring: %s", filename, line, rvalue);
                        return 0;
                }

                s->bind_ipv6_only = r ? SOCKET_ADDRESS_IPV6_ONLY : SOCKET_ADDRESS_BOTH;
        } else
                s->bind_ipv6_only = b;

        return 0;
}

int config_parse_exec_nice(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        int priority;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &priority) < 0) {
                log_error("[%s:%u] Failed to parse nice priority, ignoring: %s. ", filename, line, rvalue);
                return 0;
        }

        if (priority < PRIO_MIN || priority >= PRIO_MAX) {
                log_error("[%s:%u] Nice priority out of range, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->nice = priority;
        c->nice_set = true;

        return 0;
}

int config_parse_exec_oom_score_adjust(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        int oa;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &oa) < 0) {
                log_error("[%s:%u] Failed to parse the OOM score adjust value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (oa < OOM_SCORE_ADJ_MIN || oa > OOM_SCORE_ADJ_MAX) {
                log_error("[%s:%u] OOM score adjust value out of range, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->oom_score_adjust = oa;
        c->oom_score_adjust_set = true;

        return 0;
}

int config_parse_exec(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecCommand **e = data, *nce;
        char *path, **n;
        unsigned k;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(e);

        e += ltype;

        if (isempty(rvalue)) {
                /* An empty assignment resets the list */
                exec_command_free_list(*e);
                *e = NULL;
                return 0;
        }

        /* We accept an absolute path as first argument, or
         * alternatively an absolute prefixed with @ to allow
         * overriding of argv[0]. */
        for (;;) {
                int i;
                char *w;
                size_t l;
                char *state;
                bool honour_argv0 = false, ignore = false;

                path = NULL;
                nce = NULL;
                n = NULL;

                rvalue += strspn(rvalue, WHITESPACE);

                if (rvalue[0] == 0)
                        break;

                for (i = 0; i < 2; i++) {
                        if (rvalue[0] == '-' && !ignore) {
                                ignore = true;
                                rvalue ++;
                        }

                        if (rvalue[0] == '@' && !honour_argv0) {
                                honour_argv0 = true;
                                rvalue ++;
                        }
                }

                if (*rvalue != '/') {
                        log_error("[%s:%u] Executable path is not absolute, ignoring: %s",
                                  filename, line, rvalue);
                        return 0;
                }

                k = 0;
                FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                        if (strneq(w, ";", MAX(l, 1U)))
                                break;

                        k++;
                }

                n = new(char*, k + !honour_argv0);
                if (!n)
                        return log_oom();

                k = 0;
                FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                        if (strneq(w, ";", MAX(l, 1U)))
                                break;
                        else if (strneq(w, "\\;", MAX(l, 1U)))
                                w ++;

                        if (honour_argv0 && w == rvalue) {
                                assert(!path);

                                path = strndup(w, l);
                                if (!path) {
                                        r = log_oom();
                                        goto fail;
                                }

                                if (!utf8_is_valid(path)) {
                                        log_error("[%s:%u] Path is not UTF-8 clean, ignoring assignment: %s", filename, line, rvalue);
                                        r = 0;
                                        goto fail;
                                }

                        } else {
                                char *c;

                                c = n[k++] = cunescape_length(w, l);
                                if (!c) {
                                        r = log_oom();
                                        goto fail;
                                }

                                if (!utf8_is_valid(c)) {
                                        log_error("[%s:%u] Path is not UTF-8 clean, ignoring assignment: %s", filename, line, rvalue);
                                        r = 0;
                                        goto fail;
                                }
                        }
                }

                n[k] = NULL;

                if (!n[0]) {
                        log_error("[%s:%u] Invalid command line, ignoring: %s", filename, line, rvalue);
                        r = 0;
                        goto fail;
                }

                if (!path) {
                        path = strdup(n[0]);
                        if (!path) {
                                r = log_oom();
                                goto fail;
                        }
                }

                assert(path_is_absolute(path));

                nce = new0(ExecCommand, 1);
                if (!nce) {
                        r = log_oom();
                        goto fail;
                }

                nce->argv = n;
                nce->path = path;
                nce->ignore = ignore;

                path_kill_slashes(nce->path);

                exec_command_append_list(e, nce);

                rvalue = state;
        }

        return 0;

fail:
        n[k] = NULL;
        strv_free(n);
        free(path);
        free(nce);

        return r;
}

DEFINE_CONFIG_PARSE_ENUM(config_parse_service_type, service_type, ServiceType, "Failed to parse service type");
DEFINE_CONFIG_PARSE_ENUM(config_parse_service_restart, service_restart, ServiceRestart, "Failed to parse service restart specifier");

int config_parse_socket_bindtodevice(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Socket *s = data;
        char *n;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (rvalue[0] && !streq(rvalue, "*")) {
                n = strdup(rvalue);
                if (!n)
                        return log_oom();
        } else
                n = NULL;

        free(s->bind_to_device);
        s->bind_to_device = n;

        return 0;
}

DEFINE_CONFIG_PARSE_ENUM(config_parse_output, exec_output, ExecOutput, "Failed to parse output specifier");
DEFINE_CONFIG_PARSE_ENUM(config_parse_input, exec_input, ExecInput, "Failed to parse input specifier");

int config_parse_exec_io_class(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        int x;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        x = ioprio_class_from_string(rvalue);
        if (x < 0) {
                log_error("[%s:%u] Failed to parse IO scheduling class, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->ioprio = IOPRIO_PRIO_VALUE(x, IOPRIO_PRIO_DATA(c->ioprio));
        c->ioprio_set = true;

        return 0;
}

int config_parse_exec_io_priority(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        int i;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &i) < 0 || i < 0 || i >= IOPRIO_BE_NR) {
                log_error("[%s:%u] Failed to parse io priority, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->ioprio = IOPRIO_PRIO_VALUE(IOPRIO_PRIO_CLASS(c->ioprio), i);
        c->ioprio_set = true;

        return 0;
}

int config_parse_exec_cpu_sched_policy(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {


        ExecContext *c = data;
        int x;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        x = sched_policy_from_string(rvalue);
        if (x < 0) {
                log_error("[%s:%u] Failed to parse CPU scheduling policy, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->cpu_sched_policy = x;
        /* Moving to or from real-time policy? We need to adjust the priority */
        c->cpu_sched_priority = CLAMP(c->cpu_sched_priority, sched_get_priority_min(x), sched_get_priority_max(x));
        c->cpu_sched_set = true;

        return 0;
}

int config_parse_exec_cpu_sched_prio(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        int i, min, max;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &i) < 0) {
                log_error("[%s:%u] Failed to parse CPU scheduling priority, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        /* On Linux RR/FIFO range from 1 to 99 and OTHER/BATCH may only be 0 */
        min = sched_get_priority_min(c->cpu_sched_policy);
        max = sched_get_priority_max(c->cpu_sched_policy);

        if (i < min || i > max) {
                log_error("[%s:%u] CPU scheduling priority is out of range, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        c->cpu_sched_priority = i;
        c->cpu_sched_set = true;

        return 0;
}

int config_parse_exec_cpu_affinity(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        char *w;
        size_t l;
        char *state;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* An empty assignment resets the CPU list */
                if (c->cpuset)
                        CPU_FREE(c->cpuset);
                c->cpuset = NULL;
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char _cleanup_free_ *t = NULL;
                int r;
                unsigned cpu;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                r = safe_atou(t, &cpu);

                if (!c->cpuset) {
                        c->cpuset = cpu_set_malloc(&c->cpuset_ncpus);
                        if (!c->cpuset)
                                return log_oom();
                }

                if (r < 0 || cpu >= c->cpuset_ncpus) {
                        log_error("[%s:%u] Failed to parse CPU affinity %s, ignoring: %s",
                                  filename, line, t, rvalue);
                        return 0;
                }

                CPU_SET_S(cpu, CPU_ALLOC_SIZE(c->cpuset_ncpus), c->cpuset);
        }

        return 0;
}

int config_parse_exec_capabilities(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        cap_t cap;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        cap = cap_from_text(rvalue);
        if (!cap) {
                if (errno == ENOMEM)
                        return log_oom();

                log_error("[%s:%u] Failed to parse capabilities, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (c->capabilities)
                cap_free(c->capabilities);
        c->capabilities = cap;

        return 0;
}

int config_parse_exec_secure_bits(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        char *w;
        size_t l;
        char *state;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* An empty assignment resets the field */
                c->secure_bits = 0;
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                if (first_word(w, "keep-caps"))
                        c->secure_bits |= SECURE_KEEP_CAPS;
                else if (first_word(w, "keep-caps-locked"))
                        c->secure_bits |= SECURE_KEEP_CAPS_LOCKED;
                else if (first_word(w, "no-setuid-fixup"))
                        c->secure_bits |= SECURE_NO_SETUID_FIXUP;
                else if (first_word(w, "no-setuid-fixup-locked"))
                        c->secure_bits |= SECURE_NO_SETUID_FIXUP_LOCKED;
                else if (first_word(w, "noroot"))
                        c->secure_bits |= SECURE_NOROOT;
                else if (first_word(w, "noroot-locked"))
                        c->secure_bits |= SECURE_NOROOT_LOCKED;
                else {
                        log_error("[%s:%u] Failed to parse secure bits, ignoring: %s",
                                  filename, line, rvalue);
                        return 0;
                }
        }

        return 0;
}

int config_parse_bounding_set(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint64_t *capability_bounding_set_drop = data;
        char *w;
        size_t l;
        char *state;
        bool invert = false;
        uint64_t sum = 0;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* An empty assignment resets */
                *capability_bounding_set_drop = 0;
                return 0;
        }

        if (rvalue[0] == '~') {
                invert = true;
                rvalue++;
        }

        /* Note that we store this inverted internally, since the
         * kernel wants it like this. But we actually expose it
         * non-inverted everywhere to have a fully normalized
         * interface. */

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char _cleanup_free_ *t = NULL;
                int r;
                cap_value_t cap;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                r = cap_from_name(t, &cap);
                if (r < 0) {
                        log_error("[%s:%u] Failed to parse capability in bounding set, ignoring: %s",
                                  filename, line, t);
                        continue;
                }

                sum |= ((uint64_t) 1ULL) << (uint64_t) cap;
        }

        if (invert)
                *capability_bounding_set_drop |= sum;
        else
                *capability_bounding_set_drop |= ~sum;

        return 0;
}

int config_parse_limit(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        struct rlimit **rl = data;
        unsigned long long u;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        rl += ltype;

        if (streq(rvalue, "infinity"))
                u = (unsigned long long) RLIM_INFINITY;
        else if (safe_atollu(rvalue, &u) < 0) {
                log_error("[%s:%u] Failed to parse resource value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (!*rl) {
                *rl = new(struct rlimit, 1);
                if (!*rl)
                        return log_oom();
        }

        (*rl)->rlim_cur = (*rl)->rlim_max = (rlim_t) u;
        return 0;
}

int config_parse_unit_cgroup(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        char *w;
        size_t l;
        char *state;

        if (isempty(rvalue)) {
                /* An empty assignment resets the list */
                cgroup_bonding_free_list(u->cgroup_bondings, false);
                u->cgroup_bondings = NULL;
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char _cleanup_free_ *t = NULL, *k = NULL, *ku = NULL;
                int r;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                k = unit_full_printf(u, t);
                if (!k)
                        return log_oom();

                ku = cunescape(k);
                if (!ku)
                        return log_oom();

                r = unit_add_cgroup_from_text(u, ku, true, NULL);
                if (r < 0) {
                        log_error("[%s:%u] Failed to parse cgroup value %s, ignoring: %s",
                                  filename, line, k, rvalue);
                        return 0;
                }
        }

        return 0;
}

#ifdef HAVE_SYSV_COMPAT
int config_parse_sysv_priority(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        int *priority = data;
        int i;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &i) < 0 || i < 0) {
                log_error("[%s:%u] Failed to parse SysV start priority, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        *priority = (int) i;
        return 0;
}
#endif

int config_parse_fsck_passno(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        int *passno = data;
        int i;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atoi(rvalue, &i) || i < 0) {
                log_error("[%s:%u] Failed to parse fsck pass number, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        *passno = (int) i;
        return 0;
}

DEFINE_CONFIG_PARSE_ENUM(config_parse_kill_mode, kill_mode, KillMode, "Failed to parse kill mode");

int config_parse_kill_signal(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        int *sig = data;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(sig);

        r = signal_from_string_try_harder(rvalue);
        if (r <= 0) {
                log_error("[%s:%u] Failed to parse kill signal, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        *sig = r;
        return 0;
}

int config_parse_exec_mount_flags(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        char *w;
        size_t l;
        char *state;
        unsigned long flags = 0;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        FOREACH_WORD_SEPARATOR(w, l, rvalue, ", ", state) {
                char _cleanup_free_ *t;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                if (streq(t, "shared"))
                        flags |= MS_SHARED;
                else if (streq(t, "slave"))
                        flags |= MS_SLAVE;
                else if (streq(w, "private"))
                        flags |= MS_PRIVATE;
                else {
                        log_error("[%s:%u] Failed to parse mount flag %s, ignoring: %s",
                                  filename, line, t, rvalue);
                        return 0;
                }
        }

        c->mount_flags = flags;
        return 0;
}

int config_parse_timer(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Timer *t = data;
        usec_t u = 0;
        TimerValue *v;
        TimerBase b;
        CalendarSpec *c = NULL;
        clockid_t id;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets list */
                timer_free_values(t);
                return 0;
        }

        b = timer_base_from_string(lvalue);
        if (b < 0) {
                log_error("[%s:%u] Failed to parse timer base, ignoring: %s", filename, line, lvalue);
                return 0;
        }

        if (b == TIMER_CALENDAR) {
                if (calendar_spec_from_string(rvalue, &c) < 0) {
                        log_error("[%s:%u] Failed to parse calendar specification, ignoring: %s", filename, line, rvalue);
                        return 0;
                }

                id = CLOCK_REALTIME;
        } else {
                if (parse_usec(rvalue, &u) < 0) {
                        log_error("[%s:%u] Failed to parse timer value, ignoring: %s", filename, line, rvalue);
                        return 0;
                }

                id = CLOCK_MONOTONIC;
        }

        v = new0(TimerValue, 1);
        if (!v)
                return log_oom();

        v->base = b;
        v->clock_id = id;
        v->value = u;
        v->calendar_spec = c;

        LIST_PREPEND(TimerValue, value, t->values, v);

        return 0;
}

int config_parse_timer_unit(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Timer *t = data;
        int r;
        DBusError error;
        Unit *u;
        _cleanup_free_ char *p = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        dbus_error_init(&error);

        p = unit_name_printf(UNIT(t), rvalue);
        if (!p)
                return log_oom();

        if (endswith(p, ".timer")) {
                log_error("[%s:%u] Unit cannot be of type timer, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        r = manager_load_unit(UNIT(t)->manager, p, NULL, NULL, &u);
        if (r < 0) {
                log_error("[%s:%u] Failed to load unit %s, ignoring: %s", filename, line, rvalue, bus_error(&error, r));
                dbus_error_free(&error);
                return 0;
        }

        unit_ref_set(&t->unit, u);

        return 0;
}

int config_parse_path_spec(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Path *p = data;
        PathSpec *s;
        PathType b;
        char *k;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment clears list */
                path_free_specs(p);
                return 0;
        }

        b = path_type_from_string(lvalue);
        if (b < 0) {
                log_error("[%s:%u] Failed to parse path type, ignoring: %s", filename, line, lvalue);
                return 0;
        }

        k = unit_full_printf(UNIT(p), rvalue);
        if (!k)
                return log_oom();

        if (!path_is_absolute(k)) {
                log_error("[%s:%u] Path is not absolute, ignoring: %s", filename, line, k);
                free(k);
                return 0;
        }

        s = new0(PathSpec, 1);
        if (!s) {
                free(k);
                return log_oom();
        }

        s->path = path_kill_slashes(k);
        s->type = b;
        s->inotify_fd = -1;

        LIST_PREPEND(PathSpec, spec, p->specs, s);

        return 0;
}

int config_parse_path_unit(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Path *t = data;
        int r;
        DBusError error;
        Unit *u;
        _cleanup_free_ char *p = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        dbus_error_init(&error);

        p = unit_name_printf(UNIT(t), rvalue);
        if (!p)
                return log_oom();

        if (endswith(p, ".path")) {
                log_error("[%s:%u] Unit cannot be of type path, ignoring: %s", filename, line, p);
                return 0;
        }

        r = manager_load_unit(UNIT(t)->manager, p, NULL, &error, &u);
        if (r < 0) {
                log_error("[%s:%u] Failed to load unit %s, ignoring: %s", filename, line, p, bus_error(&error, r));
                dbus_error_free(&error);
                return 0;
        }

        unit_ref_set(&t->unit, u);

        return 0;
}

int config_parse_socket_service(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Socket *s = data;
        int r;
        DBusError error;
        Unit *x;
        _cleanup_free_ char *p = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        dbus_error_init(&error);

        p = unit_name_printf(UNIT(s), rvalue);
        if (!p)
                return log_oom();

        if (!endswith(p, ".service")) {
                log_error("[%s:%u] Unit must be of type service, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        r = manager_load_unit(UNIT(s)->manager, p, NULL, &error, &x);
        if (r < 0) {
                log_error("[%s:%u] Failed to load unit %s, ignoring: %s", filename, line, rvalue, bus_error(&error, r));
                dbus_error_free(&error);
                return 0;
        }

        unit_ref_set(&s->service, x);

        return 0;
}

int config_parse_service_sockets(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Service *s = data;
        int r;
        char *state, *w;
        size_t l;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char _cleanup_free_ *t = NULL, *k = NULL;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                k = unit_name_printf(UNIT(s), t);
                if (!k)
                        return log_oom();

                if (!endswith(k, ".socket")) {
                        log_error("[%s:%u] Unit must be of type socket, ignoring: %s",
                                  filename, line, k);
                        continue;
                }

                r = unit_add_two_dependencies_by_name(UNIT(s), UNIT_WANTS, UNIT_AFTER, k, NULL, true);
                if (r < 0)
                        log_error("[%s:%u] Failed to add dependency on %s, ignoring: %s",
                                  filename, line, k, strerror(-r));

                r = unit_add_dependency_by_name(UNIT(s), UNIT_TRIGGERED_BY, k, NULL, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

int config_parse_service_timeout(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Service *s = userdata;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(s);

        r = config_parse_usec(filename, line, section, lvalue, ltype, rvalue, data, userdata);
        if (r < 0)
                return r;

        if (streq(lvalue, "TimeoutSec")) {
                s->start_timeout_defined = true;
                s->timeout_stop_usec = s->timeout_start_usec;
        } else if (streq(lvalue, "TimeoutStartSec"))
                s->start_timeout_defined = true;

        return 0;
}

int config_parse_unit_env_file(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        char ***env = data;
        Unit *u = userdata;
        _cleanup_free_ char *s = NULL;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment frees the list */
                strv_free(*env);
                *env = NULL;
                return 0;
        }

        s = unit_full_printf(u, rvalue);
        if (!s)
                return log_oom();

        if (!path_is_absolute(s[0] == '-' ? s + 1 : s)) {
                log_error("[%s:%u] Path '%s' is not absolute, ignoring.", filename, line, s);
                return 0;
        }

        r = strv_extend(env, s);
        if (r < 0)
                return log_oom();

        return 0;
}

int config_parse_environ(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        char*** env = data, *w, *state;
        size_t l;
        _cleanup_free_ char *k = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                strv_free(*env);
                *env = NULL;
                return 0;
        }

        k = unit_full_printf(u, rvalue);
        if (!k)
                return log_oom();

        FOREACH_WORD_QUOTED(w, l, k, state) {
                _cleanup_free_ char *n;
                char **x;

                n = cunescape_length(w, l);
                if (!n)
                        return log_oom();

                if (!env_assignment_is_valid(n)) {
                        log_error("[%s:%u] Invalid environment assignment, ignoring: %s", filename, line, rvalue);
                        continue;
                }

                x = strv_env_set(*env, n);
                if (!x)
                        return log_oom();

                strv_free(*env);
                *env = x;
        }

        return 0;
}

int config_parse_ip_tos(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        int *ip_tos = data, x;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        x = ip_tos_from_string(rvalue);
        if (x < 0) {
                log_error("[%s:%u] Failed to parse IP TOS value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        *ip_tos = x;
        return 0;
}

int config_parse_unit_condition_path(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ConditionType cond = ltype;
        Unit *u = data;
        bool trigger, negate;
        Condition *c;
        _cleanup_free_ char *p = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                condition_free_list(u->conditions);
                u->conditions = NULL;
                return 0;
        }

        trigger = rvalue[0] == '|';
        if (trigger)
                rvalue++;

        negate = rvalue[0] == '!';
        if (negate)
                rvalue++;

        p = unit_full_printf(u, rvalue);
        if (!p)
                return log_oom();

        if (!path_is_absolute(p)) {
                log_error("[%s:%u] Path in condition not absolute, ignoring: %s", filename, line, p);
                return 0;
        }

        c = condition_new(cond, p, trigger, negate);
        if (!c)
                return log_oom();

        LIST_PREPEND(Condition, conditions, u->conditions, c);
        return 0;
}

int config_parse_unit_condition_string(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ConditionType cond = ltype;
        Unit *u = data;
        bool trigger, negate;
        Condition *c;
        _cleanup_free_ char *s = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                condition_free_list(u->conditions);
                u->conditions = NULL;
                return 0;
        }

        trigger = rvalue[0] == '|';
        if (trigger)
                rvalue++;

        negate = rvalue[0] == '!';
        if (negate)
                rvalue++;

        s = unit_full_printf(u, rvalue);
        if (!s)
                return log_oom();

        c = condition_new(cond, s, trigger, negate);
        if (!c)
                return log_oom();

        LIST_PREPEND(Condition, conditions, u->conditions, c);
        return 0;
}

int config_parse_unit_condition_null(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = data;
        Condition *c;
        bool trigger, negate;
        int b;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                condition_free_list(u->conditions);
                u->conditions = NULL;
                return 0;
        }

        trigger = rvalue[0] == '|';
        if (trigger)
                rvalue++;

        negate = rvalue[0] == '!';
        if (negate)
                rvalue++;

        b = parse_boolean(rvalue);
        if (b < 0) {
                log_error("[%s:%u] Failed to parse boolean value in condition, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (!b)
                negate = !negate;

        c = condition_new(CONDITION_NULL, NULL, trigger, negate);
        if (!c)
                return log_oom();

        LIST_PREPEND(Condition, conditions, u->conditions, c);
        return 0;
}

DEFINE_CONFIG_PARSE_ENUM(config_parse_notify_access, notify_access, NotifyAccess, "Failed to parse notify access specifier");
DEFINE_CONFIG_PARSE_ENUM(config_parse_start_limit_action, start_limit_action, StartLimitAction, "Failed to parse start limit action specifier");

int config_parse_unit_cgroup_attr(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = data;
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                /* Empty assignment clears the list */
                cgroup_attribute_free_list(u->cgroup_attributes);
                u->cgroup_attributes = NULL;
                return 0;
        }

        l = strv_split_quoted(rvalue);
        if (!l)
                return log_oom();

        if (strv_length(l) != 2) {
                log_error("[%s:%u] Failed to parse cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        r = unit_add_cgroup_attribute(u, NULL, l[0], l[1], NULL, NULL);
        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

int config_parse_unit_cpu_shares(const char *filename, unsigned line, const char *section, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata) {
        Unit *u = data;
        int r;
        unsigned long ul;
        _cleanup_free_ char *t = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (safe_atolu(rvalue, &ul) < 0 || ul < 1) {
                log_error("[%s:%u] Failed to parse CPU shares value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (asprintf(&t, "%lu", ul) < 0)
                return log_oom();

        r = unit_add_cgroup_attribute(u, "cpu", "cpu.shares", t, NULL, NULL);
        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

int config_parse_unit_memory_limit(const char *filename, unsigned line, const char *section, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata) {
        Unit *u = data;
        int r;
        off_t sz;
        _cleanup_free_ char *t = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (parse_bytes(rvalue, &sz) < 0 || sz <= 0) {
                log_error("[%s:%u] Failed to parse memory limit value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (asprintf(&t, "%llu", (unsigned long long) sz) < 0)
                return log_oom();

        r = unit_add_cgroup_attribute(u,
                                      "memory",
                                      streq(lvalue, "MemorySoftLimit") ? "memory.soft_limit_in_bytes" : "memory.limit_in_bytes",
                                      t, NULL, NULL);
        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

static int device_map(const char *controller, const char *name, const char *value, char **ret) {
        _cleanup_strv_free_ char **l = NULL;

        assert(controller);
        assert(name);
        assert(value);
        assert(ret);

        l = strv_split_quoted(value);
        if (!l)
                return -ENOMEM;

        assert(strv_length(l) >= 1);

        if (streq(l[0], "*")) {

                if (asprintf(ret, "a *:*%s%s",
                             isempty(l[1]) ? "" : " ", strempty(l[1])) < 0)
                        return -ENOMEM;
        } else {
                struct stat st;

                if (stat(l[0], &st) < 0) {
                        log_warning("Couldn't stat device %s", l[0]);
                        return -errno;
                }

                if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
                        log_warning("%s is not a device.", l[0]);
                        return -ENODEV;
                }

                if (asprintf(ret, "%c %u:%u%s%s",
                             S_ISCHR(st.st_mode) ? 'c' : 'b',
                             major(st.st_rdev), minor(st.st_rdev),
                             isempty(l[1]) ? "" : " ", strempty(l[1])) < 0)
                        return -ENOMEM;
        }

        return 0;
}

int config_parse_unit_device_allow(const char *filename, unsigned line, const char *section, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata) {
        Unit *u = data;
        _cleanup_strv_free_ char **l = NULL;
        int r;
        unsigned k;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        l = strv_split_quoted(rvalue);
        if (!l)
                return log_oom();

        k = strv_length(l);
        if (k < 1 || k > 2) {
                log_error("[%s:%u] Failed to parse device value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (!streq(l[0], "*") && !path_startswith(l[0], "/dev")) {
                log_error("[%s:%u] Device node path not absolute, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (!isempty(l[1]) && !in_charset(l[1], "rwm")) {
                log_error("[%s:%u] Device access string invalid, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        r = unit_add_cgroup_attribute(u, "devices",
                                      streq(lvalue, "DeviceAllow") ? "devices.allow" : "devices.deny",
                                      rvalue, device_map, NULL);

        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

static int blkio_map(const char *controller, const char *name, const char *value, char **ret) {
        struct stat st;
        _cleanup_strv_free_ char **l = NULL;
        dev_t d;

        assert(controller);
        assert(name);
        assert(value);
        assert(ret);

        l = strv_split_quoted(value);
        if (!l)
                return log_oom();

        assert(strv_length(l) == 2);

        if (stat(l[0], &st) < 0) {
                log_warning("Couldn't stat device %s", l[0]);
                return -errno;
        }

        if (S_ISBLK(st.st_mode))
                d = st.st_rdev;
        else if (major(st.st_dev) != 0) {
                /* If this is not a device node then find the block
                 * device this file is stored on */
                d = st.st_dev;

                /* If this is a partition, try to get the originating
                 * block device */
                block_get_whole_disk(d, &d);
        } else {
                log_warning("%s is not a block device and file system block device cannot be determined or is not local.", l[0]);
                return -ENODEV;
        }

        if (asprintf(ret, "%u:%u %s", major(d), minor(d), l[1]) < 0)
                return -ENOMEM;

        return 0;
}

int config_parse_unit_blkio_weight(const char *filename, unsigned line, const char *section, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata) {
        Unit *u = data;
        int r;
        unsigned long ul;
        const char *device = NULL, *weight;
        unsigned k;
        _cleanup_free_ char *t = NULL;
        _cleanup_strv_free_ char **l = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        l = strv_split_quoted(rvalue);
        if (!l)
                return log_oom();

        k = strv_length(l);
        if (k < 1 || k > 2) {
                log_error("[%s:%u] Failed to parse weight value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (k == 1)
                weight = l[0];
        else {
                device = l[0];
                weight = l[1];
        }

        if (device && !path_is_absolute(device)) {
                log_error("[%s:%u] Failed to parse block device node value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (safe_atolu(weight, &ul) < 0 || ul < 10 || ul > 1000) {
                log_error("[%s:%u] Failed to parse block IO weight value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (device)
                r = asprintf(&t, "%s %lu", device, ul);
        else
                r = asprintf(&t, "%lu", ul);
        if (r < 0)
                return log_oom();

        if (device)
                r = unit_add_cgroup_attribute(u, "blkio", "blkio.weight_device", t, blkio_map, NULL);
        else
                r = unit_add_cgroup_attribute(u, "blkio", "blkio.weight", t, NULL, NULL);
        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

int config_parse_unit_blkio_bandwidth(const char *filename, unsigned line, const char *section, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata) {
        Unit *u = data;
        int r;
        off_t bytes;
        unsigned k;
        _cleanup_free_ char *t = NULL;
        _cleanup_strv_free_ char **l = NULL;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        l = strv_split_quoted(rvalue);
        if (!l)
                return log_oom();

        k = strv_length(l);
        if (k != 2) {
                log_error("[%s:%u] Failed to parse bandwidth value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (!path_is_absolute(l[0])) {
                log_error("[%s:%u] Failed to parse block device node value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        if (parse_bytes(l[1], &bytes) < 0 || bytes <= 0) {
                log_error("[%s:%u] Failed to parse block IO bandwidth value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        r = asprintf(&t, "%s %llu", l[0], (unsigned long long) bytes);
        if (r < 0)
                return log_oom();

        r = unit_add_cgroup_attribute(u, "blkio",
                                      streq(lvalue, "BlockIOReadBandwidth") ? "blkio.read_bps_device" : "blkio.write_bps_device",
                                      t, blkio_map, NULL);
        if (r < 0) {
                log_error("[%s:%u] Failed to add cgroup attribute value, ignoring: %s", filename, line, rvalue);
                return 0;
        }

        return 0;
}

int config_parse_unit_requires_mounts_for(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        int r;
        bool empty_before;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        empty_before = !u->requires_mounts_for;

        r = config_parse_path_strv(filename, line, section, lvalue, ltype, rvalue, data, userdata);

        /* Make it easy to find units with requires_mounts set */
        if (empty_before && u->requires_mounts_for)
                LIST_PREPEND(Unit, has_requires_mounts_for, u->manager->has_requires_mounts_for, u);

        return r;
}

int config_parse_documentation(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Unit *u = userdata;
        int r;
        char **a, **b;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                strv_free(u->documentation);
                u->documentation = NULL;
                return 0;
        }

        r = config_parse_unit_strv_printf(filename, line, section, lvalue, ltype, rvalue, data, userdata);
        if (r < 0)
                return r;

        for (a = b = u->documentation; a && *a; a++) {

                if (is_valid_documentation_url(*a))
                        *(b++) = *a;
                else {
                        log_error("[%s:%u] Invalid URL, ignoring: %s", filename, line, *a);
                        free(*a);
                }
        }
        *b = NULL;

        return r;
}

static void syscall_set(uint32_t *p, int nr) {
        nr = SYSCALL_TO_INDEX(nr);
        p[nr >> 4] |= 1 << (nr & 31);
}

static void syscall_unset(uint32_t *p, int nr) {
        nr = SYSCALL_TO_INDEX(nr);
        p[nr >> 4] &= ~(1 << (nr & 31));
}

int config_parse_syscall_filter(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        ExecContext *c = data;
        Unit *u = userdata;
        bool invert = false;
        char *w;
        size_t l;
        char *state;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(u);

        if (isempty(rvalue)) {
                /* Empty assignment resets the list */
                free(c->syscall_filter);
                c->syscall_filter = NULL;
                return 0;
        }

        if (rvalue[0] == '~') {
                invert = true;
                rvalue++;
        }

        if (!c->syscall_filter) {
                size_t n;

                n = (syscall_max() + 31) >> 4;
                c->syscall_filter = new(uint32_t, n);
                if (!c->syscall_filter)
                        return log_oom();

                memset(c->syscall_filter, invert ? 0xFF : 0, n * sizeof(uint32_t));

                /* Add these by default */
                syscall_set(c->syscall_filter, __NR_execve);
                syscall_set(c->syscall_filter, __NR_rt_sigreturn);
#ifdef __NR_sigreturn
                syscall_set(c->syscall_filter, __NR_sigreturn);
#endif
                syscall_set(c->syscall_filter, __NR_exit_group);
                syscall_set(c->syscall_filter, __NR_exit);
        }

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                int id;
                char _cleanup_free_ *t = NULL;

                t = strndup(w, l);
                if (!t)
                        return log_oom();

                id = syscall_from_name(t);
                if (id < 0)  {
                        log_error("[%s:%u] Failed to parse syscall, ignoring: %s",
                                  filename, line, t);
                        continue;
                }

                if (invert)
                        syscall_unset(c->syscall_filter, id);
                else
                        syscall_set(c->syscall_filter, id);
        }

        c->no_new_privileges = true;

        return 0;
}

#define FOLLOW_MAX 8

static int open_follow(char **filename, FILE **_f, Set *names, char **_final) {
        unsigned c = 0;
        int fd, r;
        FILE *f;
        char *id = NULL;

        assert(filename);
        assert(*filename);
        assert(_f);
        assert(names);

        /* This will update the filename pointer if the loaded file is
         * reached by a symlink. The old string will be freed. */

        for (;;) {
                char *target, *name;

                if (c++ >= FOLLOW_MAX)
                        return -ELOOP;

                path_kill_slashes(*filename);

                /* Add the file name we are currently looking at to
                 * the names of this unit, but only if it is a valid
                 * unit name. */
                name = path_get_file_name(*filename);

                if (unit_name_is_valid(name, true)) {

                        id = set_get(names, name);
                        if (!id) {
                                id = strdup(name);
                                if (!id)
                                        return -ENOMEM;

                                r = set_put(names, id);
                                if (r < 0) {
                                        free(id);
                                        return r;
                                }
                        }
                }

                /* Try to open the file name, but don't if its a symlink */
                fd = open(*filename, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
                if (fd >= 0)
                        break;

                if (errno != ELOOP)
                        return -errno;

                /* Hmm, so this is a symlink. Let's read the name, and follow it manually */
                r = readlink_and_make_absolute(*filename, &target);
                if (r < 0)
                        return r;

                free(*filename);
                *filename = target;
        }

        f = fdopen(fd, "re");
        if (!f) {
                r = -errno;
                close_nointr_nofail(fd);
                return r;
        }

        *_f = f;
        *_final = id;
        return 0;
}

static int merge_by_names(Unit **u, Set *names, const char *id) {
        char *k;
        int r;

        assert(u);
        assert(*u);
        assert(names);

        /* Let's try to add in all symlink names we found */
        while ((k = set_steal_first(names))) {

                /* First try to merge in the other name into our
                 * unit */
                r = unit_merge_by_name(*u, k);
                if (r < 0) {
                        Unit *other;

                        /* Hmm, we couldn't merge the other unit into
                         * ours? Then let's try it the other way
                         * round */

                        other = manager_get_unit((*u)->manager, k);
                        free(k);

                        if (other) {
                                r = unit_merge(other, *u);
                                if (r >= 0) {
                                        *u = other;
                                        return merge_by_names(u, names, NULL);
                                }
                        }

                        return r;
                }

                if (id == k)
                        unit_choose_id(*u, id);

                free(k);
        }

        return 0;
}

static int load_from_path(Unit *u, const char *path) {
        int r;
        Set *symlink_names;
        FILE *f = NULL;
        char *filename = NULL, *id = NULL;
        Unit *merged;
        struct stat st;

        assert(u);
        assert(path);

        symlink_names = set_new(string_hash_func, string_compare_func);
        if (!symlink_names)
                return -ENOMEM;

        if (path_is_absolute(path)) {

                filename = strdup(path);
                if (!filename) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = open_follow(&filename, &f, symlink_names, &id);
                if (r < 0) {
                        free(filename);
                        filename = NULL;

                        if (r != -ENOENT)
                                goto finish;
                }

        } else  {
                char **p;

                STRV_FOREACH(p, u->manager->lookup_paths.unit_path) {

                        /* Instead of opening the path right away, we manually
                         * follow all symlinks and add their name to our unit
                         * name set while doing so */
                        filename = path_make_absolute(path, *p);
                        if (!filename) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        if (u->manager->unit_path_cache &&
                            !set_get(u->manager->unit_path_cache, filename))
                                r = -ENOENT;
                        else
                                r = open_follow(&filename, &f, symlink_names, &id);

                        if (r < 0) {
                                free(filename);
                                filename = NULL;

                                if (r != -ENOENT)
                                        goto finish;

                                /* Empty the symlink names for the next run */
                                set_clear_free(symlink_names);
                                continue;
                        }

                        break;
                }
        }

        if (!filename) {
                /* Hmm, no suitable file found? */
                r = 0;
                goto finish;
        }

        merged = u;
        r = merge_by_names(&merged, symlink_names, id);
        if (r < 0)
                goto finish;

        if (merged != u) {
                u->load_state = UNIT_MERGED;
                r = 0;
                goto finish;
        }

        if (fstat(fileno(f), &st) < 0) {
                r = -errno;
                goto finish;
        }

        if (null_or_empty(&st))
                u->load_state = UNIT_MASKED;
        else {
                /* Now, parse the file contents */
                r = config_parse(filename, f, UNIT_VTABLE(u)->sections, config_item_perf_lookup, (void*) load_fragment_gperf_lookup, false, u);
                if (r < 0)
                        goto finish;

                u->load_state = UNIT_LOADED;
        }

        free(u->fragment_path);
        u->fragment_path = filename;
        filename = NULL;

        u->fragment_mtime = timespec_load(&st.st_mtim);

        if (u->source_path) {
                if (stat(u->source_path, &st) >= 0)
                        u->source_mtime = timespec_load(&st.st_mtim);
                else
                        u->source_mtime = 0;
        }

        r = 0;

finish:
        set_free_free(symlink_names);
        free(filename);

        if (f)
                fclose(f);

        return r;
}

int unit_load_fragment(Unit *u) {
        int r;
        Iterator i;
        const char *t;

        assert(u);
        assert(u->load_state == UNIT_STUB);
        assert(u->id);

        /* First, try to find the unit under its id. We always look
         * for unit files in the default directories, to make it easy
         * to override things by placing things in /etc/systemd/system */
        r = load_from_path(u, u->id);
        if (r < 0)
                return r;

        /* Try to find an alias we can load this with */
        if (u->load_state == UNIT_STUB)
                SET_FOREACH(t, u->names, i) {

                        if (t == u->id)
                                continue;

                        r = load_from_path(u, t);
                        if (r < 0)
                                return r;

                        if (u->load_state != UNIT_STUB)
                                break;
                }

        /* And now, try looking for it under the suggested (originally linked) path */
        if (u->load_state == UNIT_STUB && u->fragment_path) {

                r = load_from_path(u, u->fragment_path);
                if (r < 0)
                        return r;

                if (u->load_state == UNIT_STUB) {
                        /* Hmm, this didn't work? Then let's get rid
                         * of the fragment path stored for us, so that
                         * we don't point to an invalid location. */
                        free(u->fragment_path);
                        u->fragment_path = NULL;
                }
        }

        /* Look for a template */
        if (u->load_state == UNIT_STUB && u->instance) {
                char *k;

                k = unit_name_template(u->id);
                if (!k)
                        return -ENOMEM;

                r = load_from_path(u, k);
                free(k);

                if (r < 0)
                        return r;

                if (u->load_state == UNIT_STUB)
                        SET_FOREACH(t, u->names, i) {

                                if (t == u->id)
                                        continue;

                                k = unit_name_template(t);
                                if (!k)
                                        return -ENOMEM;

                                r = load_from_path(u, k);
                                free(k);

                                if (r < 0)
                                        return r;

                                if (u->load_state != UNIT_STUB)
                                        break;
                        }
        }

        return 0;
}

void unit_dump_config_items(FILE *f) {
        static const struct {
                const ConfigParserCallback callback;
                const char *rvalue;
        } table[] = {
                { config_parse_int,                   "INTEGER" },
                { config_parse_unsigned,              "UNSIGNED" },
                { config_parse_bytes_size,            "SIZE" },
                { config_parse_bool,                  "BOOLEAN" },
                { config_parse_string,                "STRING" },
                { config_parse_path,                  "PATH" },
                { config_parse_unit_path_printf,      "PATH" },
                { config_parse_strv,                  "STRING [...]" },
                { config_parse_exec_nice,             "NICE" },
                { config_parse_exec_oom_score_adjust, "OOMSCOREADJUST" },
                { config_parse_exec_io_class,         "IOCLASS" },
                { config_parse_exec_io_priority,      "IOPRIORITY" },
                { config_parse_exec_cpu_sched_policy, "CPUSCHEDPOLICY" },
                { config_parse_exec_cpu_sched_prio,   "CPUSCHEDPRIO" },
                { config_parse_exec_cpu_affinity,     "CPUAFFINITY" },
                { config_parse_mode,                  "MODE" },
                { config_parse_unit_env_file,         "FILE" },
                { config_parse_output,                "OUTPUT" },
                { config_parse_input,                 "INPUT" },
                { config_parse_facility,              "FACILITY" },
                { config_parse_level,                 "LEVEL" },
                { config_parse_exec_capabilities,     "CAPABILITIES" },
                { config_parse_exec_secure_bits,      "SECUREBITS" },
                { config_parse_bounding_set,          "BOUNDINGSET" },
                { config_parse_limit,                 "LIMIT" },
                { config_parse_unit_cgroup,           "CGROUP [...]" },
                { config_parse_unit_deps,             "UNIT [...]" },
                { config_parse_exec,                  "PATH [ARGUMENT [...]]" },
                { config_parse_service_type,          "SERVICETYPE" },
                { config_parse_service_restart,       "SERVICERESTART" },
#ifdef HAVE_SYSV_COMPAT
                { config_parse_sysv_priority,         "SYSVPRIORITY" },
#else
                { config_parse_warn_compat,           "NOTSUPPORTED" },
#endif
                { config_parse_kill_mode,             "KILLMODE" },
                { config_parse_kill_signal,           "SIGNAL" },
                { config_parse_socket_listen,         "SOCKET [...]" },
                { config_parse_socket_bind,           "SOCKETBIND" },
                { config_parse_socket_bindtodevice,   "NETWORKINTERFACE" },
                { config_parse_usec,                  "SECONDS" },
                { config_parse_nsec,                  "NANOSECONDS" },
                { config_parse_path_strv,             "PATH [...]" },
                { config_parse_unit_requires_mounts_for, "PATH [...]" },
                { config_parse_exec_mount_flags,      "MOUNTFLAG [...]" },
                { config_parse_unit_string_printf,    "STRING" },
                { config_parse_timer,                 "TIMER" },
                { config_parse_timer_unit,            "NAME" },
                { config_parse_path_spec,             "PATH" },
                { config_parse_path_unit,             "UNIT" },
                { config_parse_notify_access,         "ACCESS" },
                { config_parse_ip_tos,                "TOS" },
                { config_parse_unit_condition_path,   "CONDITION" },
                { config_parse_unit_condition_string, "CONDITION" },
                { config_parse_unit_condition_null,   "CONDITION" },
        };

        const char *prev = NULL;
        const char *i;

        assert(f);

        NULSTR_FOREACH(i, load_fragment_gperf_nulstr) {
                const char *rvalue = "OTHER", *lvalue;
                unsigned j;
                size_t prefix_len;
                const char *dot;
                const ConfigPerfItem *p;

                assert_se(p = load_fragment_gperf_lookup(i, strlen(i)));

                dot = strchr(i, '.');
                lvalue = dot ? dot + 1 : i;
                prefix_len = dot-i;

                if (dot)
                        if (!prev || !strneq(prev, i, prefix_len+1)) {
                                if (prev)
                                        fputc('\n', f);

                                fprintf(f, "[%.*s]\n", (int) prefix_len, i);
                        }

                for (j = 0; j < ELEMENTSOF(table); j++)
                        if (p->parse == table[j].callback) {
                                rvalue = table[j].rvalue;
                                break;
                        }

                fprintf(f, "%s=%s\n", lvalue, rvalue);
                prev = i;
        }
}
