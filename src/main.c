#include <sys/stat.h>
#include <sys/wait.h>

#include <error.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_SIZE  4096
#define LINE_SIZE  1024
#define ARGS_SIZE  (LINE_SIZE / 2)

#define STR_PIPE   (void *)&dummy[0]
#define STR_REDIN  (void *)&dummy[1]
#define STR_REDOUT (void *)&dummy[2]
#define STR_REDAPP (void *)&dummy[3]

void prompt();
int readline();
int readargs();
void runall();
void runcmd(int, int, int);
void inthandler(int);
void chldhandler(int);
int iscmd(const char *);
int isexec(const char *);
struct builtin *getbuiltin(const char *);
void changedir();
void help();
void myexit();
void jobadd(int);
void jobrm(int);
void jobupdate();
void syntax(const char *);
void errfile(const char *);

struct builtin {
	const char *name;
	void (*func)();
};

struct job {
	int pid;
	int done;
	struct job *next;
};

struct job *jobs;
struct builtin builtins[] = { { "cd", changedir }, { "help", help },
	{ "exit", myexit }, { NULL, NULL } };

char dummy[4];
char linebuf[LINE_SIZE];
char line[LINE_SIZE];
char *argsbuf[ARGS_SIZE];
char *args[ARGS_SIZE];
int rdout;
int rdin;
int resume;
int background;

int
main()
{
	signal(SIGINT, inthandler);
	signal(SIGCHLD, chldhandler);

	while (readline() == 0) {
		if (readargs() < 0)
			continue;

		char *name = args[0];
		struct builtin *b = getbuiltin(name);

		if (b)
			b->func();
		else if (iscmd(name))
			runall();
		else
			printf("eh?\n");

		jobupdate();
	}
}

int
readline()
{
	int size = 0, quote = 0;
	int c;

	if (!resume)
		prompt();

	while ((c = getchar()) != EOF && (quote || !strchr(";&\n", c))) {
		if (c == '\\') {
			linebuf[size++] = '\'';
			linebuf[size++] = c = getchar();
			linebuf[size++] = '\'';
		} else {
			if (c == '\'')
				quote ^= 1;
			linebuf[size++] = c;
		}

		if (c == '\n')
			printf("> ");
	}
	linebuf[size] = '\0';
	resume = c != '\n';
	background = c == '&';

	if (size == 0 && c == EOF)
		return -1;

	char *linep = line;
	char *bufp = linebuf;
	char **argp = argsbuf;
	char *stop;

	quote = 0;
	while (bufp < linebuf + size) {
		stop = strpbrk(bufp, ">|< ");
		if (!stop)
			stop = linebuf + size;

		if (bufp != stop) {
			for (*argp++ = linep; quote || bufp < stop; bufp++) {
				if (*bufp == '\'') {
					quote ^= 1;
					if (!quote) {
						stop = strpbrk(bufp, ">|< ");
						if (!stop)
							stop = linebuf + size;
					}
				} else {
					if (quote && strchr("\\~?*[]{}", *bufp))
						*linep++ = '\\';
					*linep++ = *bufp;
				}
			}
			*linep = '\0';
			linep = *--argp;
			glob_t gl;

			if (!glob(linep, GLOB_TILDE | GLOB_NOCHECK | GLOB_BRACE,
				NULL, &gl)) {
				for (size_t i = 0; i < gl.gl_pathc; i++) {
					char *p = gl.gl_pathv[i];

					*argp++ = linep;
					do {
						if (*p == '\\')
							p++;
					} while ((*linep++ = *p++));
				}
				globfree(&gl);
			}
		} else {
			switch (*stop) {
			case '|':
				*argp++ = STR_PIPE;
				break;
			case '<':
				*argp++ = STR_REDIN;
				break;
			case '>':
				if (*(bufp + 1) == '>') {
					bufp++;
					*argp++ = STR_REDAPP;
				} else {
					*argp++ = STR_REDOUT;
				}
				break;
			}
			bufp++;
		}
	}

	*argp = NULL;
	return 0;
}

