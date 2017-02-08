/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

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

#include <errno.h>
#include <linux/seccomp.h>
#include <seccomp.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/shm.h>

#include "af-list.h"
#include "alloc-util.h"
#include "macro.h"
#include "nsflags.h"
#include "seccomp-util.h"
#include "string-util.h"
#include "util.h"
#include "errno-list.h"

const uint32_t seccomp_local_archs[] = {

#if defined(__i386__) || defined(__x86_64__)
                SCMP_ARCH_X86,
                SCMP_ARCH_X86_64,
                SCMP_ARCH_X32,

#elif defined(__arm__) || defined(__aarch64__)
                SCMP_ARCH_ARM,
                SCMP_ARCH_AARCH64,

#elif defined(__mips__) || defined(__mips64__)
                SCMP_ARCH_MIPS,
                SCMP_ARCH_MIPS64,
                SCMP_ARCH_MIPS64N32,
                SCMP_ARCH_MIPSEL,
                SCMP_ARCH_MIPSEL64,
                SCMP_ARCH_MIPSEL64N32,

#elif defined(__powerpc__) || defined(__powerpc64__)
                SCMP_ARCH_PPC,
                SCMP_ARCH_PPC64,
                SCMP_ARCH_PPC64LE,

#elif defined(__s390__) || defined(__s390x__)
                SCMP_ARCH_S390,
                SCMP_ARCH_S390X,
#endif
                (uint32_t) -1
        };

const char* seccomp_arch_to_string(uint32_t c) {
        /* Maintain order used in <seccomp.h>.
         *
         * Names used here should be the same as those used for ConditionArchitecture=,
         * except for "subarchitectures" like x32. */

        switch(c) {
        case SCMP_ARCH_NATIVE:
                return "native";
        case SCMP_ARCH_X86:
                return "x86";
        case SCMP_ARCH_X86_64:
                return "x86-64";
        case SCMP_ARCH_X32:
                return "x32";
        case SCMP_ARCH_ARM:
                return "arm";
        case SCMP_ARCH_AARCH64:
                return "arm64";
        case SCMP_ARCH_MIPS:
                return "mips";
        case SCMP_ARCH_MIPS64:
                return "mips64";
        case SCMP_ARCH_MIPS64N32:
                return "mips64-n32";
        case SCMP_ARCH_MIPSEL:
                return "mips-le";
        case SCMP_ARCH_MIPSEL64:
                return "mips64-le";
        case SCMP_ARCH_MIPSEL64N32:
                return "mips64-le-n32";
        case SCMP_ARCH_PPC:
                return "ppc";
        case SCMP_ARCH_PPC64:
                return "ppc64";
        case SCMP_ARCH_PPC64LE:
                return "ppc64-le";
        case SCMP_ARCH_S390:
                return "s390";
        case SCMP_ARCH_S390X:
                return "s390x";
        default:
                return NULL;
        }
}

int seccomp_arch_from_string(const char *n, uint32_t *ret) {
        if (!n)
                return -EINVAL;

        assert(ret);

        if (streq(n, "native"))
                *ret = SCMP_ARCH_NATIVE;
        else if (streq(n, "x86"))
                *ret = SCMP_ARCH_X86;
        else if (streq(n, "x86-64"))
                *ret = SCMP_ARCH_X86_64;
        else if (streq(n, "x32"))
                *ret = SCMP_ARCH_X32;
        else if (streq(n, "arm"))
                *ret = SCMP_ARCH_ARM;
        else if (streq(n, "arm64"))
                *ret = SCMP_ARCH_AARCH64;
        else if (streq(n, "mips"))
                *ret = SCMP_ARCH_MIPS;
        else if (streq(n, "mips64"))
                *ret = SCMP_ARCH_MIPS64;
        else if (streq(n, "mips64-n32"))
                *ret = SCMP_ARCH_MIPS64N32;
        else if (streq(n, "mips-le"))
                *ret = SCMP_ARCH_MIPSEL;
        else if (streq(n, "mips64-le"))
                *ret = SCMP_ARCH_MIPSEL64;
        else if (streq(n, "mips64-le-n32"))
                *ret = SCMP_ARCH_MIPSEL64N32;
        else if (streq(n, "ppc"))
                *ret = SCMP_ARCH_PPC;
        else if (streq(n, "ppc64"))
                *ret = SCMP_ARCH_PPC64;
        else if (streq(n, "ppc64-le"))
                *ret = SCMP_ARCH_PPC64LE;
        else if (streq(n, "s390"))
                *ret = SCMP_ARCH_S390;
        else if (streq(n, "s390x"))
                *ret = SCMP_ARCH_S390X;
        else
                return -EINVAL;

        return 0;
}

