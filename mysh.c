

#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>


#define SHELL_NAME  "mysh"
#define MAX_INPUT   4096
#define MAX_ARGS    256
#define MAX_PIPES   32


static void print_prompt(void)
{
    char hostname[256] = "unknown";
    char cwd[1024]     = "?";
    char disp[1024]    = "?";

    gethostname(hostname, sizeof(hostname) - 1);
    struct passwd *pw   = getpwuid(getuid());
    const char    *user = pw ? pw->pw_name : "unknown";

    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");


    if (pw) {
        size_t hlen = strlen(pw->pw_dir);
        if (strncmp(cwd, pw->pw_dir, hlen) == 0)
            snprintf(disp, sizeof(disp), "~%s", cwd + hlen);
        else
            strncpy(disp, cwd, sizeof(disp) - 1);
    } else {
        strncpy(disp, cwd, sizeof(disp) - 1);
    }

    printf("\033[1;32m%s@%s\033[0m:\033[1;34m%s\033[0m$ ", hostname, user, disp);
    fflush(stdout);
}


static int tokenise(char *line, char **argv, int max_args)
{
    int   argc = 0;
    char *p    = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        char buf[MAX_INPUT];
        int  bi = 0;

        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) buf[bi++] = *p++;
            if (*p) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') buf[bi++] = *p++;
        }
        buf[bi]    = '\0';
        argv[argc] = strdup(buf);
        if (!argv[argc]) { perror("strdup"); exit(1); }
        argc++;
    }
    argv[argc] = NULL;
    return argc;
}


static char *extract_redirect(char **argv, int *argc)
{
    for (int i = 0; i < *argc; i++) {
        if (strcmp(argv[i], ">") == 0 && i + 1 < *argc) {
            char *file = argv[i + 1];
            for (int j = i; j < *argc - 2; j++) argv[j] = argv[j + 2];
            *argc -= 2;
            argv[*argc] = NULL;
            return file;
        }
    }
    return NULL;
}


static int builtin_echo(char **argv, int argc, int out_fd)
{
    int no_newline = 0;
    int interpret  = 0;
    int i          = 1;

    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') break;
        int ok = 1;
        for (int k = 1; argv[i][k]; k++) {
            if      (argv[i][k] == 'n') no_newline = 1;
            else if (argv[i][k] == 'e') interpret  = 1;
            else { ok = 0; break; }
        }
        if (!ok) break;
    }

    FILE *out = fdopen(dup(out_fd), "w");
    if (!out) { out = stdout; }

    for (int j = i; j < argc; j++) {
        if (j > i) fputc(' ', out);
        if (interpret) {
            for (const char *c = argv[j]; *c; c++) {
                if (*c == '\\' && *(c + 1)) {
                    c++;
                    switch (*c) {
                        case 'n':  fputc('\n', out); break;
                        case 't':  fputc('\t', out); break;
                        case '\\': fputc('\\', out); break;
                        case '"':  fputc('"',  out); break;
                        case 'r':  fputc('\r', out); break;
                        default:   fputc('\\', out); fputc(*c, out); break;
                    }
                } else {
                    fputc(*c, out);
                }
            }
        } else {
            fputs(argv[j], out);
        }
    }
    if (!no_newline) fputc('\n', out);
    fflush(out);
    fclose(out);
    return 0;
}


static void cat_stream(FILE *fp, FILE *out, int number, int show_ends)
{
    char line[MAX_INPUT];
    int  lineno = 1;

    while (fgets(line, sizeof(line), fp)) {
        if (number) fprintf(out, "%6d\t", lineno++);
        if (show_ends) {
            for (char *c = line; *c; c++) {
                if (*c == '\n') { fputs("$\n", out); }
                else if ((unsigned char)*c < 32 || (unsigned char)*c == 127)
                    fprintf(out, "^%c", ((*c + 64) & 127));
                else fputc(*c, out);
            }
        } else {
            fputs(line, out);
        }
    }
}

