/*
 * Tunnel shell through UNIX socket.
 *
 * Copyright (C) 2014 Andrey Gelman <andrey.gelman@gmail.com>
 *
 * For example, server run as root will export
 * root shell to any client.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libgen.h>

#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <pty.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef ANDROID
#define SERVER_PATH "/data/misc/shell-tunnel-socket"
#define EXEC_ARGV	{"/system/bin/sh", "-i", NULL}
#else
#define SERVER_PATH "/tmp/shell-tunnel-socket"
#define EXEC_ARGV	{"/bin/bash", "-i", NULL}
#endif

enum {MODE_UNDEF, MODE_DAEMON, MODE_CLIENT};
enum {CONS_CONFIG, CONS_RESTORE};

static void server_mode(void);
static void spawn_shell(int sockfd);
static void client_mode(bool local_echo);
static int console_proxy(int sockfd, bool local_echo);
static int byte_interchange(int in_a, int out_a, int in_b, int out_b);
static void print_usage(const char *name);

int main(int argc, char *argv[])
{
	int i;
	int mode = MODE_UNDEF;
	bool local_echo = false;

	/* parse command line */
	for (i = 1; i < argc; ++i) {
		if (!strcmp("--daemon", argv[i]))
			mode = MODE_DAEMON;
		else if (!strcmp("--client", argv[i]))
			mode = MODE_CLIENT;
		else if (!strcmp("--echo", argv[i]))
			local_echo = true;
	}

	switch (mode)
	{
	case MODE_DAEMON:
		/* detach from calling process */
		if (fork() != 0)
			return 0;

		unlink(SERVER_PATH);
		signal(SIGCHLD, SIG_IGN);
		server_mode();
		break;
	case MODE_CLIENT:
		client_mode(local_echo);
		break;
	default:
		print_usage(basename(argv[0]));
	}

	return 0;
}

static void print_usage(const char *name)
{
	printf("Usage: \n");
	printf("%s --daemon \n", name);
	printf("%s --client [--echo] \n", name);
	exit(1);
}

/*
 * Server side
 */
void server_mode(void)
{
	int err;
	int sockfd;
	int new_sockfd;
	struct sockaddr_un serveraddr;
	pid_t pid;

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("could not open socket");
		return;
	}

	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sun_family = AF_UNIX;
	strcpy(serveraddr.sun_path, SERVER_PATH);

	err = bind(sockfd, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr));
	if (err < 0) {
		perror("could not bind to socket");
		goto out_err_1;
	}

	err = listen(sockfd, 1);
	if (err < 0) {
		perror("could not listen to socket");
		goto out_err_1;
	}

	/* change socket mode to enable any user to connect */
	err = chmod(SERVER_PATH, 0666);
	if (err < 0) {
		perror("could not change socket mode");
		goto out_err_1;
	}

	while (true) {
		new_sockfd = accept(sockfd, NULL, NULL);
		if (new_sockfd < 0) {
			perror("could not accept connection");
			goto out_err_1;
		}

		pid = fork();
		if (pid < 0) {
			perror("could not fork process");
			goto out_err_2;
		}
		else if (pid > 0) {
			/* parent process */
			close(new_sockfd);
			continue;
		}
		else {
			/* child process */
			close(sockfd);

			/* business logic */
			spawn_shell(new_sockfd);

			close(new_sockfd);
			return;
		}
	}

out_err_2:
	close(new_sockfd);

out_err_1:
	close(sockfd);
	unlink(SERVER_PATH);
}

static void shell(int fd)
{
	int err;
	char *const execargv[] = EXEC_ARGV;

	err = setsid();
	if (err < 0) {
		perror("could not create a new session");
		return;
	}

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);

	err = ioctl(fd, TIOCSCTTY, NULL);
	if (err < 0) {
		perror("could not issue IOCTL");
		return;
	}

	close(fd);

	execvp(execargv[0], execargv);
	perror("could not exec shell");
}

static void spawn_shell(int sockfd)
{
	int err;
	int mst, slv;

	err = openpty(&mst, &slv, NULL, NULL, NULL);
	if (err < 0) {
		perror("could not open pseudo terminal");
		return;
	}

	switch (fork())
	{
	case -1:
		perror("could not fork process");
		return;

	case 0:
		/* child process */
		close(mst);
		shell(slv);
		break;

	default:
		/* parent process */
		close(slv);
		byte_interchange(sockfd, sockfd, mst, mst);
		close(mst);
	}
}

/*
 * Client side
 */
void client_mode(bool local_echo)
{
	int err;
	int sockfd;
	struct sockaddr_un serveraddr;

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("could not open socket");
		goto out_err_1;
	}

	memset(&serveraddr, 0, sizeof(serveraddr));
	serveraddr.sun_family = AF_UNIX;
	strcpy(serveraddr.sun_path, SERVER_PATH);

	err = connect(sockfd, (struct sockaddr *)&serveraddr, SUN_LEN(&serveraddr));
	if (err < 0) {
		perror("could not connect to socket");
		goto out_err_2;
	}

	console_proxy(sockfd, local_echo);
	shutdown(sockfd, SHUT_RDWR);

out_err_2:
	close(sockfd);

out_err_1:
	putchar('\n');
}

static void setup_console(int phase, bool local_echo)
{
	static struct termios term_state;
	struct termios new_state;

	switch (phase)
	{
	case CONS_CONFIG:
		/* switch to raw */
		tcgetattr(STDIN_FILENO, &term_state);
		new_state = term_state;

		new_state.c_lflag &= ~(ICANON | (!local_echo ? ECHO : 0));

		tcsetattr(STDIN_FILENO, TCSADRAIN, &new_state);
		break;
	case CONS_RESTORE:
		/* restore */
		tcsetattr(STDIN_FILENO, TCSADRAIN, &term_state);
		break;
	}
}

static int console_proxy(int sockfd, bool local_echo)
{
	int err;

	setup_console(CONS_CONFIG, local_echo);
	err = byte_interchange(STDIN_FILENO, STDOUT_FILENO, sockfd, sockfd);
	setup_console(CONS_RESTORE, local_echo);
	return err;
}

/*
 * in_a  --\ /-- in_b
 *          X
 * out_a <-/ \-> out_b
 */
static int byte_interchange(int in_a, int out_a, int in_b, int out_b)
{
	struct timeval tv;
	fd_set readfds;
	int len;
	char buff;
	int nfds = ((in_a >= in_b) ? in_a : in_b) + 1;

	while (true) {
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		FD_ZERO(&readfds);
		FD_SET(in_a, &readfds);
		FD_SET(in_b, &readfds);

		select(nfds, &readfds, NULL, NULL, &tv);

		/* in_a --> out_b */
		if (FD_ISSET(in_a, &readfds)) {
			len = read(in_a, &buff, 1);
			if (len < 0) {
				perror("could not read file (a)");
				break;
			}
			else if (len == 0) {
				/* end of file */
				break;
			}

			len = write(out_b, &buff, 1);
			if (len < 1) {
				perror("could not write file (b)");
				break;
			}
		}

		/* in_b --> out_a */
		if (FD_ISSET(in_b, &readfds)) {
			len = read(in_b, &buff, 1);
			if (len < 0) {
				perror("could not read file (b)");
				break;
			}
			else if (len == 0) {
				/* end of file */
				break;
			}

			len = write(out_a, &buff, 1);
			if (len < 1) {
				perror("could not write file (a)");
				break;
			}
		}
	}

	return len;
}

