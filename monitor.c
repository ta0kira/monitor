#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>


#define TIME_FORMAT "%c"
#define UMASK 0077
#define ENV_NAME "MONITOR_MSG"
#define DIFF_DELAY { 0, 100 * 1000 * 1000 }


static const char *program_name = "";
static char *notify_command = NULL;
static char *diff_command   = NULL;


static int open_log_file(const char*);
static void close_log_file();
static void flush_log_file(void);

static void log_section_start(const char*, ...);
static void log_section_end(const char*, ...);
static void log_single_line(const char*, ...);
static void log_raw_data(const void*, ssize_t);

static int open_notify();
static void ready_notify();
static void close_notify();
static int open_diff();
static void ready_diff();
static void close_diff();

static void set_handlers(void);


int main(int argc, char *argv[])
{
	program_name = argv[0];
	umask(UMASK);

	if (argc < 4)
	{
	fprintf(stderr, "%s [monitor file] [storage file] [log file] (notify command) (diff command)\n", argv[0]);
	return 1;
	}

	notify_command = (argc > 4 && strlen(argv[4]))? argv[4] : NULL;
	diff_command   = (argc > 5 && strlen(argv[5]))? argv[5] : NULL;

	int monitor_file = open(argv[1], O_RDONLY);
	if (monitor_file < 0)
	{
	fprintf(stderr, "%s: unable to open monitor file '%s': %s\n", argv[0], argv[1], strerror(errno));
	return 1;
	}
	fcntl(monitor_file, F_SETFD, fcntl(monitor_file, F_GETFD) | O_CLOEXEC);

	int storage_file = open(argv[2], O_RDWR | O_CREAT | O_APPEND, 0666);
	if (storage_file < 0)
	{
	fprintf(stderr, "%s: unable to open storage file '%s': %s\n", argv[0], argv[2], strerror(errno));
	return 1;
	}
	ftruncate(storage_file, 0);
	fcntl(storage_file, F_SETFD, fcntl(monitor_file, F_GETFD) | O_CLOEXEC);

	if (!open_log_file(argv[3])) return 1;

	set_handlers();


	const int event_count = 2;
	struct kevent event_list[event_count];
	struct kevent current_event;

	static char buffer[1024];
	ssize_t read_size = 0, read_total = 0;

	int new_events = 0, run_diff;
	struct stat monitor_stat, storage_stat, new_stat;

	int notify_out = -1, diff_in = -1;


	int queue = kqueue();
	if (queue < 0)
	{
	fprintf(stderr, "%s: unable to create event queue: %s\n", argv[0], strerror(errno));
	return 1;
	}

	EV_SET(event_list + 0, monitor_file, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR | EV_RECEIPT,
	  NOTE_DELETE | NOTE_ATTRIB | NOTE_RENAME | NOTE_WRITE | NOTE_LINK | NOTE_REVOKE, 0, 0);

	EV_SET(event_list + 1, monitor_file, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR | EV_RECEIPT, 0, 0, 0);

	//(for some reason you need '&event' for the second to register)
	kevent(queue, event_list, event_count, &current_event, 1, NULL);

	//(don't 'lseek' here so that the first event will be 'EVFILT_READ')

	log_single_line("STARTED");


	while (1)
	{
	new_events = kevent(queue, NULL, 0, &current_event, 1, NULL);
	if (new_events < 0)
	 {
	fprintf(stderr, "%s: error polling for events: %s\n", argv[0], strerror(errno));
	return 1;
	 }

	else if (new_events > 0)
	 {
	run_diff = 0;

	fstat(monitor_file, &monitor_stat);

	if (current_event.filter & EVFILT_VNODE)
	  {
	if (current_event.fflags & NOTE_DELETE)
	   {
	log_single_line("DELETED => EXITING");
	break;
	   }

	if (current_event.fflags & NOTE_REVOKE)
	   {
	log_single_line("REVOKED => EXITING");
	break;
	   }

	if (current_event.fflags & NOTE_WRITE && monitor_stat.st_size == lseek(monitor_file, 0, SEEK_CUR))
	   {
	run_diff = 1;
	log_single_line("EDITED");
	   }

	if (current_event.fflags & NOTE_ATTRIB)
	   {
	char time_buffer[256] = { 0x00 };
	strftime(time_buffer, sizeof time_buffer, TIME_FORMAT, localtime(&monitor_stat.st_mtim.tv_sec));
	log_single_line("ATTRIBUTE CHANGE: %i:%i %04o %s",
	  monitor_stat.st_uid, monitor_stat.st_gid, monitor_stat.st_mode & 07777, time_buffer);
	   }

	if (current_event.fflags & NOTE_RENAME)
	   {
	log_single_line("RENAMED => EXITING");
	break;
	   }

	if (current_event.fflags & NOTE_LINK)
	   {
	log_single_line("LINK COUNT CHANGED: %i", monitor_stat.st_nlink);
	   }

	flush_log_file();
	  }

	if (current_event.filter & EVFILT_READ && (int) current_event.data != 0)
	  {
	run_diff = 1;
	int old_position = lseek(monitor_file, 0, SEEK_CUR);

	if (current_event.data > 0)
	   {
	log_section_start("ADDED");
	for (int I = 0; I < current_event.data; I += read_size)
	    {
	read_size = read(monitor_file, buffer, (sizeof buffer + I > current_event.data)? (current_event.data - I) : sizeof buffer);
	if (read_size > 0)
	     {
	write(storage_file, buffer, read_size);
	log_raw_data(buffer, read_size);
	     }
	else break;
	    }
	log_section_end("ADDED");
	flush_log_file();
	   }

	else if (current_event.data < 0)
	   {
	ssize_t position = lseek(storage_file, current_event.data, SEEK_CUR);

	setenv(ENV_NAME, "truncated", 1);
	notify_out = open_notify();
	unsetenv(ENV_NAME);
	if (notify_out >= 0)
	    {
	lseek(storage_file, position, SEEK_SET);
	ready_notify();
	    }

	log_section_start("REMOVED");
	for (int I = 0; I < -current_event.data; I += read_size)
	    {
	read_size = read(storage_file, buffer, (sizeof buffer + I > -current_event.data)? (-current_event.data - I) : sizeof buffer);
	if (read_size > 0)
	     {
	log_raw_data(buffer, read_size);
	if (notify_out >= 0) write(notify_out, buffer, read_size);
	     }
	else break;
	    }
	log_section_end("REMOVED");
	flush_log_file();

	close_notify();

	ftruncate(storage_file, lseek(storage_file, position, SEEK_SET));
	   }

	lseek(monitor_file, old_position + current_event.data, SEEK_SET);
	  }


	if (run_diff)
	  {
	//this is important for 'newsyslog', since it appends to the file then immediately renames over it
	struct timespec delay = DIFF_DELAY;
	nanosleep(&delay, NULL);

	fstat(monitor_file, &monitor_stat);
	fstat(storage_file, &storage_stat);
	run_diff |= stat(argv[1], &new_stat) == 0;
	  }

	//only 'diff' if the filename points to the same inode, and the storage file is the same size

	if (run_diff &&
	    monitor_stat.st_dev == new_stat.st_dev &&
	    monitor_stat.st_ino == new_stat.st_ino &&
	    lseek(storage_file, 0, SEEK_CUR) == storage_stat.st_size &&
	    lseek(monitor_file, 0, SEEK_CUR) == monitor_stat.st_size &&
            monitor_stat.st_size == storage_stat.st_size)
	  {
	diff_in = open_diff(), notify_out = -1;
	read_size = 0, read_total = 0;

	if (diff_in >= 0)
	   {
	ready_diff();

	while ((read_size = read(diff_in, buffer, sizeof buffer)) > 0)
	    {
	//try to avoid a race condition if there are rapid appends to the file;
	//the assumption is that rapid appends will result in diffs that fit
	//within the output buffer of 'diff', so 'diff' should exit before
	//anything is read here => diffs should be limited to "actual" edits to
	//the file, i.e. made by hand.
	fstat(monitor_file, &monitor_stat);
	if (monitor_stat.st_size != storage_stat.st_size) break;

	if (read_total == 0)
	     {
	log_section_start("DIFF");
	setenv(ENV_NAME, "edited", 1);
	notify_out = open_notify();
	unsetenv(ENV_NAME);
	ready_notify();
	     }

	read_total += read_size;

	if (notify_out >= 0) write(notify_out, buffer, read_size);
	log_raw_data(buffer, read_size);
	    }

	close_notify();
	close_diff();


	if (read_total > 0)
	    {
	log_section_end("DIFF");
	flush_log_file();

	lseek(monitor_file, 0, SEEK_SET);
	ftruncate(storage_file, 0);
	lseek(storage_file, 0, SEEK_SET);

	read_size = 0;
	for (int I = 0; I < monitor_stat.st_size; I += read_size)
	     {
	if ((read_size = read(monitor_file, buffer, sizeof buffer)) <= 0) break;
	write(storage_file, buffer, read_size);
	     }
	    }
	   }
	  }
	 }
	}

	close(queue);
	close(monitor_file);
	close(storage_file);
	close_log_file();

	return 0;
}


