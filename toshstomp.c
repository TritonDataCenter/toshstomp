/*
 * toshstomp.c: generates an I/O pattern that attempts to induce ROGUE-28.
 */

#include <err.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <alloca.h>

#define	TSH_NWRITERS	10
#define	TSH_NREADERS	10
#define	TSH_BUFSHIFT    13	/* default buffer size of 8192 bytes */
#define	TSH_BUFMASK	(tsh_bufsz - 1)

/* reporting interval */
static unsigned int tsh_report_msec = 1000;

/* buffer of data that we will write out */
static char *tsh_buffer;
/* size of buffer */
off_t tsh_bufsz = (1 << TSH_BUFSHIFT);
/* identifiers for the threads we create */
static pthread_t *tsh_threads;
/* file descriptor for the disk or file that we're operating on */
static int tsh_fd;
/* size of the disk or file that we're working on */
static off_t tsh_size;
/* initial LBA used for writes */
static off_t tsh_write_lba_init;
/* current LBA used for writes */
static off_t tsh_write_lba_current;
/* number of times the write LBA has wrapped to the start */
static unsigned int tsh_write_lba_wraparounds;
/* lock that protects tsh_write_lba_current */
static pthread_mutex_t tsh_write_lba_lock = PTHREAD_MUTEX_INITIALIZER;

/* total number of reads completed */
static int tsh_nreads;
/* time spent reading */
static hrtime_t tsh_time_reading;
/* total number of writes completed */
static int tsh_nwrites;
/* time spent writing */
static hrtime_t tsh_time_writing;

static void usage(void);
static void init_buffer(char *, size_t);
static void *tsh_thread_writer(void *);
static void *tsh_thread_reader(void *);

int
main(int argc, char *argv[])
{
	struct stat st;
	unsigned int i;
	int error;
	unsigned int nwriters = TSH_NWRITERS;
	unsigned int nreaders = TSH_NREADERS;
	char timebuf[25];
	char *file;
	int c;

	while ((c = getopt(argc, argv, "b:r:w:")) != -1) {
		char *end;

		switch (c) {
		case 'b': {
			int bufshift = strtoul(optarg, &end, 10);

			if (*end != '\0' || (bufshift < 9))
				errx(1, "invalid buffer shift");

			tsh_bufsz = (1 << bufshift);
			break;
		}

		case 'r':
			nreaders = strtoul(optarg, &end, 10);

			if (*end != '\0')
				errx(1, "invalid number of readers");

			break;

		case 'w':
			nwriters = strtoul(optarg, &end, 10);

			if (*end != '\0')
				errx(1, "invalid number of writers");

			break;

		default:
			usage();
		}
	}

	if (argc == optind) {
		usage();
	}

	if ((tsh_buffer = malloc(tsh_bufsz)) == NULL)
		err(1, "couldn't allocate write buffer");

	init_buffer(tsh_buffer, tsh_bufsz);
	tsh_fd = open(file = argv[optind], O_RDWR);
	if (tsh_fd < 0) {
		err(1, "open \"%s\"", file);
	}

	if (fstat(tsh_fd, &st) != 0) {
		err(1, "fstat(%d) (\"%s\"):", tsh_fd, file);
	}

	if (S_ISREG(st.st_mode)) {
		warnx("operating on a regular file");
	} else if (S_ISBLK(st.st_mode)) {
		errx(1, "refusing to operate on (buffered) block device\n");
	} else if (!S_ISCHR(st.st_mode)) {
		errx(1, "unsupported file type");
	}

	tsh_size = st.st_size;
	tsh_write_lba_init = (tsh_size / 2) & (~TSH_BUFMASK);
	tsh_write_lba_current = tsh_write_lba_init;

	if (tsh_size < tsh_bufsz) {
		errx(1, "file is too small");
	}

	tsh_threads = malloc((nwriters + nreaders) * sizeof (pthread_t));

	if (tsh_threads == NULL)
		err(1, "couldn't allocate thread buffer");

	(void) printf("file: %s\n", file);
	(void) printf("size: 0x%lx\n", tsh_size);
	(void) printf("buffer size: %ld\n", tsh_bufsz);
	(void) printf("writers: %d\n", nwriters);
	(void) printf("readers: %d\n", nreaders);
	(void) printf("using initial write LBA: 0x%lx\n", tsh_write_lba_init);

	for (i = 0; i < nwriters; i++) {
		error = pthread_create(&tsh_threads[i], NULL,
		    tsh_thread_writer, (void *)(uintptr_t)i);
		if (error != 0) {
			err(1, "pthread_create");
		}
	}

	for (i = 0; i < nreaders; i++) {
		error = pthread_create(&tsh_threads[i + nwriters], NULL,
		    tsh_thread_reader, (void *)(uintptr_t)i);
		if (error != 0) {
			err(1, "pthread_create");
		}
	}

	(void) printf("%20s %7s %7s %7s %7s %14s %2s\n", "TIME",
	    "NREADS", "RDLATus", "NWRITE", "WRLATus", "WRLBA", "WR");

	for (;;) {
		time_t now;
		struct tm nowtm;

		(void) usleep(tsh_report_msec * 1000);
		/* XXX check buffer overflow conditions */
		(void) time(&now);
		(void) gmtime_r(&now, &nowtm);
		(void) strftime(timebuf, sizeof (timebuf), "%FT%TZ", &nowtm);

		(void) printf("%20s %7d %7ld %7d %7ld 0x%012lx %2d\n", timebuf,
		    tsh_nreads, tsh_nreads ? (unsigned long)
		    (tsh_time_reading / tsh_nreads / 1000) : 0,
		    tsh_nwrites, tsh_nwrites ? (unsigned long)
		    (tsh_time_writing / tsh_nwrites / 1000) : 0,
		    tsh_write_lba_current, tsh_write_lba_wraparounds);
	}

	/*
	 * Note: nothing currently causes these threads to terminate, or for us
	 * to get here.
	 */
	for (i = 0; i < TSH_NWRITERS + TSH_NREADERS; i++) {
		/* XXX VERIFY return value */
		(void) pthread_join(tsh_threads[i], NULL);
	}

	return (0);
}

