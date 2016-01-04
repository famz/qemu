#!/usr/bin/env python
#
# Block I/O trace analyzer
#
# Copyright 2016 Red Hat Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, see <http://www.gnu.org/licenses/>.

import sys
import simpletrace

def warn(msg):
    sys.stderr.write(msg + "\n")

class RequestTracker(object):
    class BIORequest(object):
        def __init__(self, tag, t):
            self.tag = tag
            self._t = t
            self.events = {}

        def add_event(self, event, t):
            if event in self.events:
                warn("duplicated event for 0x%016x" % self.tag)
                return
            self.events[event] = t

        def calculate_time(self, t):
            return t - self._t

    def __init__(self, name):
        self._inflight_reqs = {}
        self.total_requests = 0
        self.total_queue_depth = 0
        self.max_completion_time = 0
        self.min_completion_time = -1
        self.total_completion_time = 0
        self._total_time = 0
        self._cur_queue_depth = 0
        self.name = name

    def create_request(self, tag, t):
        self._inflight_reqs[tag] = BIORequest(tag, t)

    def complete_request(self, tag, t):
        r = self._inflight_reqs[tag]

    def event(self, tag, event, t):
        r = self._inflight_reqs[tag]
        r.add_event(event, t)

    def average_completion_time(self):
        if self.total_requests > 0:
            return self.total_completion_time / self.total_requests
        else:
            return 0

    def average_queue_depth(self):
        if self.total_requests > 0:
            return self.total_queue_depth / float(self.total_requests)
        else:
            return 0

class BIOTraceTracker(simpletrace.Analyzer):

    def __init__(self):
        self._req_trackers = {}
        self._trackers = {}

    def _get_tracker(self, name):
        if name not in self._trackers:
            self._trackers[name] = RequestTracker(name)
        return self._trackers[name]

    def begin(self):
        pass

    def bio_create(self, t, creator, tag):
        if tag in self._req_trackers:
            warn("trying to create duplicated tag %d" % tag)
            return
        tracker = self._get_tracker(creator)
        tracker.create_request(tag, t)
        self._req_trackers[tag] = tracker

    def bio_event(self, t, tag, event):
        if tag not in self._req_trackers:
            warn("event of unknown tag %d" % tag)
            return
        tracker = self._req_trackers[tag]
        tracker.event(tag, event, t)

    def bio_complete(self, t, tag):
        if tag not in self._req_trackers:
            warn("trying to complete unknown tag %d" % tag)
            return
        tracker = self._req_trackers[tag]
        tracker.complete_request(tag, t)
        del self._req_trackers[tag]

    def end(self):
        for tracker in self._trackers.values():
            print "%s:" % tracker.name
            print "  Requests:", tracker.total_requests
            print "  Average completion time (us):", tracker.average_completion_time() / 1000
            print "  Max completion time (us):", tracker.max_completion_time / 1000
            print "  Min completion time (us):", tracker.min_completion_time / 1000
            print "  Average queue depth: %.1f" % tracker.average_queue_depth()
            print

if __name__ == "__main__":
    simpletrace.run(BIOTraceTracker())
