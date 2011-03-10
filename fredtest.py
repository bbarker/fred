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

import sys
import fredapp
import fred.fredutil
import fred.dmtcpmanager
import fred.fredio

"""
This file should be executable from the command line to run several integration and unit tests on FReD.
"""

GS_PASSED_STRING = "Passed"
GS_FAILED_STRING = "Failed"

# Used for storing variable values between runs.
gd_stored_variables = {}
g_debugger = None

def start_session(l_cmd):
    """Start the given command line as a fred session."""
    global g_debugger
    g_debugger = fredapp.fred_setup(l_cmd)

def end_session():
    """End the current debugger session."""
    global g_debugger
    fred.fredutil.fred_teardown()
    fred.dmtcpmanager.manager_quit()
    g_debugger = None

def execute_commands(l_cmds):
    """Execute the given list of commands as if they were a source file."""
    fredapp.source_from_list(l_cmds)

def store_variable(s_name):
    """Evaluate given variable in debugger and store value."""
    global gd_stored_variables, g_debugger
    gd_stored_variables[s_name] = g_debugger.evaluate_expression(s_name)

def check_stored_variable(s_name):
    """Evaluate given variable in debugger and check against stored value."""
    global gd_stored_variables, g_debugger
    return gd_stored_variables[s_name] == g_debugger.evaluate_expression(s_name)

def check_variable(s_name, s_value):
    """Evaluate given variable in debugger and check against given value."""
    return s_value == str(g_debugger.evaluate_expression(s_name))

def print_test_name(s_name):
    print "%-30s | " % s_name,
    sys.stdout.flush()

def gdb_record_replay(n_count=1):
    """Run a test on deterministic record/replay on pthread-test example."""
    l_cmd = ["gdb", "../test-programs/pthread-test"]
    for i in range(0, n_count):
        print_test_name("gdb record/replay %d" % i)
        start_session(l_cmd)
        execute_commands(["b main", "r", "fred-ckpt", "c"])
        store_variable("solution")
        execute_commands(["fred-restart", "c"])
        if check_stored_variable("solution"):
            print GS_PASSED_STRING
        else:
            print GS_FAILED_STRING
        end_session()

def gdb_reverse_watch(n_count=1):
    """Run a reverse-watch test on test_list linked list example."""
    l_cmd = ["gdb", "../test-programs/test_list"]
    for i in range(0, n_count):
        print_test_name("gdb reverse watch %d" % i)
        start_session(l_cmd)
        execute_commands(["b main", "r", "fred-ckpt", "b 26",
                          "c", "fred-rw list_len(head) < 10"])
        if check_variable("list_len(head)", "9"):
            execute_commands(["n"])
            if check_variable("list_len(head)", "10"):
                print GS_PASSED_STRING
            else:
                print GS_FAILED_STRING
        else:
            print GS_FAILED_STRING
        end_session()
        
def run_integration_tests():
    """Run all available integration tests."""
    gdb_record_replay()
    gdb_reverse_watch()
    
def run_unit_tests():
    """Run all available unit tests."""
    pass

def run_all_tests():
    """Run all available test suites."""
    print "%-30s | %-15s" % ("Test name", "Result")
    print "-" * 31 + "+" + "-" * 15
    # TODO: This is hackish. Used to hide fred_info() messages.
    fred.fredio.gb_hide_output = True
    run_integration_tests()
    run_unit_tests()

if __name__ == "__main__":
    run_all_tests()