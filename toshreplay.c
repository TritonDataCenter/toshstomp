/*
 * Copyright 2017, Joyent, Inc.
 */

/*
 * toshreplay.c: Takes a file containing an I/O pattern and a device as
 * arguments, replaying the I/O pattern on the specified device.
 */

#include <err.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <alloca.h>
#include <limits.h>
#include <sys/param.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#define	TSH_NTHREADS	100

#define	TSH_TOK_IOSTART		" -> "
#define	TSH_TOK_READ		" type=R "
#define	TSH_TOK_WRITE		" type=W "
#define	TSH_TOK_BLKNO		" blkno="
#define	TSH_TOK_SIZE		" size="

#define	TSH_BUFSHIFT    17	/* max buffer size of 128K bytes */
#define	TSH_BUFMASK	(tsh_bufsz - 1)
#define	TSH_NWORKERS	128

typedef struct tsh_op {
	boolean_t	tsho_read;		/* boolean: is read */
	off_t		tsho_offset;		/* offset for op */
	off_t		tsho_size;		/* size for op */
	hrtime_t	tsho_sched;		/* scheduled start time */
	hrtime_t	tsho_start;		/* actual start time */
	hrtime_t	tsho_done;		/* completion time */
	int		tsho_outr;		/* outstanding reads */
	int		tsho_outw;		/* outstanding writes */
	int		tsho_doner;		/* outstanding reads on done */
	int		tsho_donew;		/* outstanding writes on done */
	pthread_t	tsho_worker;		/* processing worker */
	struct tsh_op	*tsho_next;		/* next operation */
	struct tsh_op	*tsho_nextstart;	/* next started operation */
	struct tsh_op	*tsho_nextdone;		/* next completed operation */
} tsh_op_t;

typedef struct tsh_worker {
	pthread_t	tshw_id;		/* thread ID of worker */
	tsh_op_t	*tshw_op;		/* operation */
	pthread_cond_t	tshw_cv;		/* worker's cond variable */
	struct tsh_worker *tshw_next;		/* next worker */
} tsh_worker_t;

static char *tsh_buffer;			/* buffer to write */
off_t tsh_bufsz = (1 << TSH_BUFSHIFT); 		/* size of buffer */

static int tsh_fd;				/* disk/file fd */
static off_t tsh_size;				/* size of disk/file */
static tsh_op_t *tsh_first;			/* first operation */
static tsh_op_t *tsh_last;			/* last operation */
static tsh_op_t *tsh_firststart;		/* first op to start */
static tsh_op_t *tsh_laststart;			/* last op to start */
static tsh_op_t *tsh_firstdone;			/* first op to complete */
static tsh_op_t *tsh_lastdone;			/* last op to complete */
static FILE *tsh_log;
static pthread_mutex_t tsh_worker_lock = PTHREAD_MUTEX_INITIALIZER;
static int tsh_nworkers = TSH_NWORKERS;		/* number of workers */
static tsh_worker_t *tsh_workers;
static int tsh_readers;
static int tsh_writers;
static hrtime_t tsh_cap = 120 * NANOSEC;
static hrtime_t tsh_start;			/* start of replay */
static boolean_t tsh_clamp = B_FALSE;

static void
usage(void)
{
	(void) fprintf(stderr, "usage: toshreplay "
	    "DEVICE_OR_FILE < REPLAY_FILE\n");
	exit(2);
}

static void
init_buffer(char *buf, size_t bufsz)
{
	size_t i;
	char c;

	c = 'A';
	for (i = 0; i < bufsz; i++) {
		buf[i] = c;
		if (++c == 'Z') {
			c = 'A';
		}
	}
}

long long
read_field(int lineno, char *line, char *field)
{
	char *start, *end;
	long long rval;

	if ((start = strstr(line, field)) == NULL)
		errx(1, "line %d: missing required field '%s'", lineno, field);

	start += strlen(field);
	errno = 0;
	rval = strtoll(start, &end, 10);

	if (errno != 0)
		err(1, "line %d: illegal value for field '%s'", lineno, field);

	if (*end != ' ' && (*end != '\0' || end == start))
		errx(1, "line %d: invalid value for field '%s'", lineno, field);

	return (rval);
}

