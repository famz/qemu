#!/usr/bin/env python
# Code generator for trace events
#
# Copyright IBM, Corp. 2011
#
# This work is licensed under the terms of the GNU GPL, version 2.  See
# the COPYING file in the top-level directory.
#
#

import sys
import getopt
import re

def error_write(*lines):
    sys.stderr.writelines(lines)

def error(*lines):
    error_write(*lines)
    sys.exit(1)

######################################################################
# formats

def nop_fn(events):
    pass

class Format(object):
    '''An output file format'''
    def __init__(self, description, begin, end=nop_fn):
        self.description = description
        self.begin = begin
        self.end = end

def h_begin(events):
    print '''#ifndef TRACE_H
#define TRACE_H

/* This file is autogenerated by tracetool, do not edit. */

#include "qemu-common.h"'''

def h_end(events):
    for e in events:
        if 'disable' in e.properties:
            enabled = 0
        else:
            enabled = 1
        print "#define TRACE_%s_ENABLED %d" % (e.name.upper(), enabled)
    print
    print '#endif /* TRACE_H */'

def c_begin(events):
    print '/* This file is autogenerated by tracetool, do not edit. */'

formats = {
    'h': Format("Generate .h file", h_begin, h_end),
    'c': Format("Generate .c file", c_begin),
    'd': Format("Generate .d file (DTrace probes)", c_begin),
    'stap': Format("Generate .stp file (SystemTAP tapsets)", c_begin),
}

######################################################################
# backends

class Backend(object):
    '''Tracing backend code generator'''
    def h(self, events):
        pass

    def c(self, events):
        pass

    def d(self, events):
        pass

    def stap(self, events):
        pass

class NopBackend(Backend):
    description = "Tracing disabled"

    def h(self, events):
        print
        for event in events:
            print '''static inline void trace_%(name)s(%(args)s)
{
}
''' % {
    'name': event.name,
    'args': event.args
}

class SimpleBackend(Backend):
    description = "Simple built-in backend"

    def h(self, events):
        print '#include "trace/simple.h"'
        print

        for num, event in enumerate(events):
            if len(event.args):
                argstr = event.args.names()
                arg_prefix = ', (uint64_t)(uintptr_t)'
                cast_args = arg_prefix + arg_prefix.join(argstr)
                simple_args = (str(num) + cast_args)
            else:
                simple_args = str(num)

            print '''static inline void trace_%(name)s(%(args)s)
{
    trace%(argc)d(%(trace_args)s);
}''' % {
    'name': event.name,
    'args': event.args,
    'argc': len(event.args),
    'trace_args': simple_args
}
            print
        print '#define NR_TRACE_EVENTS %d' % len(events)
        print 'extern TraceEvent trace_list[NR_TRACE_EVENTS];'

    def c(self, events):
        print '#include "trace.h"'
        print
        print 'TraceEvent trace_list[] = {'
        print

        for event in events:
            print '{.tp_name = "%(name)s", .state=0},' % {
                'name': event.name,
            }
            print
        print '};'

class StderrBackend(Backend):
    description = "Stderr built-in backend"

    def h(self, events):
        print '''#include <stdio.h>
#include "trace/stderr.h"

extern TraceEvent trace_list[];'''
        for num, event in enumerate(events):
            argnames = ", ".join(event.args.names())
            if len(event.args) > 0:
                argnames = ", " + argnames
            print '''
static inline void trace_%(name)s(%(args)s)
{
    if (trace_list[%(event_num)s].state != 0) {
        fprintf(stderr, "%(name)s " %(fmt)s "\\n" %(argnames)s);
    }
}''' % {
    'name': event.name,
    'args': event.args,
    'event_num': num,
    'fmt': event.fmt,
    'argnames': argnames
}
        print
        print '#define NR_TRACE_EVENTS %d' % len(events)

    def c(self, events):
        print '''#include "trace.h"

TraceEvent trace_list[] = {
'''
        for event in events:
            print '{.tp_name = "%(name)s", .state=0},' % {
                'name': event.name
            }
            print
        print '};'