static FILE *log_file = NULL;

static int open_log_file(const char *filename)
{
	log_file = fopen(filename, "a+");
	if (!log_file)
	{
	fprintf(stderr, "%s: unable to open log file '%s': %s\n", program_name, filename, strerror(errno));
	return 0;
	}
	//use 'ftruncate' so it can still be opened in append mode
	ftruncate(fileno(log_file), 0);
	fcntl(fileno(log_file), F_SETFD, fcntl(fileno(log_file), F_GETFD) | O_CLOEXEC);
	return 1;
}


static void close_log_file()
{
	//(copy the pointer so this can be used in a signal handler)
	FILE *old_file = log_file;
	log_file = NULL;
	if (old_file) fclose(old_file);
}


#define LOG_FORMATTED(format) \
{ va_list ap; \
  va_start(ap, format); \
  vfprintf(log_file, format, ap); }


static const char *log_timestamp()
{
	static char time_buffer[256] = { 0x00 };
	time_t current_time = time(NULL);
	strftime(time_buffer, sizeof time_buffer, TIME_FORMAT, localtime(&current_time));
	return time_buffer;
}


static void log_section_start(const char *format, ...)
{
	if (!log_file) return;
	fprintf(log_file, "##### [%s] ", log_timestamp());
	LOG_FORMATTED(format)
	fprintf(log_file, " >>>>>\n");
}

