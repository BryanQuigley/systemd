/*-*- Mode: C; c-basic-offset: 8 -*-*/

#ifndef footimerhfoo
#define footimerhfoo

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

typedef struct Timer Timer;

#include "unit.h"

typedef enum TimerState {
        TIMER_DEAD,
        TIMER_WAITING,
        TIMER_RUNNING,
        _TIMER_STATE_MAX
} TimerState;

struct Timer {
        Meta meta;

        TimerState state;

        clockid_t clock_id;
        usec_t next_elapse;

        Service *service;
};

extern const UnitVTable timer_vtable;

#endif
