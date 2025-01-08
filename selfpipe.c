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

// The signals that we care about handling.  We map between
// these (which are dense and ordered, so can be used to size
// arrays and so forth) and the actual signal number (which is
// system dependent.
typedef enum SpSigno SpSigno;
enum SpSigno {
	SpSIGINT,
	SpSIGQUIT,
	SpSIGHUP,
	SpSIGUSR1,
};

// The maximum signal number.  Note that this is not part of the
// `enum` above; some compilers will warn if we try to e.g.
// index based on into an array that is exactly sized to the
// number of elements in the enum, or if we have a switch that
// doesn't have an explicit `case` for the max size element.
// Making this a constant outside of the enumeration works this.
enum { SP_SIGMAX = SpSIGUSR1 + 1 };

// Maps from our internal signal identifier to the system's
// signal number.
int
sptosigno(SpSigno signo)
{
	const int sigmap[SP_SIGMAX] = {
	    [SpSIGINT] = SIGINT,
	    [SpSIGQUIT] = SIGQUIT,
	    [SpSIGHUP] = SIGHUP,
	    [SpSIGUSR1] = SIGUSR1,
	};
	return sigmap[signo];
}

// Mapes from out internal signal identifier to a signal name,
// like `SIGINT` etc.
const char *
sptosigname(SpSigno signo)
{
	switch (signo) {
	case SpSIGINT:	return "SIGINT";
	case SpSIGQUIT:	return "SIGQUIT";
	case SpSIGHUP:	return "SIGHUP";
	case SpSIGUSR1:	return "SIGUSR1";
	}
	fprintf(stderr, "Unhandleable signal: %d\n", signo);
	_exit(EXIT_FAILURE);
}

// Maps from signal number to our internal identifier.
SpSigno
signotosp(int signo)
{
	switch (signo) {
	case SIGINT:	return SpSIGINT;
	case SIGQUIT:	return SpSIGQUIT;
	case SIGHUP:	return SpSIGHUP;
	case SIGUSR1:	return SpSIGUSR1;
	}
	fprintf(stderr, "Unhandleable signal: %d\n", signo);
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
Aux sigaux[SP_SIGMAX];

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
handler(int signo)
{
	SpSigno sp = signotosp(signo);
	Aux *ap = &sigaux[sp];
	char b = (char)signo;
	(void)write(ap->pds[1], &b, sizeof(b));
}

// Sets up a signal for handling.  Returns a pointer to a
// static, initialized, owned condition variable that will be
// broadcast on when the given signal is received.
static pthread_cond_t *
initselfpipe(SpSigno signo)
{
	Aux *ap = &sigaux[signo];
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
	// signals are edge triggered, and b) if we can't write into
	// the pipe this implies that there are existing notification
	// messages in the pipe, so the receiver will process those.
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
	if (sigaction(sptosigno(signo), &sa, NULL) < 0) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}

	return &ap->cv;
}

/*
 * A basic test harness.
 */
typedef struct SigData SigData;
struct SigData {
	pthread_cond_t *cv;
	const char *signame;
	int signo;
	pthread_t tid;
};

void *
cvwaiter(void *arg)
{
	int times = 0;
	SigData *data = arg;
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&mtx);
	for (;;) {
		pthread_cond_wait(data->cv, &mtx);
		printf("Recevied signal %s (%d)\n", data->signame, data->signo);
		if (++times == 10) {
			printf("received %s %d times; bailing.\n", data->signame, times);
			_exit(EXIT_SUCCESS);
		}
	}
}

static SigData sigdata[SP_SIGMAX];

void
initsigs(void)
{
	for (SpSigno k = 0; k < SP_SIGMAX; ++k) {
		SigData *data = &sigdata[k];
		data->cv = initselfpipe(k);
		assert(data->cv != NULL);
		data->signame = sptosigname(k);
		data->signo = sptosigno(k);
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