int seccomp_init_for_arch(scmp_filter_ctx *ret, uint32_t arch, uint32_t default_action) {
        scmp_filter_ctx seccomp;
        int r;

        /* Much like seccomp_init(), but initializes the filter for one specific architecture only, without affecting
         * any others. Also, turns off the NNP fiddling. */

        seccomp = seccomp_init(default_action);
        if (!seccomp)
                return -ENOMEM;

        if (arch != SCMP_ARCH_NATIVE &&
            arch != seccomp_arch_native()) {

                r = seccomp_arch_remove(seccomp, seccomp_arch_native());
                if (r < 0)
                        goto finish;

                r = seccomp_arch_add(seccomp, arch);
                if (r < 0)
                        goto finish;

                assert(seccomp_arch_exist(seccomp, arch) >= 0);
                assert(seccomp_arch_exist(seccomp, SCMP_ARCH_NATIVE) == -EEXIST);
                assert(seccomp_arch_exist(seccomp, seccomp_arch_native()) == -EEXIST);
        } else {
                assert(seccomp_arch_exist(seccomp, SCMP_ARCH_NATIVE) >= 0);
                assert(seccomp_arch_exist(seccomp, seccomp_arch_native()) >= 0);
        }

        r = seccomp_attr_set(seccomp, SCMP_FLTATR_ACT_BADARCH, SCMP_ACT_ALLOW);
        if (r < 0)
                goto finish;

        r = seccomp_attr_set(seccomp, SCMP_FLTATR_CTL_NNP, 0);
        if (r < 0)
                goto finish;

        *ret = seccomp;
        return 0;

finish:
        seccomp_release(seccomp);
        return r;
}

static bool is_basic_seccomp_available(void) {
        int r;
        r = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
        return r >= 0;
}

static bool is_seccomp_filter_available(void) {
        int r;
        r = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, NULL, 0, 0);
        return r < 0 && errno == EFAULT;
}

bool is_seccomp_available(void) {
        static int cached_enabled = -1;
        if (cached_enabled < 0)
                cached_enabled = is_basic_seccomp_available() && is_seccomp_filter_available();
        return cached_enabled;
}