static int builtin_cat(char **argv, int argc, int out_fd)
{
    int number    = 0;
    int show_ends = 0;
    int i         = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--number") == 0)
            number = 1;
        else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "-e") == 0)
            show_ends = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0')
            fprintf(stderr, "%s: cat: unknown flag: %s\n", SHELL_NAME, argv[i]);
        else break;
    }

    FILE *out = fdopen(dup(out_fd), "w");
    if (!out) out = stdout;

    if (i >= argc) {
        cat_stream(stdin, out, number, show_ends);
    } else {
        for (; i < argc; i++) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "%s: cat: %s: %s\n", SHELL_NAME, argv[i], strerror(errno));
                continue;
            }
            cat_stream(fp, out, number, show_ends);
            fclose(fp);
        }
    }

    fflush(out);
    fclose(out);
    return 0;
}

/* ─── CUSTOM BUILT-IN: touch ─────────────────────────────────────────────── */

static int builtin_touch(char **argv, int argc)
{
    int no_create = 0;
    int i         = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--no-create") == 0)
            no_create = 1;
        else if (strcmp(argv[i], "-t") == 0)
            i++;  /* skip STAMP argument; use current time */
        else if (argv[i][0] == '-' && argv[i][1] != '\0')
            fprintf(stderr, "%s: touch: unknown flag: %s\n", SHELL_NAME, argv[i]);
        else break;
    }

    if (i >= argc) {
        fprintf(stderr, "usage: touch [-c] file ...\n");
        return 1;
    }

    int ret = 0;
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0) {
            /* File exists — update timestamps to now */
            if (utimes(argv[i], NULL) != 0) {
                fprintf(stderr, "%s: touch: %s: %s\n", SHELL_NAME, argv[i], strerror(errno));
                ret = 1;
            }
        } else {
            if (no_create) continue;
            int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
            if (fd < 0) {
                fprintf(stderr, "%s: touch: %s: %s\n", SHELL_NAME, argv[i], strerror(errno));
                ret = 1;
            } else {
                close(fd);
            }
        }
    }
    return ret;
}

/* ─── CUSTOM BUILT-IN: pwd ───────────────────────────────────────────────── */
static int builtin_pwd(char **argv, int argc, int out_fd)
{
    int physical = 0;
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-P") == 0) physical = 1;
        else if (strcmp(argv[i], "-L") == 0) physical = 0;
    }

    char buf[4096];
    if (physical) {
        if (!realpath(".", buf)) { perror("realpath"); return 1; }
    } else {
        if (!getcwd(buf, sizeof(buf))) { perror("getcwd"); return 1; }
    }

    FILE *out = fdopen(dup(out_fd), "w");
    if (!out) out = stdout;
    fprintf(out, "%s\n", buf);
    fflush(out);
    fclose(out);
    return 0;
}

/* ─── cd ─────────────────────────────────────────────────────────────────── */
static int builtin_cd(char **argv, int argc)
{
    const char *dir;
    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        struct passwd *pw = getpwuid(getuid());
        dir = pw ? pw->pw_dir : "/";
    } else {
        dir = argv[1];
    }
    if (chdir(dir) != 0) {
        fprintf(stderr, "%s: cd: %s: %s\n", SHELL_NAME, dir, strerror(errno));
        return 1;
    }
    return 0;
}

