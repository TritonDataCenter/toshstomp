# toshstomp: attempt to induce ROGUE-28

Build with:

    $ make

Run it on a regular file or device:

    $ mkfile 1g 1gfile 
    $ ./toshstomp 1gfile 
    toshstomp: operating on a regular file
    file: 1gfile
    size: 0x40000000
    using initial write LBA: 0x20000000
                    TIME  NREADS RDLATus  NWRITE WRLATus          WRLBA WR
    2017-10-26T18:40:11Z   77163     108   69678     133 0x000022360000  1
    2017-10-26T18:40:12Z  174618      87  181032     100 0x000038d62000  2
    2017-10-26T18:40:13Z  242427      94  256267     107 0x00003dcf8000  3
    2017-10-26T18:40:14Z  341263      88  362566     101 0x000031fe2000  5
    2017-10-26T18:40:15Z  432746      87  459171     100 0x000021612000  7
    ^C

Notes:

- 10 writers write to sequential LBAs starting halfway through the given file
  or device.  When reaching they end, they start again halfway through the file
  or device.
- 10 readers read from LBAs all over the given file or device.
- All read and write operations are 8K.

Stats are reported once per second:

    NREADS   total number of read operations so far
    RTLATus  average latency of all read operations so far, in microseconds
    NWRITE   total number of write operations so far
    WRLATus  average latency of all write operations so far, in microseconds
    WRLBA    LBA used for the next write operation
    WR       number of times the current write LBA has wrapped around