const SyscallFilterSet syscall_filter_sets[_SYSCALL_FILTER_SET_MAX] = {
        [SYSCALL_FILTER_SET_BASIC_IO] = {
                /* Basic IO */
                .name = "@basic-io",
                .value =
                "close\0"
                "dup2\0"
                "dup3\0"
                "dup\0"
                "lseek\0"
                "pread64\0"
                "preadv\0"
                "pwrite64\0"
                "pwritev\0"
                "read\0"
                "readv\0"
                "write\0"
                "writev\0"
        },
        [SYSCALL_FILTER_SET_CLOCK] = {
                /* Clock */
                .name = "@clock",
                .value =
                "adjtimex\0"
                "clock_adjtime\0"
                "clock_settime\0"
                "settimeofday\0"
                "stime\0"
        },
        [SYSCALL_FILTER_SET_CPU_EMULATION] = {
                /* CPU emulation calls */
                .name = "@cpu-emulation",
                .value =
                "modify_ldt\0"
                "subpage_prot\0"
                "switch_endian\0"
                "vm86\0"
                "vm86old\0"
        },
        [SYSCALL_FILTER_SET_DEBUG] = {
                /* Debugging/Performance Monitoring/Tracing */
                .name = "@debug",
                .value =
                "lookup_dcookie\0"
                "perf_event_open\0"
                "process_vm_readv\0"
                "process_vm_writev\0"
                "ptrace\0"
                "rtas\0"
#ifdef __NR_s390_runtime_instr
                "s390_runtime_instr\0"
#endif
                "sys_debug_setcontext\0"
        },
        [SYSCALL_FILTER_SET_DEFAULT] = {
                /* Default list: the most basic of operations */
                .name = "@default",
                .value =
                "clock_getres\0"
                "clock_gettime\0"
                "clock_nanosleep\0"
                "execve\0"
                "exit\0"
                "exit_group\0"
                "getrlimit\0"      /* make sure processes can query stack size and such */
                "gettimeofday\0"
                "nanosleep\0"
                "pause\0"
                "rt_sigreturn\0"
                "sigreturn\0"
                "time\0"
        },
        [SYSCALL_FILTER_SET_IO_EVENT] = {
                /* Event loop use */
                .name = "@io-event",
                .value =
                "_newselect\0"
                "epoll_create1\0"
                "epoll_create\0"
                "epoll_ctl\0"
                "epoll_ctl_old\0"
                "epoll_pwait\0"
                "epoll_wait\0"
                "epoll_wait_old\0"
                "eventfd2\0"
                "eventfd\0"
                "poll\0"
                "ppoll\0"
                "pselect6\0"
                "select\0"
        },
        [SYSCALL_FILTER_SET_IPC] = {
                /* Message queues, SYSV IPC or other IPC */
                .name = "@ipc",
                .value = "ipc\0"
                "memfd_create\0"
                "mq_getsetattr\0"
                "mq_notify\0"
                "mq_open\0"
                "mq_timedreceive\0"
                "mq_timedsend\0"
                "mq_unlink\0"
                "msgctl\0"
                "msgget\0"
                "msgrcv\0"
                "msgsnd\0"
                "pipe2\0"
                "pipe\0"
                "process_vm_readv\0"
                "process_vm_writev\0"
                "semctl\0"
                "semget\0"
                "semop\0"
                "semtimedop\0"
                "shmat\0"
                "shmctl\0"
                "shmdt\0"
                "shmget\0"
        },
        [SYSCALL_FILTER_SET_KEYRING] = {
                /* Keyring */
                .name = "@keyring",
                .value =
                "add_key\0"
                "keyctl\0"
                "request_key\0"
        },
        [SYSCALL_FILTER_SET_MODULE] = {
                /* Kernel module control */
                .name = "@module",
                .value =
                "delete_module\0"
                "finit_module\0"
                "init_module\0"
        },
        [SYSCALL_FILTER_SET_MOUNT] = {
                /* Mounting */
                .name = "@mount",
                .value =
                "chroot\0"
                "mount\0"
                "pivot_root\0"
                "umount2\0"
                "umount\0"
        },
        [SYSCALL_FILTER_SET_NETWORK_IO] = {
                /* Network or Unix socket IO, should not be needed if not network facing */
                .name = "@network-io",
                .value =
                "accept4\0"
                "accept\0"
                "bind\0"
                "connect\0"
                "getpeername\0"
                "getsockname\0"
                "getsockopt\0"
                "listen\0"
                "recv\0"
                "recvfrom\0"
                "recvmmsg\0"
                "recvmsg\0"
                "send\0"
                "sendmmsg\0"
                "sendmsg\0"
                "sendto\0"
                "setsockopt\0"
                "shutdown\0"
                "socket\0"
                "socketcall\0"
                "socketpair\0"
        },
        [SYSCALL_FILTER_SET_OBSOLETE] = {
                /* Unusual, obsolete or unimplemented, some unknown even to libseccomp */
                .name = "@obsolete",
                .value =
                "_sysctl\0"
                "afs_syscall\0"
                "break\0"
                "create_module\0"
                "ftime\0"
                "get_kernel_syms\0"
                "getpmsg\0"
                "gtty\0"
                "lock\0"
                "mpx\0"
                "prof\0"
                "profil\0"
                "putpmsg\0"
                "query_module\0"
                "security\0"
                "sgetmask\0"
                "ssetmask\0"
                "stty\0"
                "sysfs\0"
                "tuxcall\0"
                "ulimit\0"
                "uselib\0"
                "ustat\0"
                "vserver\0"
        },
        [SYSCALL_FILTER_SET_PRIVILEGED] = {
                /* Nice grab-bag of all system calls which need superuser capabilities */
                .name = "@privileged",
                .value =
                "@clock\0"
                "@module\0"
                "@raw-io\0"
                "acct\0"
                "bdflush\0"
                "bpf\0"
                "capset\0"
                "chown32\0"
                "chown\0"
                "chroot\0"
                "fchown32\0"
                "fchown\0"
                "fchownat\0"
                "kexec_file_load\0"
                "kexec_load\0"
                "lchown32\0"
                "lchown\0"
                "nfsservctl\0"
                "pivot_root\0"
                "quotactl\0"
                "reboot\0"
                "setdomainname\0"
                "setfsuid32\0"
                "setfsuid\0"
                "setgroups32\0"
                "setgroups\0"
                "sethostname\0"
                "setresuid32\0"
                "setresuid\0"
                "setreuid32\0"
                "setreuid\0"
                "setuid32\0"
                "setuid\0"
                "swapoff\0"
                "swapon\0"
                "_sysctl\0"
                "vhangup\0"
        },
        [SYSCALL_FILTER_SET_PROCESS] = {
                /* Process control, execution, namespaces */
                .name = "@process",
                .value =
                "arch_prctl\0"
                "clone\0"
                "execveat\0"
                "fork\0"
                "kill\0"
                "prctl\0"
                "setns\0"
                "tgkill\0"
                "tkill\0"
                "unshare\0"
                "vfork\0"
        },
        [SYSCALL_FILTER_SET_RAW_IO] = {
                /* Raw I/O ports */
                .name = "@raw-io",
                .value =
                "ioperm\0"
                "iopl\0"
                "pciconfig_iobase\0"
                "pciconfig_read\0"
                "pciconfig_write\0"
#ifdef __NR_s390_pci_mmio_read
                "s390_pci_mmio_read\0"
#endif
#ifdef __NR_s390_pci_mmio_write
                "s390_pci_mmio_write\0"
#endif
        },
        [SYSCALL_FILTER_SET_RESOURCES] = {
                /* Alter resource settings */
                .name = "@resources",
                .value =
                "sched_setparam\0"
                "sched_setscheduler\0"
                "sched_setaffinity\0"
                "setpriority\0"
                "setrlimit\0"
                "set_mempolicy\0"
                "migrate_pages\0"
                "move_pages\0"
                "mbind\0"
                "sched_setattr\0"
                "prlimit64\0"
        },
};

