#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>

FILE *fdopen(int fildes, const char *mode); // WTF ?!!

#include "smtp_server.h"

/* Forks, closes all file descriptors and redirects stdin/stdout to /dev/null */
void daemonize(void) {
	struct rlimit rl = {0};
	int fd = -1;
	int i;

	switch (fork()) {
	case -1:
		//syslog(LOG_ERR, "Prefork stage 1: %m");
		exit(1);
	case 0: /* child */
		break;
	default: /* parent */
		exit(0);
	}

	rl.rlim_max = 0;
	getrlimit(RLIMIT_NOFILE, &rl);
	switch (rl.rlim_max) {
	case -1: /* oops! */
		//syslog(LOG_ERR, "getrlimit");
		exit(1);
	case 0:
		//syslog(LOG_ERR, "Max number of open file descriptors is 0!");
		exit(1);
	}
	for (i = 0; i < rl.rlim_max; i++)
		close(i);
	if (setsid() == -1) {
		//syslog(LOG_ERR, "setsid failed");
		exit(1);
	}
	switch (fork()) {
	case -1:
		//syslog(LOG_ERR, "Prefork stage 2: %m");
		exit(1);
	case 0: /* child */
		break;
	default: /* parent */
		exit(0);
	}

	chdir("/");
	umask(0);
	fd = open("/dev/null", O_RDWR);
	dup(fd);
	dup(fd);
}

void server_main(void)
{
	int sock, on = 1, status;
	struct sockaddr_in servaddr;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	assert(sock != -1);

	status = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	assert(status != -1);

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(8025);

	status = bind(sock, (struct sockaddr *)&servaddr, sizeof(servaddr));
	assert(status != -1);

	status = listen(sock, 20);
	assert(status != -1);

	do {
		socklen_t addrlen = sizeof(struct sockaddr_in);
		struct smtp_server_context ctx;
		FILE *f;

		status = accept(sock, (struct sockaddr *)&ctx.addr, &addrlen);
		if (status < 0) {
			continue; // FIXME busy loop daca avem o problema recurenta
		}

		f = fdopen(status, "r+");
		assert(f != NULL);

		switch (fork()) {
		case -1:
			assert(0); // FIXME
			break;
		case 0:
			smtp_server_run(&ctx, f);
			fclose(f);
			exit(EXIT_SUCCESS);
		//default:
			// FIXME append child to list for graceful shutdown
		}
	} while (1);
}

int main(int argc, char **argv) {
	printf("Hello\n");
	smtp_server_init();
	server_main();
	return 0;
}
