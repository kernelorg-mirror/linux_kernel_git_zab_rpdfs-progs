## Running

Compile the code with make. You will need sparse installed.  The sparse
package tends to depend on a lot of other packages, so you may want to
build and install your own.

The two main components of rpdfs are the devd storage userspace server
built here, and a kernel client module maintained elsewhere.

The devd process requires a listening address and a backing storage
device.  Optionally it can be given a file in which to write debugging
traces.

Example invocation:

```
	$ truncate -s 1G ~/tmp/dev
	$ ./devd/rpdfs-devd -d ~/tmp/dev -l 127.0.0.1:8081 -t /tmp/trace_devd
```

In another terminal:

```
	# git clone $somewhere
	# cd linux-*
	# make $runningconfig
	# make ./fs/rpdfs/rpdfs.ko && insmod ./fs/rpdfs/rpdfs.ko
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

This lets us use reasonably stand-alone kernel interfaces in userspace
(list.h, endian swapping, bitops, etc).

**shared/format-*.h**

These headers contain the structures and protocol constants that are
exposed to the world through storage on persistent media or by sending
over the network.
