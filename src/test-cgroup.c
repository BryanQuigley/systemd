/*-*- Mode: C; c-basic-offset: 8 -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <string.h>

#include <libcgroup.h>

#include "cgroup-util.h"
#include "util.h"
#include "log.h"

int main(int argc, char*argv[]) {
        char *path;

        assert_se(cgroup_init() == 0);

        assert_se(cg_create("name=systemd", "/test-a") == 0);
        assert_se(cg_create("name=systemd", "/test-a") == 0);
        assert_se(cg_create("name=systemd", "/test-b") == 0);
        assert_se(cg_create("name=systemd", "/test-b/test-c") == 0);
        assert_se(cg_create_and_attach("name=systemd", "/test-b", 0) == 0);

        assert_se(cg_get_by_pid("name=systemd", getpid(), &path) == 0);
        assert_se(streq(path, "/test-b"));
        free(path);

        assert_se(cg_attach("name=systemd", "/test-a", 0) == 0);

        assert_se(cg_get_by_pid("name=systemd", getpid(), &path) == 0);
        assert_se(path_equal(path, "/test-a"));
        free(path);

        assert_se(cg_create_and_attach("name=systemd", "/test-b/test-d", 0) == 0);

        assert_se(cg_get_by_pid("name=systemd", getpid(), &path) == 0);
        assert_se(path_equal(path, "/test-b/test-d"));
        free(path);

        assert_se(cg_get_path("name=systemd", "/test-b/test-d", NULL, &path) == 0);
        assert_se(path_equal(path, "/cgroup/systemd/test-b/test-d"));
        free(path);

        assert_se(cg_is_empty("name=systemd", "/test-a", false) > 0);
        assert_se(cg_is_empty("name=systemd", "/test-b", false) > 0);
        assert_se(cg_is_empty_recursive("name=systemd", "/test-a", false) > 0);
        assert_se(cg_is_empty_recursive("name=systemd", "/test-b", false) == 0);

        assert_se(cg_kill_recursive("name=systemd", "/test-a", 0, false) == 0);
        assert_se(cg_kill_recursive("name=systemd", "/test-b", 0, false) > 0);

        assert_se(cg_migrate_recursive("name=systemd", "/test-b", "/test-a", false) == 0);

        assert_se(cg_is_empty_recursive("name=systemd", "/test-a", false) == 0);
        assert_se(cg_is_empty_recursive("name=systemd", "/test-b", false) > 0);

        assert_se(cg_kill_recursive("name=systemd", "/test-a", 0, false) > 0);
        assert_se(cg_kill_recursive("name=systemd", "/test-b", 0, false) == 0);

        cg_trim("name=systemd", "/", false);

        assert_se(cg_delete("name=systemd", "/test-b") < 0);
        assert_se(cg_delete("name=systemd", "/test-a") == 0);

        return 0;
}