class USTBackend(Backend):
    description = "LTTng User Space Tracing backend"

    def h(self, events):
        print '''#include <ust/tracepoint.h>
#undef mutex_lock
#undef mutex_unlock
#undef inline
#undef wmb'''

        for event in events:
            if len(event.args) > 0:
                print '''
DECLARE_TRACE(ust_%(name)s, TP_PROTO(%(args)s), TP_ARGS(%(argnames)s));
#define trace_%(name)s trace_ust_%(name)s''' % {
    'name': event.name,
    'args': event.args,
    'argnames': ", ".join(event.args.names())
}
            else:
                print '''
_DECLARE_TRACEPOINT_NOARGS(ust_%(name)s);
#define trace_%(name)s trace_ust_%(name)s''' % {
    'name': event.name,
}
        print

    def c(self, events):
        print '''#include <ust/marker.h>
#undef mutex_lock
#undef mutex_unlock
#undef inline
#undef wmb
#include "trace.h"'''
        for event in events:
            argnames = ", ".join(event.args.names())
            if len(event.args) > 0:
                argnames = ', ' + argnames
                print '''
DEFINE_TRACE(ust_%(name)s);

static void ust_%(name)s_probe(%(args)s)
{
    trace_mark(ust, %(name)s, %(fmt)s%(argnames)s);
}''' % {
    'name': event.name,
    'args': event.args,
    'fmt': event.fmt,
    'argnames': argnames
}
            else:
                print '''
DEFINE_TRACE(ust_%(name)s);

static void ust_%(name)s_probe(%(args)s)
{
    trace_mark(ust, %(name)s, UST_MARKER_NOARGS);
}''' % {
    'name': event.name,
    'args': event.args,
}

        # register probes
        print '''
static void __attribute__((constructor)) trace_init(void)
{'''
        for event in events:
            print '    register_trace_ust_%(name)s(ust_%(name)s_probe);' % {
                'name': event.name
            }
        print '}'

class DTraceBackend(Backend):
    description = "DTrace/SystemTAP backend"

    def h(self, events):
        print '#include "trace-dtrace.h"'
        print
        for event in events:
            print '''static inline void trace_%(name)s(%(args)s) {
    if (QEMU_%(uppername)s_ENABLED()) {
        QEMU_%(uppername)s(%(argnames)s);
    }
}
''' % {
    'name': event.name,
    'args': event.args,
    'uppername': event.name.upper(),
    'argnames': ", ".join(event.args.names()),
}

    def d(self, events):
        print 'provider qemu {'
        for event in events:
            args = event.args

            # DTrace provider syntax expects foo() for empty
            # params, not foo(void)
            if args == 'void':
                args = ''

            # Define prototype for probe arguments
            print '''
        probe %(name)s(%(args)s);''' % {
        'name': event.name,
        'args': args
}
        print
        print '};'

    def stap(self, events):
        for event in events:
            # Define prototype for probe arguments
            print '''
probe %(probeprefix)s.%(name)s = process("%(binary)s").mark("%(name)s")
{''' % {
    'probeprefix': probeprefix,
    'name': event.name,
    'binary': binary
}
            i = 1
            if len(event.args) > 0:
                for name in event.args.names():
                    # 'limit' is a reserved keyword
                    if name == 'limit':
                        name = '_limit'
                    print '  %s = $arg%d;' % (name.lstrip(), i)
                    i += 1
            print '}'
        print

backends = {
    "nop": NopBackend(),
    "simple": SimpleBackend(),
    "stderr": StderrBackend(),
    "ust": USTBackend(),
    "dtrace": DTraceBackend(),
}

def get_backend(fmt, backend):
    return getattr(backends[backend], fmt, nop_fn)

######################################################################
# Event arguments

class Arguments:
    def __init__ (self, arg_str):
        self._args = []
        for arg in arg_str.split(","):
            arg = arg.strip()
            parts = arg.split()
            head, sep, tail = parts[-1].rpartition("*")
            parts = parts[:-1]
            if tail == "void":
                assert len(parts) == 0 and sep == ""
                continue
            arg_type = " ".join(parts + [ " ".join([head, sep]).strip() ]).strip()
            self._args.append((arg_type, tail))

    def __iter__(self):
        return iter(self._args)

    def __len__(self):
        return len(self._args)

    def __str__(self):
        if len(self._args) == 0:
            return "void"
        else:
            return ", ".join([ " ".join([t, n]) for t,n in self._args ])

    def names(self):
        return [ name for _, name in self._args ]

    def types(self):
        return [ type_ for type_, _ in self._args ]