const SyscallFilterSet *syscall_filter_set_find(const char *name) {
        unsigned i;

        if (isempty(name) || name[0] != '@')
                return NULL;

        for (i = 0; i < _SYSCALL_FILTER_SET_MAX; i++)
                if (streq(syscall_filter_sets[i].name, name))
                        return syscall_filter_sets + i;

        return NULL;
}

static int seccomp_add_syscall_filter_set(
                scmp_filter_ctx seccomp,
                uint32_t default_action,
                const SyscallFilterSet *set,
                uint32_t action) {

        const char *sys;
        int r;

        assert(seccomp);
        assert(set);

        NULSTR_FOREACH(sys, set->value) {
                int id;

                if (sys[0] == '@') {
                        const SyscallFilterSet *other;

                        other = syscall_filter_set_find(sys);
                        if (!other)
                                return -EINVAL;

                        r = seccomp_add_syscall_filter_set(seccomp, default_action, other, action);
                        if (r < 0)
                                return r;
                } else {
                        id = seccomp_syscall_resolve_name(sys);
                        if (id == __NR_SCMP_ERROR)
                                return -EINVAL; /* Not known at all? Then that's a real error */

                        r = seccomp_rule_add_exact(seccomp, action, id, 0);
                        if (r < 0)
                                /* If the system call is not known on this architecture, then that's fine, let's ignore it */
                                log_debug_errno(r, "Failed to add rule for system call %s, ignoring: %m", sys);
                }
        }

        return 0;
}

