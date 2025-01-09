/*
 * Example of using sigsetjmp/siglongjmp with pthreads to
 * broadcast on a condition variable in response to receipt of
 * a signal.
 *
 * Dan Cross <net!gajendra!cross>
 */

#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Aux Aux;
struct Aux {
	const int signum;
	const char *name;
	pthread_cond_t cv;
	pthread_t tid;
};

static Aux sigx[] = {
    { SIGINT, "SIGINT", PTHREAD_COND_INITIALIZER },
    { SIGQUIT, "SIGQUIT", PTHREAD_COND_INITIALIZER },
    { SIGHUP, "SIGHUP", PTHREAD_COND_INITIALIZER },
    { SIGTSTP, "SIGTSTP", PTHREAD_COND_INITIALIZER },
    { SIGUSR1, "SIGUSR1", PTHREAD_COND_INITIALIZER },
};
static const size_t NSIGX = sizeof(sigx) / sizeof(sigx[0]);

static pthread_cond_t misccv = PTHREAD_COND_INITIALIZER;

static jmp_buf sigloop;

static void
notify1(int signum)
{
	if (signum != 0) {
		for (size_t k = 0; k < NSIGX; ++k) {
			Aux *ap = &sigx[k];
			if (ap->signum == signum) {
				pthread_cond_broadcast(&ap->cv);
				return;
			}
		}
		pthread_cond_broadcast(&misccv);
	}
}

static void *
notifier(void *arg)
{
	sigset_t set;
	(void)arg;
	int sig;

	sigfillset(&set);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	sig = sigsetjmp(sigloop, 1);
	notify1(sig);
	for (;;) {
		sigset_t aset;
		sigemptyset(&aset);
		sigsuspend(&aset);
	}

	return NULL;
}

static void
handler(int signum)
{
	siglongjmp(sigloop, signum);
}

// Sets up a signal for handling.  Returns a pointer to a
// static, initialized condition variable that will be
// broadcast on when the given signal is received.
void
initsig(const int signum)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	if (sigaction(signum, &sa, NULL) < 0) {
		perror("sigaction");
		exit(EXIT_FAILURE);
	}
}

void *
cvwaiter(void *arg)
{
	int times = 0;
	pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
	Aux *aux = arg;

	assert(aux != NULL);

	pthread_mutex_lock(&mtx);
	for (;;) {
		pthread_cond_wait(&aux->cv, &mtx);
		printf("Recevied signal %s (%d)\n", aux->name, aux->signum);
		if (++times == 10) {
			printf("received %s %d times; bailing.\n", aux->name, times);
			_exit(EXIT_SUCCESS);
		}
	}
}

void
initsigs(void)
{
	for (size_t k = 0; k < NSIGX; ++k) {
		Aux *aux = &sigx[k];
		if (pthread_create(&aux->tid, NULL, cvwaiter, aux) < 0) {
			perror("pthread_create");
			exit(EXIT_FAILURE);
		}
		initsig(aux->signum);
	}
}

int
main(void)
{
	sigset_t all, set;

	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &set);
	initsigs();
	notifier(NULL);

	return 0;
}