int
readargs()
{
	char *s, *filename;
	int flags, j = 0;

	rdin = rdout = -1;
	for (int i = 0; argsbuf[i]; i++) {
		s = argsbuf[i];

		if (s == STR_REDIN || s == STR_REDOUT || s == STR_REDAPP) {
			if ((filename = argsbuf[++i]) == NULL) {
				if (rdin > 0)
					close(rdin);
				if (rdout > 0)
					close(rdout);

				syntax("newline");
				return -1;
			}

			if (s == STR_REDOUT || s == STR_REDAPP) {
				flags = O_CREAT | O_WRONLY | O_TRUNC;
				if (s == STR_REDAPP)
					flags ^= O_TRUNC | O_APPEND;

				if (rdout > 0)
					close(rdout);
				rdout = open(filename, flags, 0644);
				if (rdout < 0) {
					if (rdin > 0)
						close(rdin);
					errfile(filename);
					return -1;
				};
			} else {
				if (rdin > 0)
					close(rdin);
				rdin = open(filename, O_RDONLY);
				if (rdin < 0) {
					if (rdout > 0)
						close(rdout);
					errfile(filename);
					return -1;
				}
			}
		} else {
			args[j++] = argsbuf[i];
		}
	}

	args[j] = NULL;
	if (rdin < 0)
		rdin = 0;
	if (rdout < 0)
		rdout = 1;

	if (j > 0 && args[j - 1] == STR_PIPE) {
		syntax("|");
		return -1;
	}

	return j != 0 ? 0 : -1;
}

void
runall()
{
	int fd[2] = { rdin, rdout };
	int bg = background;
	int i, j;

	background = 0;
	for (i = 0; args[i];) {
		for (j = i + 1; args[j] && args[j] != STR_PIPE; j++)
			;

		int last = args[j] == NULL;
		int tmpfd[2];

		pipe(tmpfd);
		if (last) {
			if (bg)
				background = 1;
			tmpfd[1] = fd[1];
		}

		args[j] = NULL;
		runcmd(i, fd[0], tmpfd[1]);

		if (fd[0] != 0)
			close(fd[0]);
		fd[0] = tmpfd[0];

		if (tmpfd[1] != 1)
			close(tmpfd[1]);

		i = j + !last;
	}
}

void
runcmd(int offset, int in, int out)
{
	char **argv = args + offset;
	int pid = vfork();

	if (pid == 0) {
		if (in != 0) {
			dup2(in, 0);
			close(in);
		}
		if (out != 1) {
			dup2(out, 1);
			close(out);
		}

		if (execvp(argv[0], argv) < 0) {
			exit(-1);
		}
	} else if (pid > 0) {
		if (background)
			jobadd(pid);
		else
			waitpid(pid, NULL, 0);
	}
}

void
prompt()
{
	static char cwd[PATH_SIZE];

	char *home = getenv("HOME");
	char *cwdp = cwd;

	getcwd(cwd, sizeof(cwd));
	if (strstr(cwd, home) == cwd) {
		cwdp += strlen(home) - 1;
		*cwdp = '~';
	}

	printf("\033[01;32m%s λ \033[00m", cwdp);
	fflush(stdout);
}

int
iscmd(const char *name)
{
	static char path[PATH_SIZE], buf[PATH_SIZE];

	if (strchr(name, '/') != NULL)
		return isexec(name);

	strncpy(path, getenv("PATH"), sizeof(path));
	for (char *tok = strtok(path, ":"); tok; tok = strtok(NULL, ":")) {
		sprintf(buf, "%s/%s", tok, name);
		if (isexec(buf))
			return 1;
	}
	return 0;
}

int
isexec(const char *path)
{
	struct stat st;
	if (stat(path, &st) == -1)
		return 0;
	return S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR);
}

struct builtin *
getbuiltin(const char *name)
{
	struct builtin *b;
	for (b = builtins; b->name && strcmp(b->name, name) != 0; b++)
		;
	return b->name ? b : NULL;
}

void
changedir()
{
	if (!args[1])
		args[1] = getenv("HOME");

	if (chdir(args[1]) < 0)
		perror(NULL);
}

void
help()
{
	printf("Mateo Gjika, Sushi version 0.1\n");
}

void
myexit()
{
	printf("exit\n");
	exit(0);
}

void
syntax(const char *tok)
{
	printf("syntax error near \"%s\"\n", tok);
}

void
errfile(const char *filename)
{
	printf("couldn't open \"%s\"\n", filename);
}

void
inthandler(int signum)
{
	(void)signum;
	putchar('\n');
	prompt();
}

void
chldhandler(int signum)
{
	(void)signum;
	int pid;
	while ((pid = waitpid(-1, NULL, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
		jobrm(pid);
}

void
jobadd(int pid)
{
	printf("started [%d]\n", pid);

	struct job *j = malloc(sizeof(struct job));
	j->pid = pid;
	j->done = 0;
	j->next = jobs;
	jobs = j;
}

void
jobrm(int pid)
{
	struct job *j;
	for (j = jobs; j && j->pid != pid; j = j->next)
		;
	if (j)
		j->done = 1;
}

void
jobupdate()
{
	struct job *prev, *next, *cur;

	for (prev = cur = jobs; cur; cur = next) {
		next = cur->next;
		if (cur->done) {
			if (cur == jobs)
				jobs = next;
			else
				prev->next = next;

			printf("[%d] completed\n", cur->pid);
			free(cur);
		} else {
			prev = cur;
		}
	}
}