int seccomp_load_syscall_filter_set(uint32_t default_action, const SyscallFilterSet *set, uint32_t action) {
        uint32_t arch;
        int r;

        assert(set);

        /* The one-stop solution: allocate a seccomp object, add the specified filter to it, and apply it. Once for
         * earch local arch. */

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, default_action);
                if (r < 0)
                        return r;

                r = seccomp_add_syscall_filter_set(seccomp, default_action, set, action);
                if (r < 0) {
                        log_debug_errno(r, "Failed to add filter set, ignoring: %m");
                        continue;
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install filter set for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_load_syscall_filter_set_raw(uint32_t default_action, Set* set, uint32_t action) {
        uint32_t arch;
        int r;

        /* Similar to seccomp_load_syscall_filter_set(), but takes a raw Set* of syscalls, instead of a
         * SyscallFilterSet* table. */

        if (set_isempty(set) && default_action == SCMP_ACT_ALLOW)
                return 0;

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
                Iterator i;
                void *id;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, default_action);
                if (r < 0)
                        return r;

                SET_FOREACH(id, set, i) {
                        r = seccomp_rule_add_exact(seccomp, action, PTR_TO_INT(id) - 1, 0);
                        if (r < 0) {
                                /* If the system call is not known on this architecture, then that's fine, let's ignore it */
                                _cleanup_free_ char *n = NULL;

                                n = seccomp_syscall_resolve_num_arch(arch, PTR_TO_INT(id) - 1);
                                log_debug_errno(r, "Failed to add rule for system call %s, ignoring: %m", strna(n));
                        }
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install filter set for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_restrict_namespaces(unsigned long retain) {
        uint32_t arch;
        int r;

        if (log_get_max_level() >= LOG_DEBUG) {
                _cleanup_free_ char *s = NULL;

                (void) namespace_flag_to_string_many(retain, &s);
                log_debug("Restricting namespace to: %s.", strna(s));
        }

        /* NOOP? */
        if ((retain & NAMESPACE_FLAGS_ALL) == NAMESPACE_FLAGS_ALL)
                return 0;

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
                unsigned i;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, SCMP_ACT_ALLOW);
                if (r < 0)
                        return r;

                if ((retain & NAMESPACE_FLAGS_ALL) == 0)
                        /* If every single kind of namespace shall be prohibited, then let's block the whole setns() syscall
                         * altogether. */
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(setns),
                                        0);
                else
                        /* Otherwise, block only the invocations with the appropriate flags in the loop below, but also the
                         * special invocation with a zero flags argument, right here. */
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(setns),
                                        1,
                                        SCMP_A1(SCMP_CMP_EQ, 0));
                if (r < 0) {
                        log_debug_errno(r, "Failed to add setns() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                        continue;
                }

                for (i = 0; namespace_flag_map[i].name; i++) {
                        unsigned long f;

                        f = namespace_flag_map[i].flag;
                        if ((retain & f) == f) {
                                log_debug("Permitting %s.", namespace_flag_map[i].name);
                                continue;
                        }

                        log_debug("Blocking %s.", namespace_flag_map[i].name);

                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(unshare),
                                        1,
                                        SCMP_A0(SCMP_CMP_MASKED_EQ, f, f));
                        if (r < 0) {
                                log_debug_errno(r, "Failed to add unshare() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                break;
                        }

                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(clone),
                                        1,
                                        SCMP_A0(SCMP_CMP_MASKED_EQ, f, f));
                        if (r < 0) {
                                log_debug_errno(r, "Failed to add clone() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                break;
                        }

                        if ((retain & NAMESPACE_FLAGS_ALL) != 0) {
                                r = seccomp_rule_add_exact(
                                                seccomp,
                                                SCMP_ACT_ERRNO(EPERM),
                                                SCMP_SYS(setns),
                                                1,
                                                SCMP_A1(SCMP_CMP_MASKED_EQ, f, f));
                                if (r < 0) {
                                        log_debug_errno(r, "Failed to add setns() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                        break;
                                }
                        }
                }
                if (r < 0)
                        continue;

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install namespace restriction rules for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_protect_sysctl(void) {
        uint32_t arch;
        int r;

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, SCMP_ACT_ALLOW);
                if (r < 0)
                        return r;

                r = seccomp_rule_add_exact(
                                seccomp,
                                SCMP_ACT_ERRNO(EPERM),
                                SCMP_SYS(_sysctl),
                                0);
                if (r < 0) {
                        log_debug_errno(r, "Failed to add _sysctl() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                        continue;
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install sysctl protection rules for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_restrict_address_families(Set *address_families, bool whitelist) {

#if !SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
        uint32_t arch;
        int r;

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
                Iterator i;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, SCMP_ACT_ALLOW);
                if (r < 0)
                        return r;

                if (whitelist) {
                        int af, first = 0, last = 0;
                        void *afp;

                        /* If this is a whitelist, we first block the address families that are out of range and then
                         * everything that is not in the set. First, we find the lowest and highest address family in
                         * the set. */

                        SET_FOREACH(afp, address_families, i) {
                                af = PTR_TO_INT(afp);

                                if (af <= 0 || af >= af_max())
                                        continue;

                                if (first == 0 || af < first)
                                        first = af;

                                if (last == 0 || af > last)
                                        last = af;
                        }

                        assert((first == 0) == (last == 0));

                        if (first == 0) {

                                /* No entries in the valid range, block everything */
                                r = seccomp_rule_add_exact(
                                                seccomp,
                                                SCMP_ACT_ERRNO(EAFNOSUPPORT),
                                                SCMP_SYS(socket),
                                                0);
                                if (r < 0) {
                                        log_debug_errno(r, "Failed to add socket() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                        continue;
                                }

                        } else {

                                /* Block everything below the first entry */
                                r = seccomp_rule_add_exact(
                                                seccomp,
                                                SCMP_ACT_ERRNO(EAFNOSUPPORT),
                                                SCMP_SYS(socket),
                                                1,
                                                SCMP_A0(SCMP_CMP_LT, first));
                                if (r < 0) {
                                        log_debug_errno(r, "Failed to add socket() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                        continue;
                                }

                                /* Block everything above the last entry */
                                r = seccomp_rule_add_exact(
                                                seccomp,
                                                SCMP_ACT_ERRNO(EAFNOSUPPORT),
                                                SCMP_SYS(socket),
                                                1,
                                                SCMP_A0(SCMP_CMP_GT, last));
                                if (r < 0) {
                                        log_debug_errno(r, "Failed to add socket() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                        continue;
                                }

                                /* Block everything between the first and last entry */
                                for (af = 1; af < af_max(); af++) {

                                        if (set_contains(address_families, INT_TO_PTR(af)))
                                                continue;

                                        r = seccomp_rule_add_exact(
                                                        seccomp,
                                                        SCMP_ACT_ERRNO(EAFNOSUPPORT),
                                                        SCMP_SYS(socket),
                                                        1,
                                                        SCMP_A0(SCMP_CMP_EQ, af));
                                        if (r < 0)
                                                break;
                                }

                                if (r < 0) {
                                        log_debug_errno(r, "Failed to add socket() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                        continue;
                                }
                        }

                } else {
                        void *af;

                        /* If this is a blacklist, then generate one rule for
                         * each address family that are then combined in OR
                         * checks. */

                        SET_FOREACH(af, address_families, i) {

                                r = seccomp_rule_add_exact(
                                                seccomp,
                                                SCMP_ACT_ERRNO(EAFNOSUPPORT),
                                                SCMP_SYS(socket),
                                                1,
                                                SCMP_A0(SCMP_CMP_EQ, PTR_TO_INT(af)));
                                if (r < 0)
                                        break;
                        }

                        if (r < 0) {
                                log_debug_errno(r, "Failed to add socket() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                continue;
                        }
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install socket family rules for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }
#endif

        return 0;
}

int seccomp_restrict_realtime(void) {
        static const int permitted_policies[] = {
                SCHED_OTHER,
                SCHED_BATCH,
                SCHED_IDLE,
        };

        int r, max_policy = 0;
        uint32_t arch;
        unsigned i;

        /* Determine the highest policy constant we want to allow */
        for (i = 0; i < ELEMENTSOF(permitted_policies); i++)
                if (permitted_policies[i] > max_policy)
                        max_policy = permitted_policies[i];

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
                int p;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                r = seccomp_init_for_arch(&seccomp, arch, SCMP_ACT_ALLOW);
                if (r < 0)
                        return r;

                /* Go through all policies with lower values than that, and block them -- unless they appear in the
                 * whitelist. */
                for (p = 0; p < max_policy; p++) {
                        bool good = false;

                        /* Check if this is in the whitelist. */
                        for (i = 0; i < ELEMENTSOF(permitted_policies); i++)
                                if (permitted_policies[i] == p) {
                                        good = true;
                                        break;
                                }

                        if (good)
                                continue;

                        /* Deny this policy */
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(sched_setscheduler),
                                        1,
                                        SCMP_A1(SCMP_CMP_EQ, p));
                        if (r < 0) {
                                log_debug_errno(r, "Failed to add scheduler rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                continue;
                        }
                }

                /* Blacklist all other policies, i.e. the ones with higher values. Note that all comparisons are
                 * unsigned here, hence no need no check for < 0 values. */
                r = seccomp_rule_add_exact(
                                seccomp,
                                SCMP_ACT_ERRNO(EPERM),
                                SCMP_SYS(sched_setscheduler),
                                1,
                                SCMP_A1(SCMP_CMP_GT, max_policy));
                if (r < 0) {
                        log_debug_errno(r, "Failed to add scheduler rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                        continue;
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install realtime protection rules for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_memory_deny_write_execute(void) {

        uint32_t arch;
        int r;

        SECCOMP_FOREACH_LOCAL_ARCH(arch) {
                _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
                int filter_syscall = 0, block_syscall = 0, shmat_syscall = 0;

                log_debug("Operating on architecture: %s", seccomp_arch_to_string(arch));

                switch (arch) {

                case SCMP_ARCH_X86:
                        filter_syscall = SCMP_SYS(mmap2);
                        block_syscall = SCMP_SYS(mmap);

                        /* Note that shmat() isn't available on i386, where the call is multiplexed through ipc(). We
                         * ignore that here, which means there's still a way to get writable/executable memory, if an
                         * IPC key is mapped like this on i386. That's a pity, but no total loss. */
                        break;

                case SCMP_ARCH_X86_64:
                case SCMP_ARCH_X32:
                        filter_syscall = SCMP_SYS(mmap);
                        shmat_syscall = SCMP_SYS(shmat);
                        break;

                /* Please add more definitions here, if you port systemd to other architectures! */

#if !defined(__i386__) && !defined(__x86_64__)
#warning "Consider adding the right mmap() syscall definitions here!"
#endif
                }

                /* Can't filter mmap() on this arch, then skip it */
                if (filter_syscall == 0)
                        continue;

                r = seccomp_init_for_arch(&seccomp, arch, SCMP_ACT_ALLOW);
                if (r < 0)
                        return r;

                if (filter_syscall != 0)  {
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        filter_syscall,
                                        1,
                                        SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC|PROT_WRITE, PROT_EXEC|PROT_WRITE));
                        if (r < 0) {
                                _cleanup_free_ char *n = NULL;

                                n = seccomp_syscall_resolve_num_arch(arch, filter_syscall);
                                log_debug_errno(r, "Failed to add %s() rule for architecture %s, skipping: %m",
                                                strna(n),
                                                seccomp_arch_to_string(arch));
                                continue;
                        }
                }

                if (block_syscall != 0) {
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        block_syscall,
                                        0);
                        if (r < 0) {
                                _cleanup_free_ char *n = NULL;

                                n = seccomp_syscall_resolve_num_arch(arch, block_syscall);
                                log_debug_errno(r, "Failed to add %s() rule for architecture %s, skipping: %m",
                                                strna(n),
                                                seccomp_arch_to_string(arch));
                                continue;
                        }
                }

                r = seccomp_rule_add_exact(
                                seccomp,
                                SCMP_ACT_ERRNO(EPERM),
                                SCMP_SYS(mprotect),
                                1,
                                SCMP_A2(SCMP_CMP_MASKED_EQ, PROT_EXEC, PROT_EXEC));
                if (r < 0) {
                        log_debug_errno(r, "Failed to add mprotect() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                        continue;
                }

                if (shmat_syscall != 0) {
                        r = seccomp_rule_add_exact(
                                        seccomp,
                                        SCMP_ACT_ERRNO(EPERM),
                                        SCMP_SYS(shmat),
                                        1,
                                        SCMP_A2(SCMP_CMP_MASKED_EQ, SHM_EXEC, SHM_EXEC));
                        if (r < 0) {
                                log_debug_errno(r, "Failed to add shmat() rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
                                continue;
                        }
                }

                r = seccomp_load(seccomp);
                if (IN_SET(r, -EPERM, -EACCES))
                        return r;
                if (r < 0)
                        log_debug_errno(r, "Failed to install MemoryDenyWriteExecute= rule for architecture %s, skipping: %m", seccomp_arch_to_string(arch));
        }

        return 0;
}

int seccomp_restrict_archs(Set *archs) {
        _cleanup_(seccomp_releasep) scmp_filter_ctx seccomp = NULL;
        Iterator i;
        void *id;
        int r;

        /* This installs a filter with no rules, but that restricts the system call architectures to the specified
         * list. */

        seccomp = seccomp_init(SCMP_ACT_ALLOW);
        if (!seccomp)
                return -ENOMEM;

        SET_FOREACH(id, archs, i) {
                r = seccomp_arch_add(seccomp, PTR_TO_UINT32(id) - 1);
                if (r == -EEXIST)
                        continue;
                if (r < 0)
                        return r;
        }

        r = seccomp_attr_set(seccomp, SCMP_FLTATR_CTL_NNP, 0);
        if (r < 0)
                return r;

        return seccomp_load(seccomp);
}
