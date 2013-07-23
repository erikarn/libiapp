Libiapp - don't actually use this in anything!
==============================================

This is a very hacked up, FreeBSD-specific, not-very-correct network
application library which will eventually grow enough smarts to write
high-throughput networking applications with.

Thanks to?

* Netflix, Inc.

Now, the specifics:

* It's FreeBSD specific.  No, I'm not interested in making this
  work on Linux at the moment.  I'm using it to stress test kqueue
  and, when I get around to it, POSIX AIO, sendfile and other
  disk routines.

* I'll also add higher level primitives.  You shouldn't be using
  the network and disk event stuff.  You should be using higher
  level primitives, like:

  + read/write buffers/buffer lists from/to a socket
  + read/write buffers/buffer lists from/to a disk FD, from various
    offsets
  + Schedule a sendfile() from a given disk FD offset/length to
    the given socket FD
  + Schedule copying data from a given disk FD to a given disk FD
  + Schedule copying data from a given socket FD to a given socket
    FD (ie, there's no need for offsets)
  + Send message(s) from thread A to thread B, or broadcast them
    to all threads subscribed to a given event queue.

  By providing ONLY higher level APIs, the libiapp developer (ie, me)
  is free to start implementing more efficient data copying.
  For example, I'm free to use POSIX AIO or pools of threads to
  supply disk IO.  The caller doesn't need to care.  Or, I can
  introduce new syscalls to do efficient socket->socket copying.

  One of my main two aims:

  * Write a new syscall to do a non-blocking sendfile() between
    two sockets, for efficient web proxy style IO
  * Split sendfile() into multiple parts, driven by disk IO
    completion upcalls in the kernel; then modify the async
    sendfile library code to use this rather than a thread pool
    for blocking sendfile() calls.

* It's not thread aware.  Specifically, the architecture (for now!)
  runs simple code inside multiple threads.  They don't talk
  between each other.  At some point I'll teach things about basic
  thread-awareness, but ..

* It won't ever support the notion of adding, removing and processing
  events from arbitrary threads.  Ie, once an event/FD is in a thread,
  it must be managed from the thread.  If you wish to do things to it
  from another thread, you should schedule a callback to that object
  _in the destination thread_ in order to do the work.  This adds a
  little complexity but it dramatically reduces the amount of locking
  and lock order risks.

* .. ie, the only thing I'll (eventually) support is adding and
  cancelling a callback from a remote thread.  And by its very definition,
  you can't guarantee the thread cancellation will occur.

Anyway!

* src/srv implements a simple TCP server.  You connect to it and it
  sinks data.

* src/clt implements a simple TCP client.  It connects to the server
  and writes lots of data to it.

By default!

* 8 worker threads
* The client will create new sockets at 128 sockets a second per thread
  up until 4096 sockets per thread.
* Yes, this means you'll end up with 32,768 sockets from the client
  to the server, trying to push data.

What's broken?

* There's limited error handling
* The whole listen/accept (TCP) and recvfrom (UDP) currently is inefficient.
  Specifically, I have multiple threads listening on the same incoming FD.
  I'm experimenting with different ways to do connection pinning and
  supporting that in FreeBSD.  I will eventually abstract this stuff out
  into libiapp so network applications don't have to setup sockets themselves.
* I'm 100% using oneshot events for now, purely for simplification of things.
  I'll eventually start supporting persistent events for things that
  make sense.
* Every event modification is calling kevent() once.  I'm not batching
  even updates.  Yes, it's terribly inefficient.  Yes, I should fix
  this.