void
read_log(void)
{
	char line[LINE_MAX];
	int nops = 0, nreads = 0;
	int lineno = 0;
	char *end;

	if (isatty(fileno(tsh_log)))
		errx(1, "replay log cannot be a terminal");

	while (fgets(line, sizeof (line), tsh_log)) {
		tsh_op_t *op;

		lineno++;

		if (strstr(line, TSH_TOK_IOSTART) == NULL)
			continue;

		if ((op = malloc(sizeof (tsh_op_t))) == NULL)
			err(1, "could not allocate new operation");

		bzero(op, sizeof (tsh_op_t));

		if (strstr(line, TSH_TOK_READ) != NULL) {
			op->tsho_read = B_TRUE;
		} else if (strstr(line, TSH_TOK_WRITE) == NULL) {
			err(1, "line %d: could not "
			    "determine I/O type", lineno);
		}

		/*
		 * Now pull the time offset -- this should be the first
		 * field.
		 */
		errno = 0;

		op->tsho_sched = strtoll(line, &end, 10);

		if (errno != 0)
			err(1, "line %d: illegal time offset", lineno);

		if (*end != ' ')
			errx(1, "line %d: invalid time offset", lineno);

		op->tsho_offset =
		    read_field(lineno, line, TSH_TOK_BLKNO) * DEV_BSIZE;
		op->tsho_size = read_field(lineno, line, TSH_TOK_SIZE);

		if (op->tsho_offset + op->tsho_size > tsh_size) {
			if (tsh_clamp) {
				off_t clamp = tsh_size - op->tsho_size;
				clamp &= ~(DEV_BSIZE - 1);

				warnx("line %d: offset %ld exceeds %ld; "
				    "clamped to %ld", lineno, op->tsho_offset,
				    tsh_size, clamp);

				op->tsho_offset = clamp;
			} else {
				errx(1, "line %d: offset %ld exceeds size "
				    "(%ld)", lineno, op->tsho_offset, tsh_size);
			}
		}

		if (op->tsho_read)
			nreads++;

		nops++;

		if (tsh_first == NULL) {
			tsh_first = op;
		} else {
			tsh_last->tsho_next = op;
		}

		tsh_last = op;

		if (op->tsho_sched > tsh_cap)
			break;
	}

	printf("%s: %d operations (%d reads, %d writes)\n", "toshreplay",
	    nops, nreads, nops - nreads);
}

void
tsh_read(off_t offset, off_t bufsize)
{
	char *buf = alloca(bufsize);
	int nread;

	nread = pread(tsh_fd, buf, bufsize, offset);

	if (nread < 0) {
		warn("pread lba 0x%x", offset);
	} else if (nread != bufsize) {
		warnx("pread lba 0x%x reported %d bytes\n", offset);
	}
}

void
tsh_write(off_t offset, off_t bufsize)
{
	ssize_t nwritten = pwrite(tsh_fd, tsh_buffer, bufsize, offset);

	if (nwritten < 0) {
		warn("pwrite lba 0x%x", offset);
	} else if (nwritten != bufsize) {
		warnx("pwrite lba 0x%x reported %d bytes\n", offset);
	}
}

void
tsh_worker(tsh_worker_t *me)
{
	pthread_mutex_lock(&tsh_worker_lock);

	for (;;) {
		tsh_op_t *op;

		me->tshw_op = NULL;
		me->tshw_next = tsh_workers;
		tsh_workers = me;

		pthread_cond_wait(&me->tshw_cv, &tsh_worker_lock);

		op = me->tshw_op;

		op->tsho_outr = tsh_readers;
		op->tsho_outw = tsh_writers;

		/*
		 * We have something to do!
		 */
		if (op->tsho_read) {
			tsh_readers++;
		} else {
			tsh_writers++;
		}

		op->tsho_start = gethrtime();

		if (tsh_firststart == NULL) {
			tsh_firststart = op;
		} else {
			tsh_laststart->tsho_nextstart = op;
		}

		tsh_laststart = op;

		pthread_mutex_unlock(&tsh_worker_lock);
		op->tsho_worker = me->tshw_id;

		if (op->tsho_read) {
			tsh_read(op->tsho_offset, op->tsho_size);
		} else {
			tsh_write(op->tsho_offset, op->tsho_size);
		}

		pthread_mutex_lock(&tsh_worker_lock);

		op->tsho_done = gethrtime();
		op->tsho_doner = tsh_readers;
		op->tsho_donew = tsh_writers;

		if (tsh_firstdone == NULL) {
			tsh_firstdone = op;
		} else {
			tsh_lastdone->tsho_nextdone = op;
		}

		tsh_lastdone = op;

		if (op->tsho_read) {
			tsh_readers--;
		} else {
			tsh_writers--;
		}
	}
}

