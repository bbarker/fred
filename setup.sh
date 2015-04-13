# MODIFY THIS FOR YOUR ACCOUNT.
# NOTE:  You must build your DMTCP using:   ./configure --with-record-replay
# You will also probably want to modify DMTCP_ROOT and to set DMTCP_PORT
#   to some port number that no one else on your machine is using.

# DMTCP_ROOT=/home/gene/dmtcp-ptrace
# DMTCP_ROOT=/home/gene/dmtcp-asplos
DMTCP_ROOT=/work/03135/bebarker/dmtcp-2.4.0-rc2
if [ -z "$DMTCP_ROOT" ]; then
    DMTCP_ROOT=/home/gene/dmtcp-vanilla2
fi

if echo $PATH | grep --quiet $DMTCP_ROOT/bin; then
  echo PATH already set 1>&2
else
  export PATH=$DMTCP_ROOT/bin:$PATH
fi

(cd test; make test-list 1>2&)
(cd record-replay; make)
rm -rf /tmp/fred.$USER

export set DMTCP_PORT=7760

echo ./fredapp.py --fred-demo gdb test/test-list
