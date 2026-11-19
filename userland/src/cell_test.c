/*
 * cell_test: drive the modem's calls and USSD from user space, through the kernel's sysctls
 * (drivers/modem/hakios.voice.c), and say what the modem's driver knows.
 *
 * usage: cell_test STEP...
 *   status            the network, the signal, the data session, the calls
 *   wait S            sleep S seconds
 *   registered S      wait up to S seconds for the modem to be registered
 *   data S            wait up to S seconds for the modem's data session, then have its interface (en1) configured:
 *                     configd makes no network service of its own for it, so its IPConfiguration plugin is asked,
 *                     as "net_test dhcp en1" asks (the kernel answers the DHCP with the session's settings). What the
 *                     launchd job for the modem runs in an image that has one.
 *   ussd CODE [S]     send a USSD code and print the network's answer (waits up to S seconds, 30 by default)
 *   answer TEXT [S]   answer what the network asked in a USSD session
 *   cancel            end the USSD session
 *   dial NUMBER       make a call
 *   pickup            answer the call that rings
 *   hangup            end every call
 *   calls S           print the calls' states for S seconds, as they change
 */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int
get_int(const char *name)
{
	int value = 0;
	size_t size = sizeof(value);

	return sysctlbyname(name, &value, &size, NULL, 0) == 0 ? value : -99;
}

static const char *
get_text(const char *name, char *to, size_t room)
{
	size_t size = room - 1;

	to[0] = 0;
	if (sysctlbyname(name, to, &size, NULL, 0) == 0) {
		to[size < room ? size : room - 1] = 0;
	}
	return to;
}

static int
put_text(const char *name, const char *text)
{
	return sysctlbyname(name, NULL, NULL, (void *)(uintptr_t)text, strlen(text) + 1);
}

static int
put_int(const char *name, int value)
{
	return sysctlbyname(name, NULL, NULL, &value, sizeof(value));
}

static void
status(void)
{
	char a[128], b[64], c[300];

	printf("cell_test: up %d, registered %d, \"%s\", %d dBm, %d bars; data %d, address %s; calls: %s\n",
	    get_int("kern.hakios.cellular_up"), get_int("kern.hakios.cellular_registered"),
	    get_text("kern.hakios.cellular_operator", a, sizeof(a)), get_int("kern.hakios.cellular_dbm"),
	    get_int("kern.hakios.cellular_bars"), get_int("kern.hakios.cellular_data"),
	    get_text("kern.hakios.cellular_ip", b, sizeof(b)), get_text("kern.hakios.cellular_calls", c, sizeof(c)));
}

extern char **environ;

/* The data session's interface, configured once the session is there. */
static void
data(int seconds)
{
	char *const arguments[] = { "/redwood/bin/net_test", "dhcp", "en1", "waitaddr", "en1", "15", NULL };
	int waited = 0, status = -1;
	pid_t child = 0;

	while (waited < seconds && get_int("kern.hakios.cellular_data") <= 0) {
		sleep(1);
		waited++;
	}
	if (get_int("kern.hakios.cellular_data") <= 0) {
		printf("cell_test: data: no data session after %d s\n", waited);
		return;
	}
	if (posix_spawn(&child, arguments[0], NULL, NULL, arguments, environ) != 0) {
		printf("cell_test: data: the session is up after %d s, but %s could not be run\n", waited, arguments[0]);
		return;
	}
	waitpid(child, &status, 0);
	printf("cell_test: data: the session was up after %d s; en1 asked to be configured (%d)\n", waited, status);
}

/* A USSD code or answer, and what comes back. */
static void
ussd(const char *text, int seconds)
{
	char reply[600];
	int state = 1;

	if (put_text("kern.hakios.cellular_ussd", text) != 0) {
		printf("cell_test: ussd %s: NOT SENT (", text);
		perror(")");
		return;
	}
	for (int tenth = 0; tenth < seconds * 10; tenth++) {
		state = get_int("kern.hakios.cellular_ussd_state");
		if (state != 1) {
			break;
		}
		usleep(100000);
	}
	printf("cell_test: ussd %s: state %d%s: \"%s\"\n", text, state, state == 1 ? " (NO ANSWER in time)" : state == 3 ?
	    " (the network waits for an answer)" : state == -1 ? " (FAILED)" : "",
	    get_text("kern.hakios.cellular_ussd_reply", reply, sizeof(reply)));
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
		int more = i + 2 < argc && argv[i + 2][0] >= '0' && argv[i + 2][0] <= '9' && strlen(argv[i + 2]) < 4 ?
		    atoi(argv[i + 2]) : 0;

		if (strcmp(step, "status") == 0) {
			status();
		} else if (strcmp(step, "wait") == 0 && next) {
			sleep((unsigned)atoi(next));
			i++;
		} else if (strcmp(step, "registered") == 0 && next) {
			int seconds = atoi(next), waited = 0;

			while (waited < seconds && get_int("kern.hakios.cellular_registered") <= 0) {
				sleep(1);
				waited++;
			}
			printf("cell_test: registered %d after %d s\n", get_int("kern.hakios.cellular_registered"), waited);
			i++;
		} else if (strcmp(step, "data") == 0 && next) {
			data(atoi(next));
			i++;
		} else if ((strcmp(step, "ussd") == 0 || strcmp(step, "answer") == 0) && next) {
			ussd(next, more ? more : 30);
			i += more ? 2 : 1;
		} else if (strcmp(step, "cancel") == 0) {
			printf("cell_test: cancel: %d\n", put_int("kern.hakios.cellular_ussd_cancel", 1));
		} else if (strcmp(step, "dial") == 0 && next) {
			printf("cell_test: dial %s: %s\n", next, put_text("kern.hakios.cellular_dial", next) == 0 ? "sent" : "NOT SENT");
			i++;
		} else if (strcmp(step, "pickup") == 0) {
			printf("cell_test: pickup: %d\n", put_int("kern.hakios.cellular_answer", 0));
		} else if (strcmp(step, "hangup") == 0) {
			printf("cell_test: hangup: %d\n", put_int("kern.hakios.cellular_hangup", 0));
		} else if (strcmp(step, "calls") == 0 && next) {
			char now[300], last[300] = "?";

			for (int tenth = 0; tenth < atoi(next) * 10; tenth++) {
				get_text("kern.hakios.cellular_calls", now, sizeof(now));
				if (strcmp(now, last) != 0) {
					printf("cell_test: calls: %s\n", now[0] ? now : "(none)");
					strcpy(last, now);
				}
				usleep(100000);
			}
			i++;
		} else {
			printf("cell_test: what is \"%s\"?\n", step);
		}
	}
	return 0;
}
