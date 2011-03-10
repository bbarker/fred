#!/usr/bin/python

###############################################################################
# Copyright (C) 2009, 2010, 2011, 2012 by Kapil Arya, Gene Cooperman,         #
#                                        Tyler Denniston, and Ana-Maria Visan #
# {kapil,gene,tyler,amvisan}@ccs.neu.edu                                      #
#                                                                             #
# This file is part of FReD.                                                  #
#                                                                             #
# FReD is free software: you can redistribute it and/or modify                #
# it under the terms of the GNU General Public License as published by        #
# the Free Software Foundation, either version 3 of the License, or           #
# (at your option) any later version.                                         #
#                                                                             #
# FReD is distributed in the hope that it will be useful,                     #
# but WITHOUT ANY WARRANTY; without even the implied warranty of              #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the               #
# GNU General Public License for more details.                                #
#                                                                             #
# You should have received a copy of the GNU General Public License           #
# along with FReD.  If not, see <http://www.gnu.org/licenses/>.               #
###############################################################################

from optparse import OptionParser
import pdb
import os
import shutil
import signal
import sys

from fred import dmtcpmanager
from fred import fredio
from fred import freddebugger
from fred import fredutil

'''
STYLE CONVENTIONS
-----------------

We should follow the style conventions in PEP-8, with some additions:
  <http://www.python.org/dev/peps/pep-0008/>

The most obvious ones are:

 - No "camelCase" style should be used. Use the lower_case_with_underscore
   style for functions and variables.
 - Max line length 79 characters.
 - Indentation is 4 spaces (NOT literal tab (\t) characters)
 - Avoid extraneous whitespace such as:
     Yes: spam(ham[1], {eggs: 2})
     No:  spam( ham[ 1 ], { eggs: 2 } )

In addition, all global variables should have a prefix to indicate their
scope and type. The following prefixes will be used:

Prefix     |    Meaning
-----------+----------------------
g?_        |   global
-----------+----------------------
b_         |   boolean
d_         |   dictionary
f_         |   Python file object
l_         |   list
n_         |   numeric
s_         |   string

The ? after g_ is used to indicate the presence of an additional type
indicator.  For example a global integer should be name like:
  gn_a_global_number
To indicate global scope, and numeric type.

Whereever possible, local variables should also use a type prefix. Local scope
does not have or need a specific prefix.

All other non-basic types do not require a prefix other than scope-related.
For example, a mutex or thread object does not have a specific prefix.
However, the name of the variable should be descriptive enough to tell what
type it is meant to be.

Please make it a goal to document every public function with a docstring. It
only takes a few seconds, and saves a lot of time for future programmers. For
docstring conventions, you can see PEP-257.
'''


######################## Global Constants #####################################
GS_FRED_VERSION="Version: %prog 0.99-r277 (Nov. 12 2010)"
GS_FRED_USAGE="USAGE: %prog [options] xdb [ARGS] a.out [A.OUT ARGS]\n" + \
               "Replace `xdb' with the name of the target debugger"
GS_FRED_COMMAND_PREFIX="fred-"
GS_FRED_TMPDIR = '/tmp/fred.' + os.environ['USER']
######################## End Global Constants #################################

######################## Global Variables #####################################
# The global ReversibleDebugger instance.
g_debugger = None
g_source_script = None
gb_record_commands = False
gf_command_record_file = None
######################## End Global Variables #################################

def fred_command_help():
   print """FReD commands:
(all optional 'count' arguments default to 1)
  fred-undo [n]:         Undo last n debugger commands.
  fred-reverse-next [n], fred-rn [n]:  Reverse-next n times.
  fred-reverse-step [n], fred-rs [n]:  Reverse-step n times.
  fred-reverse-finish, fred-rf:        Reverse execute until function exited.
  fred-reverse-continue, fred-rc:      Reverse execute to previous breakpoint.
  fred-checkpoint, fred-ckpt: Request a new checkpoint to be made.
  fred-restart:               Restart from last checkpoint.
  fred-reverse-watch <EXPR>, fred-rw <EXPR>:
                              Reverse execute until expression EXPR changes.
  fred-source <FILE>:         Read commands from source file.
  fred-list:                  List the available checkpoint files.
  fred-help:                  Display this help message.
  fred-history:               Display your command history up to this point.
  fred-debug:                 (*Experts only) Drop into a pdb prompt for FReD.
  fred-start-record <FILE>:   Start recording your commands, writing to FILE.
  fred-stop-record:           Stop a running recording session.
"""
   sys.stdout.flush()

def is_quit_command(s_command):
    """Return True if s_command is a debugger 'quit' command."""
    return s_command in ["q", "quit", "exit"]

