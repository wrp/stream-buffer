
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "ring-buffer.h"

#define DELAY_SIZE 1024

/*
 * Extremely naive stream buffering.  If an argument is given, it should
 * be an integer between 10 and 1000000, giving the number of microseconds
 * to wait between writes of each byte.  (So to produce an output stream
 * of 100 Bytes/sec, the first argument should be 10000.)  This is useful
 * when working with input from a regular file.  If no argument is given,
 * the input stream cannot be from a regular file.  The program will read
 * data for approximately one second and attempt to match the output rate
 * to the input rate.  As processing continues, if we get ahead or fall
 * behind the input stream, we dynamically adust the output rate in
 * increments of 5% to try to match the input data rate.
 */

char *msg[] = {
	"If given, the first argument must be an integer greater than 0",
	"and less than 1e6 which specifies the number of microseconds ",
	"between bytes of output.",
	NULL,
};
sig_atomic_t received_signal;
static int is_regular(int fd, const char *name);
static void set_timer(long long value_usec, long long interval_usec);
static long long reset_timer(long long interval, float resize_factor);

static void
stream_data(long long interval, struct ring_buf *rb)
{
	int c;
	int bytes_buffered = 0;
	set_timer(interval, interval);
	errno = 0;
	while( (c = getchar()) != EOF || errno == EINTR ){

		if( received_signal ){
			int d;
			bytes_buffered -= 1;
			if( (d = rb_pop(rb)) == EOF ){
				interval = reset_timer(interval, 1.05);
			} else {
				putchar(d);
				fflush(stdout);
			}
			received_signal = 0;
			errno = 0;
		}
		if( c != EOF ){
			bytes_buffered += 1;
			rb_xpush(rb, c);
		}
		if( bytes_buffered > DELAY_SIZE ){
			interval = reset_timer(interval, .95);
			bytes_buffered = 0;
		}
		if( bytes_buffered < -DELAY_SIZE ){
			interval = reset_timer(interval, 1.05);
			bytes_buffered = 0;
		}
	}
	/* input is finished, clear out the ring buffer */
	errno = 0;
	while( pause() && errno == EINTR && (c = rb_pop(rb)) != EOF ){
		putchar(c);
		fflush(stdout);
	}
}


void
handle(int sig, siginfo_t *i, void *v)
{
	(void)i;
	(void)v;
	(void)sig;
	received_signal = 1;
}

static void
set_timer(long long value_usec, long long interval_usec)
{
	struct timeval value = {.tv_sec = 0, .tv_usec = value_usec };
	struct timeval interval = {.tv_sec = 0, .tv_usec = interval_usec };
	struct itimerval t = {.it_interval=interval, .it_value=value };

	setitimer(ITIMER_REAL, &t, NULL);
}

static long long
reset_timer(long long interval, float resize_factor)
{
	if (is_regular(STDIN_FILENO, "stdin") ){
		/* When the input stream is a regular file, we rely solely
		 * on the command line argument to dictate the output rate
		 * and do not adjust the timer. */
		return interval;
	}
	long long new_interval = interval * resize_factor;
	if( new_interval < 10 ){
		fprintf(stderr, "maximum output rate is 100KB/sec\n");
		exit(1);
	}
	if( new_interval > 1e6 ){
		fprintf(stderr, "minimum output rate is 1B/sec\n");
		exit(1);
	}
	set_timer(new_interval, new_interval);
	return new_interval;
}

static void
set_handler()
{
	struct sigaction act;
	memset(&act, 0, sizeof act);
	act.sa_sigaction = handle;
	if (sigaction(SIGALRM, &act, NULL)) {
		perror("sigaction");
		exit(1);
	}
}

static int
is_regular(int fd, const char *name)
{
	struct stat s;
	if( fstat(fd, &s) ){
		perror(name);
		exit(1);
	}
	return S_ISREG(s.st_mode);
}

static long long
parse_argument_for_interval(const char *arg)
{
	char *end;
	long long rv = strtoll(arg, &end, 0);
	if (rv < 0 || rv > 999999 || *end) {
		fprintf(stderr, "Invalid argument.  ");
		for (char **m = msg; *m; m += 1) {
			fputs(*m, stderr);
			fputc('\n', stderr);
		}
	}
	return rv;
}

static long long
estimate_input_rate(struct ring_buf *rb)
{
	int c;
	size_t bytes_read = 0;

	/* Read one byte of data before starting the timer to allow
	* for upstream initializations */
	if ((c = getchar()) != EOF) {
		rb_xpush(rb, c);
	}
	set_timer(999999, 0);
	errno = 0;
	while ((c = getchar()) != EOF) {
		bytes_read += 1;
		rb_xpush(rb, c);
	}
	if( ! received_signal || bytes_read < 1 ){
		fprintf(stderr, "not enough input to estimate data rate\n");
		exit(1);
	}
	assert(bytes_read > 0 || errno == EINTR);
	received_signal = 0;
	if (bytes_read > 999999) {
		fprintf(stderr, "input data rate is too high\n");
		exit(1);
	}
	return 1e6 / bytes_read;
}


/*
 * Given the interval (microseconds per byte), read
 * enough data for approx. 1 second of data
 */
static void
read_one_second(long long interval, struct ring_buf *rb)
{
	int c;
	long long count = 1e06 / interval + 1;
	assert(interval > 0);
	assert(interval < 1e06);
	while (count-- > 0 && (c = getchar()) != EOF) {
		rb_xpush(rb, c);
	}
}


int
main(int argc, char **argv)
{
	long long interval;
	struct ring_buf *rb = rb_create(2048);

	set_handler();
	if (argc > 1) {
		interval = parse_argument_for_interval(argv[1]);
		read_one_second(interval, rb);
	} else if (is_regular(STDIN_FILENO, "stdin") ){
		fprintf(stderr, "Interval *must* be given if reading from ");
		fprintf(stderr, "a regular file!!\n");
		exit(1);
	} else {
		interval = estimate_input_rate(rb);
	}
	stream_data(interval, rb);

	return 0;
}