/* ─── help ───────────────────────────────────────────────────────────────── */
static int builtin_help(int out_fd)
{
    FILE *out = fdopen(dup(out_fd), "w");
    if (!out) out = stdout;
    fprintf(out,
        "\033[1;36mmysh — command reference\033[0m\n\n"
        "\033[1mCustom built-in implementations (C):\033[0m\n"
        "  echo [-n] [-e] [args ...]      print text\n"
        "          -n  no trailing newline\n"
        "          -e  interpret \\n \\t \\\\ escapes\n"
        "  cat  [-n] [-A] [file ...]      concatenate files\n"
        "          -n  number output lines\n"
        "          -A  show non-printing chars, $ at EOL\n"
        "  touch [-c] [-t STAMP] file ... create/update timestamps\n"
        "          -c  do not create if missing\n"
        "  pwd  [-L|-P]                   print working directory\n"
        "          -P  physical path (resolve symlinks)\n\n"
        "\033[1mShell built-ins:\033[0m\n"
        "  cd [dir]     change directory (~ for home)\n"
        "  exit/quit    leave the shell\n"
        "  help         this message\n\n"
        "\033[1mExternal programs (fork/exec) — flags are passed through:\033[0m\n"
        "  ls   [-l] [-a] [-la]           list directory\n"
        "  mv   src dst                   move or rename\n"
        "  mkdir [-p] dir                 make directory\n"
        "  rm   [-r] [-f] [-rf]           remove files\n"
        "  cp   [-r] src dst              copy files\n"
        "  wc   [-l] [-w] [-c] [file]     count lines/words/chars\n"
        "  grep [-i] [-r] pattern [file]  search text\n"
        "  date                           print date and time\n\n"
        "\033[1mRedirection & pipes:\033[0m\n"
        "  command > file                 redirect stdout to file\n"
        "  cmd1 | cmd2 | ...             pipe output between commands\n"
    );
    fflush(out);
    fclose(out);
    return 0;
}

/* ─── Execute one command stage (fds already set) ───────────────────────── */
static int execute_one(char **argv, int argc, int in_fd, int out_fd)
{
    if (argc == 0 || !argv[0] || argv[0][0] == '\0') return 0;

    /* Built-ins */
    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0)
        { printf("Goodbye!\n"); exit(0); }
    if (strcmp(argv[0], "cd")    == 0) return builtin_cd(argv, argc);
    if (strcmp(argv[0], "echo")  == 0) return builtin_echo(argv, argc, out_fd);
    if (strcmp(argv[0], "cat")   == 0) return builtin_cat(argv, argc, out_fd);
    if (strcmp(argv[0], "touch") == 0) return builtin_touch(argv, argc);
    if (strcmp(argv[0], "pwd")   == 0) return builtin_pwd(argv, argc, out_fd);
    if (strcmp(argv[0], "help")  == 0) return builtin_help(out_fd);

    /* External program */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        if (in_fd  != STDIN_FILENO)  { dup2(in_fd,  STDIN_FILENO);  close(in_fd);  }
        if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
        execvp(argv[0], argv);
        fprintf(stderr, "%s: command not found: %s\n", SHELL_NAME, argv[0]);
        exit(127);
    }

    if (in_fd  != STDIN_FILENO)  close(in_fd);
    if (out_fd != STDOUT_FILENO) close(out_fd);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

/* Check if a command name is a built-in (needs forking in a pipeline). */
static int is_builtin(const char *name)
{
    return (strcmp(name, "echo")  == 0 ||
            strcmp(name, "cat")   == 0 ||
            strcmp(name, "touch") == 0 ||
            strcmp(name, "pwd")   == 0 ||
            strcmp(name, "help")  == 0);
}

