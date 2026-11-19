#include "yt-proc.h"

#include <obs-module.h>
#include <util/threading.h>
#include <util/darray.h>

#ifdef _WIN32
int yt_proc_run(const char *const *argv, struct dstr *output)
{
	UNUSED_PARAMETER(argv);
	dstr_copy(output, "process execution is not implemented on Windows");
	return -1;
}
void yt_proc_cancel_all(void) {}
void yt_proc_init(void) {}
void yt_proc_free(void) {}
#else

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

extern char **environ;

static pthread_mutex_t proc_mutex;
static DARRAY(pid_t) running;
static bool cancelled;

void yt_proc_init(void)
{
	pthread_mutex_init(&proc_mutex, NULL);
	da_init(running);
	cancelled = false;
}

void yt_proc_free(void)
{
	da_free(running);
	pthread_mutex_destroy(&proc_mutex);
}

void yt_proc_cancel_all(void)
{
	pthread_mutex_lock(&proc_mutex);
	cancelled = true;
	for (size_t i = 0; i < running.num; i++)
		kill(running.array[i], SIGTERM);
	pthread_mutex_unlock(&proc_mutex);
}

int yt_proc_run(const char *const *argv, struct dstr *output)
{
	int fds[2];
	pid_t pid;
	int status = 0;

	dstr_free(output);

	if (pipe(fds) != 0)
		return -1;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addclose(&actions, fds[0]);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, fds[1]);
	posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

	pthread_mutex_lock(&proc_mutex);
	if (cancelled) {
		pthread_mutex_unlock(&proc_mutex);
		posix_spawn_file_actions_destroy(&actions);
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	int ret = posix_spawn(&pid, argv[0], &actions, NULL, (char *const *)argv, environ);
	if (ret == 0)
		da_push_back(running, &pid);
	pthread_mutex_unlock(&proc_mutex);

	posix_spawn_file_actions_destroy(&actions);
	close(fds[1]);

	if (ret != 0) {
		close(fds[0]);
		dstr_printf(output, "failed to start '%s': %s", argv[0], strerror(ret));
		return -1;
	}

	char buf[4096];
	for (;;) {
		ssize_t n = read(fds[0], buf, sizeof(buf));
		if (n > 0)
			dstr_ncat(output, buf, (size_t)n);
		else if (n == 0 || errno != EINTR)
			break;
	}
	close(fds[0]);

	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;

	pthread_mutex_lock(&proc_mutex);
	da_erase_item(running, &pid);
	pthread_mutex_unlock(&proc_mutex);

	if (!output->array)
		dstr_copy(output, "");

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif
