/*
 * Calls and USSD: a small client of the modem's QMI voice service.
 *
 * Darwin's own telephony (CommCenter and what is built on it) needs a baseband of Apple's, which is not here. What is
 * here is the modem's voice service (QMI service 9), spoken to over the IPC router as hakios.nas.c speaks to the
 * network access service, and a handful of sysctls for user space to drive it:
 *
 *   kern.hakios.cellular_dial        write a number: a voice call is made to it
 *   kern.hakios.cellular_answer      write a call's id (0: the one that is ringing): it is answered
 *   kern.hakios.cellular_hangup      write a call's id (0: every call): it is ended
 *   kern.hakios.cellular_calls       the calls there are: "id state direction number", one after the other
 *   kern.hakios.cellular_call_state  the first call's state (0: no call; the numbers are QMI's, below)
 *   kern.hakios.cellular_ussd        write a code (*123#): it is sent; while the network waits for an answer to what
 *                                    it said, what is written is that answer
 *   kern.hakios.cellular_ussd_state  0 nothing asked, 1 waiting for the network, 2 an answer is there, 3 an answer is
 *                                    there and the network waits for ours, -1 it failed
 *   kern.hakios.cellular_ussd_reply  what the network said
 *   kern.hakios.cellular_ussd_cancel write 1: the USSD session is ended
 *
 * The messages and their fields are those of libqmi's description of the service (qmi-service-voice.json): Indication
 * Register 0x03, Dial Call 0x20, End Call 0x21, Answer Call 0x22, All Call Status 0x2e (indication), Originate USSD
 * 0x3a, Answer USSD 0x3b, Cancel USSD 0x3c, USSD 0x3e (indication), USSD Release 0x3d (indication). A message is a
 * header of seven bytes (0 request, 2 answer, 4 indication; the transaction; the message; the length) and then
 * type-length-value fields.
 *
 * What a call needs besides this, and does not have here: its sound. The modem's voice path to the speakers and from
 * the microphones is the audio DSP's business, and nothing of it is set up: a call made here is silent on this end.
 *
 * Run on the phone (POCO X6 5G): a USSD code was answered by the network, and a call was made, connected and ended;
 * a call coming in has not been tried.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.qrtr.h>
#include <sys/hakios.voice.h>

#define QMI_VOICE                       9
#define QMI_REQUEST                     0
#define QMI_RESPONSE                    2
#define QMI_INDICATION                  4

#define VOICE_INDICATION_REGISTER       0x0003
#define VOICE_DIAL_CALL                 0x0020
#define VOICE_END_CALL                  0x0021
#define VOICE_ANSWER_CALL               0x0022
#define VOICE_ALL_CALL_STATUS           0x002e
#define VOICE_ORIGINATE_USSD            0x003a
#define VOICE_ANSWER_USSD               0x003b
#define VOICE_CANCEL_USSD               0x003c
#define VOICE_USSD_RELEASE              0x003d
#define VOICE_USSD                      0x003e

#define USS_ASCII                       1       /* the data coding scheme of what is sent: plain characters */
#define USS_8BIT                        2
#define USS_UCS2                        3

#define CALLS                           4

static LCK_GRP_DECLARE(voice_lock_group, "hakios-voice");
static LCK_MTX_DECLARE(voice_lock, &voice_lock_group);

static struct {
	const struct hakios_qrtr_ops *transport;
	struct hakios_qrtr_port *port;
	uint32_t node, to;
	uint16_t transaction;
	bool connected;
	struct {
		uint8_t id, state, type, direction;     /* direction: 1 made here, 2 come in */
		char number[32];
	} call[CALLS];
	unsigned calls;
	int ussd_state;
	char ussd_reply[512];
} voice;

static const char *
call_state_name(unsigned state)
{
	static const char *const names[] = { "none", "origination", "incoming", "conversation", "call control in progress",
		                             "alerting", "hold", "waiting", "disconnecting", "end", "setup" };

	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

/* One request: the header, then the fields as they are given (already type-length-value). voice_lock held. */
static int
voice_ask(uint16_t message, const uint8_t *fields, size_t length)
{
	uint8_t frame[7 + 200];

	if (!voice.connected || voice.port == NULL) {
		return ENXIO;
	}
	if (length > sizeof(frame) - 7) {
		return E2BIG;
	}
	if (++voice.transaction == 0) {
		voice.transaction = 1;
	}
	frame[0] = QMI_REQUEST;
	frame[1] = (uint8_t)voice.transaction;
	frame[2] = (uint8_t)(voice.transaction >> 8);
	frame[3] = (uint8_t)message;
	frame[4] = (uint8_t)(message >> 8);
	frame[5] = (uint8_t)length;
	frame[6] = (uint8_t)(length >> 8);
	if (length != 0) {
		memcpy(frame + 7, fields, length);
	}
	return voice.transport->send(voice.port, voice.node, voice.to, frame, 7 + length);
}

/* A field of text: the type, the length, and then what `lead` bytes come before the characters (their count last). */
static size_t
text_field(uint8_t *to, size_t room, uint8_t type, const uint8_t *lead, size_t lead_length, const char *text)
{
	size_t n = strlen(text), size = lead_length + n;

	if (3 + size > room || n > 180) {
		return 0;
	}
	to[0] = type;
	to[1] = (uint8_t)size;
	to[2] = (uint8_t)(size >> 8);
	if (lead_length != 0) {
		memcpy(to + 3, lead, lead_length);
	}
	memcpy(to + 3 + lead_length, text, n);
	return 3 + size;
}

/*
 * What the network said, into the reply, as UTF-8. It comes with its coding scheme: plain characters (1, and "8 bit",
 * 2, taken as such), or UCS-2 (3), sixteen bits a character with the higher byte first, which is how a network that
 * does not write in Latin letters answers. A modem may add the same as UTF-16, the lower byte first, in a field of
 * its own (`wide`).
 */
static void
reply_put(size_t *at, uint32_t c)
{
	if (*at + 4 >= sizeof(voice.ussd_reply)) {
		return;
	}
	if (c < 0x80) {
		voice.ussd_reply[(*at)++] = (char)c;
	} else if (c < 0x800) {
		voice.ussd_reply[(*at)++] = (char)(0xc0 | c >> 6);
		voice.ussd_reply[(*at)++] = (char)(0x80 | (c & 0x3f));
	} else {
		voice.ussd_reply[(*at)++] = (char)(0xe0 | c >> 12);
		voice.ussd_reply[(*at)++] = (char)(0x80 | (c >> 6 & 0x3f));
		voice.ussd_reply[(*at)++] = (char)(0x80 | (c & 0x3f));
	}
}

static void
ussd_take(const uint8_t *value, size_t size, bool wide)
{
	size_t at = 0;

	if (wide) {
		unsigned units = size >= 1 ? value[0] : 0;

		for (unsigned i = 0; i < units && 1 + 2 * i + 1 < size; i++) {
			reply_put(&at, value[1 + 2 * i] | (uint32_t)value[2 + 2 * i] << 8);
		}
	} else if (size >= 2 && value[0] == USS_UCS2) {
		unsigned n = value[1];

		for (unsigned i = 0; i + 1 < n && 2 + i + 1 < size; i += 2) {
			reply_put(&at, (uint32_t)value[2 + i] << 8 | value[3 + i]);
		}
	} else if (size >= 2) {
		unsigned n = value[1];

		for (unsigned i = 0; i < n && 2 + i < size; i++) {
			reply_put(&at, value[2 + i] & 0x7f);
		}
	}
	voice.ussd_reply[at] = 0;
}

static void
calls_take(const uint8_t *value, size_t size)
{
	unsigned count = size >= 1 ? value[0] : 0, kept = 0;

	for (unsigned i = 0; i < count && 1 + 7 * i + 7 <= size; i++) {
		const uint8_t *call = value + 1 + 7 * i;

		/* id, state, type, direction, mode, multiparty, line */
		if (call[1] == 9) {
			printf("voice: call %u has ended\n", call[0]);
			continue;
		}
		if (kept < CALLS) {
			char number[32] = "";

			for (unsigned k = 0; k < voice.calls; k++) {
				if (voice.call[k].id == call[0]) {
					strlcpy(number, voice.call[k].number, sizeof(number));
				}
			}
			if (kept >= voice.calls || voice.call[kept].id != call[0] || voice.call[kept].state != call[1]) {
				printf("voice: call %u: %s (state %u), %s\n", call[0], call_state_name(call[1]), call[1],
				    call[3] == 1 ? "made here" : call[3] == 2 ? "COMING IN" : "direction unknown");
			}
			voice.call[kept].id = call[0];
			voice.call[kept].state = call[1];
			voice.call[kept].type = call[2];
			voice.call[kept].direction = call[3];
			strlcpy(voice.call[kept].number, number, sizeof(voice.call[kept].number));
			kept++;
		}
	}
	voice.calls = kept;
}

static void
numbers_take(const uint8_t *value, size_t size)
{
	unsigned count = size >= 1 ? value[0] : 0;
	size_t at = 1;

	for (unsigned i = 0; i < count && at + 3 <= size; i++) {
		unsigned id = value[at], n = value[at + 2];

		if (at + 3 + n > size) {
			break;
		}
		for (unsigned k = 0; k < voice.calls; k++) {
			if (voice.call[k].id == id && voice.call[k].number[0] == 0) {
				unsigned m = n < sizeof(voice.call[k].number) - 1 ? n : sizeof(voice.call[k].number) - 1;

				memcpy(voice.call[k].number, value + at + 3, m);
				voice.call[k].number[m] = 0;
				printf("voice: call %u is with %s\n", id, voice.call[k].number);
			}
		}
		at += 3 + n;
	}
}

static void
voice_receive(__unused void *context, uint32_t node, uint32_t port, const void *frame, size_t length)
{
	const uint8_t *data = frame;
	uint16_t message, result = 0, error = 0, cause = 0;
	const uint8_t *calls = NULL, *numbers = NULL, *text = NULL, *wide = NULL;
	size_t calls_size = 0, numbers_size = 0, text_size = 0, wide_size = 0;
	unsigned notification = 0;
	bool answered = false;

	if (length < 7 || (data[0] != QMI_RESPONSE && data[0] != QMI_INDICATION)) {
		return;
	}
	message = (uint16_t)(data[3] | data[4] << 8);
	if ((size_t)(data[5] | data[6] << 8) != length - 7) {
		return;
	}
	for (size_t at = 7; at + 3 <= length;) {
		uint8_t type = data[at];
		size_t size = data[at + 1] | (size_t)data[at + 2] << 8;
		const uint8_t *value = data + at + 3;

		at += 3;
		if (size > length - at) {
			return;
		}
		at += size;
		if (data[0] == QMI_RESPONSE && type == 2 && size == 4) {
			result = (uint16_t)(value[0] | value[1] << 8);
			error = (uint16_t)(value[2] | value[3] << 8);
			answered = true;
		} else if (message == VOICE_ALL_CALL_STATUS && type == 0x01) {
			calls = value; calls_size = size;
		} else if (message == VOICE_ALL_CALL_STATUS && type == 0x10) {
			numbers = value; numbers_size = size;
		} else if (message == VOICE_ORIGINATE_USSD && type == 0x10 && size >= 2) {
			cause = (uint16_t)(value[0] | value[1] << 8);
		} else if (message == VOICE_ORIGINATE_USSD && type == 0x12) {
			text = value; text_size = size;
		} else if (message == VOICE_ORIGINATE_USSD && type == 0x16) {
			wide = value; wide_size = size;
		} else if (message == VOICE_USSD && type == 0x01 && size >= 1) {
			notification = value[0];
		} else if (message == VOICE_USSD && type == 0x10) {
			text = value; text_size = size;
		} else if (message == VOICE_USSD && type == 0x11) {
			wide = value; wide_size = size;
		}
	}
	lck_mtx_lock(&voice_lock);
	if (!voice.connected || node != voice.node || port != voice.to) {
		lck_mtx_unlock(&voice_lock);
		return;
	}
	switch (message) {
	case VOICE_ALL_CALL_STATUS:
		if (calls != NULL) {
			calls_take(calls, calls_size);
		}
		if (numbers != NULL) {
			numbers_take(numbers, numbers_size);
		}
		break;
	case VOICE_DIAL_CALL:
	case VOICE_END_CALL:
	case VOICE_ANSWER_CALL:
	case VOICE_INDICATION_REGISTER:
	case VOICE_ANSWER_USSD:
	case VOICE_CANCEL_USSD:
		if (answered) {
			printf("voice: the modem answers %s: %s (result %u, error %u)\n", message == VOICE_DIAL_CALL ? "DIAL" :
			    message == VOICE_END_CALL ? "END" : message == VOICE_ANSWER_CALL ? "ANSWER" : message ==
			    VOICE_INDICATION_REGISTER ? "the registration for its notices" : message == VOICE_ANSWER_USSD ?
			    "the USSD answer" : "the USSD cancel", result == 0 ? "done" : "REFUSED", result, error);
			if (result != 0 && (message == VOICE_ANSWER_USSD)) {
				voice.ussd_state = -1;
			}
			if (message == VOICE_CANCEL_USSD && result == 0) {
				voice.ussd_state = 0;
			}
		}
		break;
	case VOICE_ORIGINATE_USSD:
		if (!answered) {
			break;
		}
		if (result != 0) {
			voice.ussd_state = -1;
			snprintf(voice.ussd_reply, sizeof(voice.ussd_reply), "refused: error %u, cause %u", error, cause);
			printf("voice: USSD REFUSED: result %u, error %u, failure cause %u\n", result, error, cause);
			break;
		}
		voice.ussd_reply[0] = 0;
		if (wide != NULL) {
			ussd_take(wide, wide_size, true);
		}
		if (voice.ussd_reply[0] == 0 && text != NULL) {
			ussd_take(text, text_size, false);
		}
		voice.ussd_state = 2;
		printf("voice: USSD, the network's answer: \"%s\"\n", voice.ussd_reply);
		break;
	case VOICE_USSD:
		voice.ussd_reply[0] = 0;
		if (wide != NULL) {
			ussd_take(wide, wide_size, true);
		}
		if (voice.ussd_reply[0] == 0 && text != NULL) {
			ussd_take(text, text_size, false);
		}
		voice.ussd_state = notification == 2 ? 3 : 2;
		printf("voice: USSD, the network says%s: \"%s\"\n", notification == 2 ? " (and waits for an answer)" : "",
		    voice.ussd_reply);
		break;
	case VOICE_USSD_RELEASE:
		printf("voice: USSD: the network has ended the session\n");
		if (voice.ussd_state == 3 || voice.ussd_state == 1) {
			voice.ussd_state = voice.ussd_reply[0] != 0 ? 2 : 0;
		}
		break;
	default:
		break;
	}
	lck_mtx_unlock(&voice_lock);
}

static void
voice_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
	static const uint8_t call_events[] = { 0x13, 1, 0, 1 }, ussd_events[] = { 0x16, 1, 0, 1 };

	if (service != QMI_VOICE) {
		return;
	}
	lck_mtx_lock(&voice_lock);
	if (arrived && !voice.connected) {
		voice.connected = true;
		voice.node = node;
		voice.to = port;
		printf("voice: the modem's voice service is at node %u port %u\n", node, port);
		/* (one request for each kind of notice: a modem that does not know one of them still takes the other) */
		(void)voice_ask(VOICE_INDICATION_REGISTER, call_events, sizeof(call_events));
		(void)voice_ask(VOICE_INDICATION_REGISTER, ussd_events, sizeof(ussd_events));
	} else if (!arrived && voice.connected && node == voice.node && port == voice.to) {
		voice.connected = false;
		voice.calls = 0;
		voice.ussd_state = 0;
		printf("voice: the modem's voice service has gone\n");
	}
	lck_mtx_unlock(&voice_lock);
}

int
hakios_voice_open(const struct hakios_qrtr_ops *transport)
{
	int error;

	if (transport == NULL || transport->open == NULL || transport->lookup == NULL || transport->send == NULL ||
	    transport->close == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&voice_lock);
	if (voice.port != NULL) {
		lck_mtx_unlock(&voice_lock);
		return 0;
	}
	voice.transport = transport;
	voice.connected = false;
	voice.calls = 0;
	voice.ussd_state = 0;
	lck_mtx_unlock(&voice_lock);
	/* (the port is opened without the lock: its callbacks take it) */
	{
		struct hakios_qrtr_port *port = transport->open(voice_receive, voice_server, NULL);

		if (port == NULL) {
			return ENXIO;
		}
		lck_mtx_lock(&voice_lock);
		voice.port = port;
		lck_mtx_unlock(&voice_lock);
		error = transport->lookup(port, QMI_VOICE, 0, 0);
	}
	if (error != 0) {
		hakios_voice_close();
	}
	return error;
}

void
hakios_voice_close(void)
{
	struct hakios_qrtr_port *port;
	const struct hakios_qrtr_ops *transport;

	lck_mtx_lock(&voice_lock);
	port = voice.port;
	transport = voice.transport;
	voice.port = NULL;
	voice.connected = false;
	voice.calls = 0;
	lck_mtx_unlock(&voice_lock);
	if (port != NULL && transport != NULL) {
		transport->close(port);
	}
}

/* ---- what user space is given ------------------------------------------------------------------- */

SYSCTL_DECL(_kern_hakios);

static bool
dialable(const char *text)
{
	if (text[0] == 0) {
		return false;
	}
	for (const char *c = text; *c != 0; c++) {
		if (!((*c >= '0' && *c <= '9') || *c == '+' || *c == '*' || *c == '#')) {
			return false;
		}
	}
	return true;
}

static int
sysctl_dial SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	char number[40] = "";
	uint8_t fields[64];
	size_t length;
	int error = sysctl_handle_string(oidp, number, sizeof(number), req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (!dialable(number)) {
		return EINVAL;
	}
	length = text_field(fields, sizeof(fields), 0x01, NULL, 0, number);
	lck_mtx_lock(&voice_lock);
	error = voice_ask(VOICE_DIAL_CALL, fields, length);
	lck_mtx_unlock(&voice_lock);
	printf("voice: asked to call %s: %s\n", number, error == 0 ? "sent to the modem" : "NOT SENT");
	return error;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_dial, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_dial, "A",
    "write a number: a voice call is made to it");

/* Answer or end: the id written, or with 0 the call that rings (answer) or every call (end). */
static int
call_command(struct sysctl_oid *oidp, struct sysctl_req *req, uint16_t message)
{
	int id = 0, error = sysctl_handle_int(oidp, &id, 0, req), sent = 0;

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	lck_mtx_lock(&voice_lock);
	error = ENOENT;
	for (unsigned i = 0; i < voice.calls; i++) {
		bool wanted = id != 0 ? voice.call[i].id == id : message == VOICE_END_CALL || voice.call[i].state == 2 ||
		    voice.call[i].state == 7;

		if (wanted) {
			const uint8_t field[] = { 0x01, 1, 0, voice.call[i].id };

			error = voice_ask(message, field, sizeof(field));
			sent++;
		}
	}
	lck_mtx_unlock(&voice_lock);
	printf("voice: asked to %s call %d: %d sent to the modem\n", message == VOICE_END_CALL ? "end" : "answer", id, sent);
	return error;
}

static int
sysctl_answer SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	return call_command(oidp, req, VOICE_ANSWER_CALL);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_answer, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_answer, "I",
    "write a call's id (0: the one that rings): it is answered");

static int
sysctl_hangup SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	return call_command(oidp, req, VOICE_END_CALL);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_hangup, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_hangup, "I",
    "write a call's id (0: every call): it is ended");

static int
sysctl_calls SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	char text[256] = "";
	size_t at = 0;

	lck_mtx_lock(&voice_lock);
	for (unsigned i = 0; i < voice.calls && at + 80 < sizeof(text); i++) {
		at += (size_t)snprintf(text + at, sizeof(text) - at, "%s%u %s %s %s", i != 0 ? "; " : "", voice.call[i].id,
		    call_state_name(voice.call[i].state), voice.call[i].direction == 2 ? "in" : "out",
		    voice.call[i].number[0] != 0 ? voice.call[i].number : "-");
	}
	lck_mtx_unlock(&voice_lock);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_calls, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_calls, "A",
    "the calls there are: id, state, direction, number");

static int
sysctl_call_state SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	int state;

	lck_mtx_lock(&voice_lock);
	state = voice.calls != 0 ? voice.call[0].state : 0;
	lck_mtx_unlock(&voice_lock);
	return SYSCTL_OUT(req, &state, sizeof(state));
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_call_state, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_call_state,
    "I", "the first call's state: 0 none, 1 origination, 2 incoming, 3 conversation, 5 alerting, 6 hold, 8 disconnecting");

static int
sysctl_ussd SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	char code[184] = "";
	uint8_t fields[200], lead[2];
	size_t length;
	bool answer;
	int error = sysctl_handle_string(oidp, code, sizeof(code), req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (code[0] == 0 || strlen(code) > 180) {
		return EINVAL;
	}
	lead[0] = USS_ASCII;
	lead[1] = (uint8_t)strlen(code);
	length = text_field(fields, sizeof(fields), 0x01, lead, sizeof(lead), code);
	lck_mtx_lock(&voice_lock);
	answer = voice.ussd_state == 3;
	error = voice_ask(answer ? VOICE_ANSWER_USSD : VOICE_ORIGINATE_USSD, fields, length);
	if (error == 0) {
		voice.ussd_state = 1;
		voice.ussd_reply[0] = 0;
	}
	lck_mtx_unlock(&voice_lock);
	printf("voice: USSD \"%s\" %s: %s\n", code, answer ? "as the answer the network waits for" : "asked", error == 0 ?
	    "sent to the modem" : "NOT SENT");
	return error;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_ussd, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_ussd, "A",
    "write a USSD code, or the answer the network waits for");

static int
sysctl_ussd_reply SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	char text[sizeof(voice.ussd_reply)];

	lck_mtx_lock(&voice_lock);
	strlcpy(text, voice.ussd_reply, sizeof(text));
	lck_mtx_unlock(&voice_lock);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_ussd_reply, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_ussd_reply,
    "A", "what the network said to the last USSD code");

static int
sysctl_ussd_state SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	int state;

	lck_mtx_lock(&voice_lock);
	state = voice.ussd_state;
	lck_mtx_unlock(&voice_lock);
	return SYSCTL_OUT(req, &state, sizeof(state));
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_ussd_state, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_ussd_state, "I",
    "USSD: 0 nothing asked, 1 waiting for the network, 2 an answer is there, 3 an answer is there and ours is waited for, -1 failed");

static int
sysctl_ussd_cancel SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int yes = 0, error = sysctl_handle_int(oidp, &yes, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL || yes == 0) {
		return error;
	}
	lck_mtx_lock(&voice_lock);
	error = voice_ask(VOICE_CANCEL_USSD, NULL, 0);
	lck_mtx_unlock(&voice_lock);
	return error;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, cellular_ussd_cancel, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_ussd_cancel,
    "I", "write 1: the USSD session is ended");
