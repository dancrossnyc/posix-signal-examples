/*
 * Example of using the "Self Pipe Trick" (https://cr.yp.to/docs/selfpipe.html)
 * with pthreads, without select or poll.
 *
 * The basic architecture is fairly simple: for every signal
 * that we need to handle, we create a) a pipe, b) a condition
 * variable, and c) a thread.  The thread runs a loop that
 * blocks on a read from the read-end of the pipe; when that
 * read returns, we broadcast on the condvar.
 *
 * We also set the handler for those signals to a function that
 * performs a (non-blocking) write to the write-end of the pipe;
 * when a signal is received, we write this byte which will
 * cause the read in the thread mentioned above to unblock and
 * return, which then signals the cond var.
 *
 * Dan Cross <net!gajendra!cross>
 */

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// This lists signals that we care about handling.
//
// If we were to sacrifice portability, much of this could be
// avoided: BSD-style systems offer a symbol called NSIG that is
// the total number of signals, and that number is guaranteed to
// be reasonably small; sadly, this is not in POSIX.  Similarly,
// BSD has an array called `sys_signame` that contains the short
// names of all signals.  Thus, on such systems, we could index
// directly by signal number, but we don't do that to be
// maximally portable.
static const struct {
	const int signum;
	const char *name;
} sigmap[] = {
	{ SIGINT,  "SIGINT"  },
	{ SIGTSTP, "SIGTSTP" },
	{ SIGQUIT, "SIGQUIT" },
	{ SIGHUP,  "SIGHUP"  },
	{ SIGUSR1, "SIGUSR1" },
};

#define	NSIGMAP (sizeof(sigmap) / sizeof(sigmap[0]))

// Maps from our internal signal index to the system's
// signal number.
static int
indexsignum(const size_t sigidx)
{
	assert(sigidx < NSIGMAP);
	return sigmap[sigidx].signum;
}

// Mapes from out internal signal index to a signal name,
// like `SIGINT` etc.
static const char *
indexsigname(const size_t sigidx)
{
	assert(sigidx < NSIGMAP);
	return sigmap[sigidx].name;
}

// Maps from signal number to map index.
static size_t
sigindex(const int signum)
{
	for (size_t k = 0; k < NSIGMAP; ++k) {
		if (sigmap[k].signum == signum)
			return k;
	}
	fprintf(stderr, "Unhandleable signal: %d\n", signum);
	_exit(EXIT_FAILURE);
}


// The auxilliary data required to handle each signal of
// interest includes the file descriptors for the read and write
// ends of the self pipe, a condition variable, and an ID for
// the thread looping on reads from the read end of the pipe.
typedef struct Aux Aux;
struct Aux {
	int pds[2];
	pthread_cond_t cv;
	pthread_t tid;
};

// The aux information for our signals of itnerest.
static Aux sigaux[NSIGMAP];

static void *
notifier(void *vaux)
{
	Aux *ap = vaux;

	assert(ap != NULL);
	for (;;) {
		char b;
		(void)read(ap->pds[0], &b, sizeof(b));
		pthread_cond_broadcast(&ap->cv);
	}

	return NULL;
}

static void
handler(int signum)
{
	const size_t si = sigindex(signum);
	const Aux *ap = &sigaux[si];
	char b = (char)signum;
	(void)write(ap->pds[1], &b, sizeof(b));
}

// Sets up a signal for handling.  Returns a pointer to a
// static, initialized condition variable that will be
// broadcast on when the given signal is received.
static pthread_cond_t *
initselfpipe(const size_t si)
{
	assert(si < NSIGMAP);

	Aux *ap = &sigaux[si];
	struct sigaction sa;

	// Create the self-pipe itself.
	if (pipe(ap->pds) < 0) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}

	// Set the write end of the pipe to be non-blocking.  This
	// isn't strictly necessary, as long as rate of incoming
	// signals doesn't exceed the reader's ability to signal them
	// on the condvar; with non-blocking IO on the write side, we
	// ensure that the signal handler will never block.  Note that
	// this implies that signal notifications can be lost (writes
	// to a full pipe will just fail and the notification for
	// any signals that the write was in response to will be
	// discarded.  But in practice this is not an issue since a)
	// signals are edge triggered anyway, and b) if we can't write
	// into the pipe this implies that there are existing
	// notification messages there already, so the receiver will
	// absorb notification of our event when it process those.
	if (fcntl(ap->pds[1], F_SETFL, O_NONBLOCK) < 0) {
		perror("fcntl(O_NONBLOCK)");
		exit(EXIT_FAILURE);
	}

	ap->cv = (pthread_cond_t)PTHREAD_COND_INITIALIZER;

	if (pthread_create(&ap->tid, NULL, notifier, ap) < 0) {
		perror("pthread_create");
		exit(EXIT_FAILURE);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	if (sigaction(indexsignum(si), &sa, NULL) < 0) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	return &ap->cv;
}

/*
 * A basic test harness.
 */
typedef struct SigThrData SigThrData;
struct SigThrData {
	pthread_cond_t *cv;
	const char *name;
	int signum;
	pthread_t tid;
};

void *
cvwaiter(void *arg)
{
	int times = 0;
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	SigThrData *data = arg;

	assert(data != NULL);

	pthread_mutex_lock(&mtx);
	for (;;) {
		pthread_cond_wait(data->cv, &mtx);
		printf("Recevied signal %s (%d)\n", data->name, data->signum);
		if (++times == 10) {
			printf("received %s %d times; bailing.\n", data->name, times);
			_exit(EXIT_SUCCESS);
		}
	}
}

static SigThrData sigdata[NSIGMAP];

void
initsigs(void)
{
	for (size_t k = 0; k < NSIGMAP; ++k) {
		SigThrData *data = &sigdata[k];
		data->cv = initselfpipe(k);
		assert(data->cv != NULL);
		data->name = indexsigname(k);
		data->signum = indexsignum(k);
		if (pthread_create(&data->tid, NULL, cvwaiter, data) < 0) {
			perror("pthread_create");
			exit(EXIT_FAILURE);
		}
	}
}

int
main(void)
{
	sigset_t all, set;

	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &set);
	initsigs();
	for (;;)
		sigsuspend(&set);

	return 0;
}