######################################################################
# A trace event

cre = re.compile("((?P<props>.*)\s+)?(?P<name>[^(\s]+)\((?P<args>[^)]*)\)\s*(?P<fmt>\".*)?")

VALID_PROPS = set(["disable"])

class Event(object):
    def __init__(self, line):
        m = cre.match(line)
        assert m is not None
        groups = m.groupdict('')
        self.name = groups["name"]
        self.fmt = groups["fmt"]
        self.properties = groups["props"].split()
        self.args = Arguments(groups["args"])

        unknown_props = set(self.properties) - VALID_PROPS
        if len(unknown_props) > 0:
            raise ValueError("Unknown properties: %s" % ", ".join(unknown_props))

# Generator that yields Event objects given a trace-events file object
def read_events(fobj):
    res = []
    for line in fobj:
        if not line.strip():
            continue
        if line.lstrip().startswith('#'):
	    continue
        res.append(Event(line))
    return res

######################################################################
# Main

binary = ""
probeprefix = ""

def usage():
    print "Tracetool: Generate tracing code for trace events file on stdin"
    print "Usage:"
    print sys.argv[0], " --format=<format> --backend=<backend>"
    print
    print "Output formats:"
    for f in formats:
        print "   %-10s %s" % (f, formats[f].description)
    print
    print "Backends:"
    for b in backends:
        print "   %-10s %s" % (b, backends[b].description)
    print """
Options:
  --binary       [path]    Full path to QEMU binary
  --target-arch  [arch]    QEMU emulator target arch
  --target-type  [type]    QEMU emulator target type ('system' or 'user')
  --probe-prefix [prefix]  Prefix for dtrace probe names
                           (default: qemu-targettype-targetarch)
"""

    sys.exit(1)

def main():
    global binary, probeprefix
    fmt = None
    backend = None
    targettype = ""
    targetarch = ""
    long_options = ["format=", "backend=", "binary=", "target-arch=", "target-type=", "probe-prefix=", "list-backends", "check-backend"]
    try:
        opts, args = getopt.getopt(sys.argv[1:], "", long_options)
    except getopt.GetoptError, err:
        # print help information and exit
        # will print something like "option -a not recognized"
        error_write(str(err)+"\n")
        usage()
        sys.exit(2)
    for opt, arg in opts:
        if opt == '--format':
            fmt = arg
        elif opt == '--backend':
            backend = arg
        elif opt == '--binary':
            binary = arg
        elif opt == '--target-arch':
            targetarch = arg
        elif opt == '--target-type':
            targettype = arg
        elif opt == '--probe-prefix':
            probeprefix = arg
        elif opt == '--list-backends':
            print ', '.join(backends)
            sys.exit(0)
        elif opt == "--check-backend":
            if backend in backends:
                sys.exit(0)
            else:
                sys.exit(1)
        else:
            print "unhandled option: ", opt
            usage()

    if fmt not in formats:
        error_write("Unknown format: %s\n\n" % fmt)
        usage()
    if backend not in backends:
        error_write("Unknown backend: %s\n\n" % backend)
        usage()
        sys.exit(0)

    if backend != 'dtrace' and fmt == 'd':
        error('DTrace probe generator not applicable to %s backend\n' % backend)

    if fmt == 'stap':
        if backend != "dtrace":
            error('SystemTAP tapset generator not applicable to %s backend\n' % backend)
        if binary == "":
            error("--binary is required for SystemTAP tapset generator\n")
        if not probeprefix and  not targettype:
            error("--target-type is required for SystemTAP tapset generator\n")
        if not probeprefix and  not targetarch:
            error("--target-arch is required for SystemTAP tapset generator\n")
        if probeprefix == "":
            probeprefix = 'qemu.' + targettype + '.' + targetarch

    events = read_events(sys.stdin)

    bfun = get_backend(fmt, backend)
    bnop = get_backend(fmt, "nop")

    formats[fmt].begin(events)
    bfun([ e for e in events if "disable" not in e.properties ])
    bnop([ e for e in events if "disable" in e.properties ])
    formats[fmt].end(events)

if __name__ == "__main__":
    main()