/* ─── Pipeline + redirect dispatcher ────────────────────────────────────── */
static int run_line(char **argv, int argc)
{
    if (argc == 0) return 0;

    /* Split on "|" — record start index and length of each stage */
    int stage_start[MAX_PIPES];
    int stage_len[MAX_PIPES];
    int num_stages    = 1;
    stage_start[0]    = 0;
    stage_len[0]      = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            argv[i] = NULL;   /* null-terminate current stage */
            stage_start[num_stages] = i + 1;
            stage_len[num_stages]   = 0;
            num_stages++;
            if (num_stages >= MAX_PIPES) break;
        } else {
            stage_len[num_stages - 1]++;
        }
    }

    /* ── Single command ── */
    if (num_stages == 1) {
        char **sargv = &argv[stage_start[0]];
        int    sargc = stage_len[0];
        int    out_fd = STDOUT_FILENO;

        char *redir = extract_redirect(sargv, &sargc);
        if (redir) {
            out_fd = open(redir, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                fprintf(stderr, "%s: %s: %s\n", SHELL_NAME, redir, strerror(errno));
                return 1;
            }
        }
        int ret = execute_one(sargv, sargc, STDIN_FILENO, out_fd);
        if (out_fd != STDOUT_FILENO) close(out_fd);
        return ret;
    }

    /* ── Pipeline ── */
    int   prev_read = STDIN_FILENO;
    pid_t pids[MAX_PIPES];
    int   nchildren = 0;

    for (int s = 0; s < num_stages; s++) {
        char **sargv  = &argv[stage_start[s]];
        int    sargc  = stage_len[s];
        int    out_fd = STDOUT_FILENO;
        int    pfd[2] = {-1, -1};

        if (s < num_stages - 1) {
            if (pipe(pfd) < 0) { perror("pipe"); return 1; }
            out_fd = pfd[1];
        } else {
            /* Last stage may redirect */
            char *redir = extract_redirect(sargv, &sargc);
            if (redir) {
                out_fd = open(redir, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (out_fd < 0) {
                    fprintf(stderr, "%s: %s: %s\n", SHELL_NAME, redir, strerror(errno));
                    if (prev_read != STDIN_FILENO) close(prev_read);
                    return 1;
                }
            }
        }

        /* Fork for every stage (built-ins must run in a child to use pipe fds) */
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }

        if (pid == 0) {
            /* child */
            if (prev_read != STDIN_FILENO)
                { dup2(prev_read, STDIN_FILENO); close(prev_read); }
            if (out_fd != STDOUT_FILENO)
                { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
            if (s < num_stages - 1) close(pfd[0]);

            if (sargc > 0 && is_builtin(sargv[0])) {
                exit(execute_one(sargv, sargc, STDIN_FILENO, STDOUT_FILENO));
            } else if (sargc > 0) {
                execvp(sargv[0], sargv);
                fprintf(stderr, "%s: command not found: %s\n", SHELL_NAME, sargv[0]);
                exit(127);
            }
            exit(0);
        }

        pids[nchildren++] = pid;

        /* Parent: close our copies of the pipe ends */
        if (prev_read != STDIN_FILENO) close(prev_read);
        if (out_fd    != STDOUT_FILENO) close(out_fd);
        if (s < num_stages - 1) prev_read = pfd[0];
    }

    int last_status = 0;
    for (int c = 0; c < nchildren; c++) {
        int st;
        waitpid(pids[c], &st, 0);
        if (c == nchildren - 1)
            last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    }
    return last_status;
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    char  input[MAX_INPUT];
    char *argv[MAX_ARGS];

    printf(
        "\033[1;36m"
        "  _ __ ___  _   _ ___| |__ \n"
        " | '_ ` _ \\| | | / __| '_ \\\n"
        " | | | | | | |_| \\__ \\ | | |\n"
        " |_| |_| |_|\\__, |___/_| |_|\n"
        "             |___/          \n"
        "\033[0m"
        "  \033[1mmysh\033[0m — type \033[1mhelp\033[0m for available commands\n\n"
    );

    while (1) {
        print_prompt();

        if (!fgets(input, sizeof(input), stdin)) {
            printf("\nGoodbye!\n");
            break;
        }

        /* Strip trailing newline / carriage return */
        size_t len = strlen(input);
        while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
            input[--len] = '\0';

        /* Skip blank lines and shell-style comments */
        const char *tr = input;
        while (*tr == ' ' || *tr == '\t') tr++;
        if (*tr == '\0' || *tr == '#') continue;

        int argc = tokenise(input, argv, MAX_ARGS);
        if (argc == 0) continue;

        run_line(argv, argc);

        for (int i = 0; i < argc; i++)
            free(argv[i]);
    }

    return 0;
}