static void
usage(void)
{
	(void) fprintf(stderr, "usage: toshstomp [-r #readers] "
	    "[-w #writers] [-b bufshift] DEVICE_OR_FILE\n");
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

static void *
tsh_thread_reader(void *whicharg __attribute__((__unused__)))
{
	char *buf = alloca(tsh_bufsz);
	off_t read_lba;
	int nread;
	hrtime_t start;

	for (;;) {
		read_lba = tsh_bufsz *
		    ((off_t)arc4random_uniform(tsh_size / tsh_bufsz));
		start = gethrtime();
		nread = pread(tsh_fd, buf, tsh_bufsz, read_lba);
		if (nread < 0) {
			warn("pread lba 0x%x", read_lba);
		} else if (nread != sizeof (buf)) {
			warnx("pread lba 0x%x reported %d bytes\n", read_lba);
		}
		tsh_time_reading += gethrtime() - start;
		tsh_nreads++;
	}

	return (NULL);
}

static void *
tsh_thread_writer(void *whicharg __attribute__((__unused__)))
{
	off_t write_lba;
	int nwritten;
	hrtime_t start;

	for (;;) {
		/*
		 * Using a lock here is cheesy, but expedient.
		 */
		(void) pthread_mutex_lock(&tsh_write_lba_lock);
		write_lba = tsh_write_lba_current;
		tsh_write_lba_current += tsh_bufsz;
		if (tsh_write_lba_current + tsh_bufsz >= tsh_size) {
			tsh_write_lba_current = tsh_write_lba_init;
			tsh_write_lba_wraparounds++;
		}
		(void) pthread_mutex_unlock(&tsh_write_lba_lock);

		start = gethrtime();
		nwritten = pwrite(tsh_fd, tsh_buffer, tsh_bufsz, write_lba);
		if (nwritten < 0) {
			warn("pwrite lba 0x%x", write_lba);
		} else if (nwritten != tsh_bufsz) {
			warnx("pwrite lba 0x%x reported %d bytes\n", write_lba);
		}
		tsh_time_writing += gethrtime() - start;
		tsh_nwrites++;
	}

	return (NULL);
}
