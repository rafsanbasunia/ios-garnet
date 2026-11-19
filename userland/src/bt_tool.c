/*
 * bt_tool: in the place of Darwin's BlueTool, for a controller that is not Broadcom's.
 *
 * Before it opens its serial port, Darwin's Bluetooth daemon asks the service com.apple.BlueTool to run the
 * script "boot", which on an iPhone sends the chip its firmware and its address. Here the kernel has done both
 * by the time the controller is a program's (drivers/bluetooth/hakios.btqca.c), and BlueTool itself knows no
 * script for this phone and waits until it is given up on. This answers in its place: whatever is asked is done.
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* (what is used of libxpc, which the SDK built against has no headers for) */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION                     (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY                     (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER    (1u << 0)
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, void (^handler)(xpc_object_t object));
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_type_t xpc_get_type(xpc_object_t object);
extern char *xpc_copy_description(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
extern void xpc_release(xpc_object_t object);

static void
say(const char *format, ...)
{
	static int console = -1;
	char line[600];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	if (length > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1);
	}
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			return;
		}
		xpc_connection_set_event_handler((xpc_connection_t)peer, ^(xpc_object_t message) {
			xpc_object_t reply;
			char *text;

			if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
				return;
			}
			text = xpc_copy_description(message);
			say("bt_tool: asked: %s\n", text != NULL ? text : "?");
			free(text);
			reply = xpc_dictionary_create_reply(message);
			if (reply != NULL) {
				xpc_dictionary_set_int64(reply, "BlueToolBTResult", 0);
				xpc_connection_send_message((xpc_connection_t)peer, reply);
				xpc_release(reply);
			}
		});
		xpc_connection_resume((xpc_connection_t)peer);
	});
	xpc_connection_resume(listener);
	say("bt_tool: answering for com.apple.BlueTool\n");
	dispatch_main();
}