void
tsh_dispatcher()
{
	tsh_op_t *op = tsh_first;
	tsh_worker_t *worker;
	tsh_start = gethrtime();

	while (op != NULL) {
		hrtime_t sched = op->tsho_sched + tsh_start;

		while (gethrtime() < sched)
			continue;

		pthread_mutex_lock(&tsh_worker_lock);

		/*
		 * We have an operation to dispatch -- take our next available
		 * thread.
		 */
		if ((worker = tsh_workers) == NULL) {
			errx(1, "ran out of workers at time offset %ld\n",
			    op->tsho_sched);
		}

		worker->tshw_op = op;
		tsh_workers = worker->tshw_next;

		/*
		 * We drop the lock before signalling the worker to assure
		 * that it will get the lock and therefore not induce
		 * unnecessary scheduling delay.
		 */
		pthread_mutex_unlock(&tsh_worker_lock);
		pthread_cond_signal(&worker->tshw_cv);

		op = op->tsho_next;
	}
}

void
tsh_dump()
{
	tsh_op_t *issued, *done, *op;

	/*
	 * We have two lists, both are tautologically sorted.  Print them
	 * out in an absolute order.
	 */
	issued = tsh_firststart;
	done = tsh_firstdone;

	while (done != NULL) {
		if (issued != NULL && issued->tsho_start <= done->tsho_done) {
			op = issued;

			printf("%lld -> type=%c blkno=%ld "
			    "size=%ld outr=%d outw=%d schedlat=%lld\n",
			    op->tsho_start - tsh_start,
			    op->tsho_read ? 'R' : 'W',
			    op->tsho_offset / DEV_BSIZE,
			    op->tsho_size, op->tsho_outr, op->tsho_outw,
			    op->tsho_start - tsh_start - op->tsho_sched);

			issued = issued->tsho_nextstart;
		} else {
			op = done;

			printf("%lld <- type=%c blkno=%ld "
			    "size=%ld outr=%d outw=%d latency=%lld worker=%d\n",
			    op->tsho_done - tsh_start,
			    op->tsho_read ? 'R' : 'W',
			    op->tsho_offset / DEV_BSIZE,
			    op->tsho_size, op->tsho_doner, op->tsho_donew,
			    op->tsho_done - op->tsho_start, op->tsho_worker);

			done = done->tsho_nextdone;
		}
	}
}

int
main(int argc, char *argv[])
{
	struct stat st;
	char *file;
	int c, i;

	while ((c = getopt(argc, argv, "hct:")) != -1) {
		switch (c) {
		case 'c':
			tsh_clamp = B_TRUE;
			break;

		case 't': {
			char *end;

			tsh_nworkers = strtoul(optarg, &end, 10);

			if (*end != '\0')
				errx(1, "invalid number of threads");
			break;
		}

		default:
			usage();
		}
	}

	if (argc == optind)
		usage();

	if ((tsh_buffer = malloc(tsh_bufsz)) == NULL)
		err(1, "couldn't allocate write buffer");

	init_buffer(tsh_buffer, tsh_bufsz);

	if ((tsh_fd = open(file = argv[optind], O_RDWR)) < 0)
		err(1, "open \"%s\"", file);

	if (fstat(tsh_fd, &st) != 0)
		err(1, "fstat(%d) (\"%s\"):", tsh_fd, file);

	if (S_ISREG(st.st_mode)) {
		warnx("replaying I/O on a regular file");
	} else if (S_ISBLK(st.st_mode)) {
		errx(1, "refusing to operate on (buffered) block device\n");
	} else if (!S_ISCHR(st.st_mode)) {
		errx(1, "unsupported file type");
	}

	tsh_size = st.st_size;

	/*
	 * Create our workers before we read the replay log to give them
	 * plenty of time to be ready for work.
	 */
	for (i = 0; i < tsh_nworkers; i++) {
		tsh_worker_t *worker;

		if ((worker = malloc(sizeof (tsh_worker_t))) == NULL)
			err(1, "couldn't allocate worker");

		pthread_cond_init(&worker->tshw_cv, NULL);

		if (pthread_create(&worker->tshw_id, NULL,
		    (void *(*)(void *))tsh_worker, worker) != 0) {
			err(1, "couldn't create worker");
		}
	}

	tsh_log = stdin;
	read_log();

	tsh_dispatcher();
	tsh_dump();
	return (0);
}
