## Running

Compile the code with make. You will need librcu and sparse installed.
The sparse package tends to depend on a lot of other packages, so you
may want to build and install your own.

rpdfs currently only runs in userspace. There are two components: the
devd process and the debugfs process.

The devd process listens on a socket for block IO requests. The
debugfs process reads commands from stdin and turns them into rpdfs
operations, which generate IO requests to the devd process. Thus you
need to tell the devd process where to listen and the debugfs process
where to connect. The devd process also needs to know where its
backing store is. Another required argument is a path to a file to
write trace data to.

Example invocation:

```
	$ truncate -s 1G ~/tmp/dev
	$ ./devd/rpdfs-devd -d ~/tmp/dev -l 127.0.0.1:8081 -t /tmp/trace_devd
```

In another terminal:

```
	# mount -t rpdfs -o mkfs 127.0.0.1:8081 /mnt
```

## Code Layout

**{cli,devd}/**

These directories contain the source for utility binaries.  They'll have
their own private source files and will link with all the shared code.

**shared/**

This is all the code that's shared by each utility.  It's not a proper
library in that it doesn't need to remain API compatible with external
builds over time.

**shared/lk/**

This is for userspace implementations of kernel interfaces.  This lets
us use reasonably stand-alone kernel interfaces (list.h) in userspace
by providing implementations of more complicated runtime services (RCU
hash tables, work queues).

**shared/format-{block,msg,trace}.h**

These headers contain the structures and protocol constants that are
exposed to the world through storage on persistent media or by sending
over the network.