static void log_section_end(const char *format, ...)
{
	if (!log_file) return;
	fprintf(log_file, "<<<<< [%s] ", log_timestamp());
	LOG_FORMATTED(format)
	fprintf(log_file, " #####\n");
}

static void log_single_line(const char *format, ...)
{
	if (!log_file) return;
	fprintf(log_file, "##### [%s] ", log_timestamp());
	LOG_FORMATTED(format)
	fprintf(log_file, " #####\n");
}

static void log_raw_data(const void *buffer, ssize_t size)
{
	if (!log_file) return;
	fwrite(buffer, size, 1, log_file);
}

static void flush_log_file(void)
{
	if (!log_file) return;
	fflush(log_file);
}


static int open_common(char *shell_command, int *pipes, int *pid, int use_fd)
{
	pipes[0] = pipes[1] = -1;
	*pid = -1;

	if (shell_command && strlen(shell_command) && pipe(pipes) == 0)
	{
	*pid = fork();

	if (*pid == 0)
	 {
	close(pipes[use_fd]);
	dup2(pipes[1-use_fd], 1-use_fd);
	if (pipes[1-use_fd] > 1) close(pipes[1-use_fd]);
	if (use_fd == 0) close(use_fd /*<-- the OS will replace this*/);
	raise(SIGSTOP);
	setsid();
	char *command[] = { "/bin/sh", "-c", shell_command, NULL };
	execvp(command[0], command);
	_exit(255);
	 }

	else if (*pid > 0)
	 {
	close(pipes[1-use_fd]);
	pipes[1-use_fd] = -1;
	fcntl(pipes[use_fd], F_SETFD, fcntl(pipes[use_fd], F_GETFD) | O_CLOEXEC);
	int status = 0;
	while (waitpid(*pid, &status, WUNTRACED) == -1 && errno == EINTR);
	if (!WIFSTOPPED(status))
	  {
	close(pipes[use_fd]);
	pipes[use_fd] = -1;
	kill(*pid, SIGKILL);
	if (!WIFEXITED(status)) wait(NULL);
	*pid = -1;
	  }
	 }

	else
	 {
	close(pipes[0]);
	close(pipes[1]);
	pipes[0] = pipes[1] = -1;
	*pid = -1;
	 }
	}

	return pipes[use_fd];
}