def handle_fred_command(s_command):
    """Performs handling of 'special' (non-debugger) commands."""
    global g_debugger, GS_FRED_COMMAND_PREFIX
    s_command = s_command.replace(GS_FRED_COMMAND_PREFIX, "")
    (s_command_name, sep, s_command_args) = s_command.partition(' ')
    n_count = fredutil.to_int(s_command_args, 1)
    if is_quit_command(s_command_name):
        fredutil.fred_quit(0)
    elif s_command_name == "undo":
        g_debugger.undo(n_count)
    elif s_command_name in ["reverse-next", "rn"]:
        g_debugger.reverse_next(n_count)
    elif s_command_name in ["reverse-step", "rs"]:
        g_debugger.reverse_step(n_count)
    elif s_command_name in ["reverse-finish", "rf"]:
        g_debugger.reverse_finish()
    elif s_command_name in ["reverse-continue", "rc"]:
        g_debugger.reverse_continue()
    elif s_command_name in ["checkpoint", "ckpt"]:
        g_debugger.do_checkpoint()
    elif s_command_name == "restart":
        # n_count defaults to 1 if no argument given (not appropriate here)
        n_index = fredutil.to_int(s_command_name, 0)
        g_debugger.do_restart(n_index, b_clear_history=True)
    elif s_command_name in ["reverse-watch", "rw"]:
        g_debugger.reverse_watch(s_command_args)
    elif s_command_name == "source":
        source_from_file(s_command_args)
    elif s_command_name == "list":
        print g_debugger.list_checkpoints()
    elif s_command_name == "help":
        fred_command_help()
    elif s_command_name == "history":
        print g_debugger.history()
    elif s_command_name == "debug":
        pdb.set_trace()
    elif s_command_name == "start-record":
       start_command_recording(s_command_args)
    elif s_command_name == "stop-record":
       stop_command_recording()
    else:
        fredutil.fred_error("Unknown FReD command '%s'" % s_command_name)

def source_from_file(s_filename):
    """Execute commands from given file."""
    fredutil.fred_debug("Start sourcing from file '%s'" % s_filename)
    try:
        f = open(s_filename)
    except IOError as (errno, strerror):
        fredutil.fred_error("Error opening source file '%s': %s" % \
                           (s_filename, strerror))
        return
    for s_line in f:
        s_line = s_line.strip()
        if is_fred_command(s_line):
            handle_fred_command(s_line)
        else:
            fredio.send_command(s_line)
            g_debugger.log_command(s_line)
    f.close()
    fredutil.fred_debug("Finished sourcing from file '%s'" % s_filename)

def source_from_list(ls_cmds):
    """Execute commands from given list."""
    for s_cmd in ls_cmds:
        s_cmd = s_cmd.strip()
        if is_fred_command(s_cmd):
            handle_fred_command(s_cmd)
        else:
            fredio.send_command(s_cmd)
            g_debugger.log_command(s_cmd)

def start_command_recording(s_filename):
    """Start recording all user commands to file."""
    global gb_record_commands, gf_command_record_file
    fredutil.fred_info("Starting command recording to file '%s'" % s_filename)
    try:
       gf_command_record_file = open(s_filename, 'a')
       gb_record_commands = True
    except IOError as e:
       fredutil.fred_error("Could not open file '%s': %s" %
                           (s_filename, str(e)))

def stop_command_recording():
    """Stop recording all user commands."""
    global gb_record_commands, gf_command_record_file
    if not gb_record_commands:
       fredutil.fred_info("Commands were not being recorded.")
       return
    fredutil.fred_info("Stopped command recording.")
    gf_command_record_file.close()
    gb_record_commands = False

def is_fred_command(s_command):
    """Return True if the given command needs special handling."""
    global GS_FRED_COMMAND_PREFIX
    return s_command.startswith(GS_FRED_COMMAND_PREFIX) or \
           is_quit_command(s_command)

def set_up_debugger(s_debugger_name):
    """Initialize global ReversibleDebugger instance g_debugger.
    Return the personality's find prompt function to pass to fredio."""
    global g_debugger
    if s_debugger_name == "gdb":
        fredutil.fred_debug("Using gdb personality.")
        from fred.personalityGdb import PersonalityGdb
        g_debugger = freddebugger.ReversibleDebugger(PersonalityGdb())
        del PersonalityGdb
    else:
        fredutil.fred_fatal("Unimplemented debugger '%s'" % s_debugger_name)
    return (g_debugger.get_find_prompt_function(),
            g_debugger.get_prompt_string_function())

def setup_environment_variables(s_dmtcp_port="7779", b_debug=False):
    """Set up the given environment variables."""
    os.environ['DMTCP_PORT'] = s_dmtcp_port
    if b_debug:
        fredutil.GB_DEBUG = True
    os.environ['DMTCP_TMPDIR'] = GS_FRED_TMPDIR