static void close_common(int *pipes, int *pid, int use_fd)
{
	if (*pid > 0)
	{
	if (pipes[use_fd] >= 0) close(pipes[use_fd]);
	assert(pipes[1-use_fd] < 0);
	//(don't use 'wait' since there could be multiple forks)
	waitpid(*pid, NULL, 0);
	pipes[use_fd] = -1;
	*pid = -1;
	}
}


static int notify_pipe[2] = { -1, -1 };
static pid_t notify_pid = -1;

static int open_notify()
{ return open_common(notify_command, notify_pipe, &notify_pid, STDOUT_FILENO); }

static void ready_notify()
{ if (notify_pid > 0) kill(notify_pid, SIGCONT); }

static void close_notify()
{ close_common(notify_pipe, &notify_pid, STDOUT_FILENO); }


static int diff_pipe[2] = { -1, -1 };
static pid_t diff_pid = -1;

static int open_diff()
{ return open_common(diff_command, diff_pipe, &diff_pid, STDIN_FILENO); }

static void ready_diff()
{ if (diff_pid > 0) kill(diff_pid, SIGCONT); }

static void close_diff()
{ close_common(diff_pipe, &diff_pid, STDIN_FILENO); }


static void signal_exit(int sig)
{
	signal(sig, SIG_DFL);
	log_single_line("SIGNAL => EXITING");
	close_log_file();
	exit(255);
}


static void set_handlers(void)
{
#ifdef SIGHUP
	signal(SIGHUP, &signal_exit);
#endif

#ifdef SIGINT
	signal(SIGINT, &signal_exit);
#endif

#ifdef SIGQUIT
	signal(SIGQUIT, &signal_exit);
#endif

#ifdef SIGILL
	signal(SIGILL, &signal_exit);
#endif

#ifdef SIGTRAP
	signal(SIGTRAP, &signal_exit);
#endif

#ifdef SIGABRT
	signal(SIGABRT, &signal_exit);
#endif

#ifdef SIGIOT
	signal(SIGIOT, &signal_exit);
#endif

#ifdef SIGEMT
	signal(SIGEMT, &signal_exit);
#endif

#ifdef SIGFPE
	signal(SIGFPE, &signal_exit);
#endif

#ifdef SIGBUS
	signal(SIGBUS, &signal_exit);
#endif

#ifdef SIGSEGV
	signal(SIGSEGV, &signal_exit);
#endif

#ifdef SIGSYS
	signal(SIGSYS, &signal_exit);
#endif

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

#ifdef SIGALRM
	signal(SIGALRM, &signal_exit);
#endif

#ifdef SIGTERM
	signal(SIGTERM, &signal_exit);
#endif

#ifdef SIGXCPU
	signal(SIGXCPU, &signal_exit);
#endif

#ifdef SIGXFSZ
	signal(SIGXFSZ, &signal_exit);
#endif

#ifdef SIGVTALRM
	signal(SIGVTALRM, &signal_exit);
#endif

#ifdef SIGPROF
	signal(SIGPROF, &signal_exit);
#endif

#ifdef SIGUSR1
	signal(SIGUSR1, &signal_exit);
#endif

#ifdef SIGUSR2
	signal(SIGUSR2, &signal_exit);
#endif

#ifdef SIGTHR
	signal(SIGTHR, &signal_exit);
#endif
}