def parse_program_args():
    """Initialize command line options, and parse them.
    Return the user's inferior to execute as a list."""
    global GS_FRED_USAGE, g_source_script
    parser = OptionParser(usage=GS_FRED_USAGE, version=GS_FRED_VERSION)
    parser.disable_interspersed_args()
    # Note that '-h' and '--help' are supported automatically.
    default_port = os.getenv("DMTCP_PORT") or 7779
    parser.add_option("-p", "--port", dest="dmtcp_port", default=default_port,
                      help="Use PORT for DMTCP port number. (default %default)",
                      metavar="PORT")
    parser.add_option("-x", "--source", dest="source_script",
                      help="Execute batch file FILE", metavar="FILE")
    parser.add_option("--enable-debug", dest="debug", default=False,
                      action="store_true",
                      help="Enable FReD debugging messages.")
    # ./fredapp.py --fred-demo gdb a.out
    parser.add_option("--fred-demo", dest="fred_demo", default=False,
                      action="store_true",
                      help="Enable FReD demo.")
    (options, l_args) = parser.parse_args()
    # 'l_args' is the 'gdb ARGS ./a.out' list
    if len(l_args) == 0:
        parser.print_help()
        fredutil.fred_quit(1)
    if options.source_script != None:
        # Source script executed from main_io_loop().
        g_source_script = options.source_script
    if options.fred_demo:
        fredio.GB_FRED_DEMO = True
    setup_environment_variables(str(options.dmtcp_port), options.debug)
    return l_args

def interactive_debugger_setup():
    """Perform any debugger setup that requires a debugger prompt."""
    global g_debugger, g_source_script
    if g_debugger.personality_name() == "gdb":
        # Special case for gdb: record name of executable.
        g_debugger._p.set_inferior_name()
    # If the user gave a source script file, execute it now.
    if g_source_script != None:
        source_from_file(g_source_script)

def main_io_loop():
    """Main I/O loop to get and handle user commands."""
    global g_source_script, gb_record_commands, gf_command_record_file
    fredio.wait_for_prompt()
    interactive_debugger_setup()
    while 1:
        try:
            # Get one user command (blocking):
            s_command = fredio.get_command()
            # Special case: user just entered '\n', meaning execute last
            # command again. We want to preserve that functionality for fred
            # commands too.
            if s_command == '':
                s_command = s_last_command
            if gb_record_commands and s_command != "fred-stop-record":
                gf_command_record_file.write(s_command + '\n')
            fredutil.fred_timer_start(s_command)
            if is_fred_command(s_command):
                handle_fred_command(s_command)
                # TODO: Currently we do not log fred commands. Do we need to?
            else:
                fredio.send_command(s_command)
                g_debugger.log_command(s_command)
            fredutil.fred_timer_stop(s_command)
            s_last_command = s_command
        except KeyboardInterrupt:
            fredio.signal_child(signal.SIGINT)

def remove_fred_tmpdir():
    """Remove FReD temporary directory and contents."""
    fredutil.fred_debug("Removing temporary directory '%s'" % \
                        GS_FRED_TMPDIR)
    # Safety feature: assert that the directory contains "/tmp", just in case.
    fredutil.fred_assert(GS_FRED_TMPDIR.find("/tmp") != -1)
    shutil.rmtree(GS_FRED_TMPDIR, ignore_errors=True)

def cleanup_fred_files():
    """Remove any FReD-related temporary files."""
    remove_fred_tmpdir()
    dmtcpmanager.remove_manager_root()

def fred_setup(l_cmd=[]):
    """Perform any setup needed by FReD before entering an I/O loop."""
    global g_debugger
    # Remove any files from a previous run:
    cleanup_fred_files()
    # Parse arguments, if none were provided.
    if len(l_cmd) == 0:
        l_cmd = parse_program_args()
    else:
        # Command provided: set up env vars with default values.
        setup_environment_variables()
    # Set up the FReD global debugger
    (find_prompt_fnc, print_prompt_fnc) = set_up_debugger(l_cmd[0])
    # Set up I/O handling (with appropriate find_prompt())
    fredio.setup(find_prompt_fnc, print_prompt_fnc,
                 g_debugger.get_prompt_regex(), g_debugger.get_ls_needs_input(),
                 l_cmd)
    # Set up DMTCP manager
    dmtcpmanager.start(l_cmd, int(os.environ['DMTCP_PORT']))
    # We return the debugger instance for the sole purpose of fredtest.py.
    return g_debugger

def main():
    """Program execution starts here."""
    fred_setup()
    # Main input/output loop
    main_io_loop()
    # If we get here, quit.
    fredutil.fred_quit(0)
    
if __name__ == '__main__':
    main()