/*
 * libhakios.phone.dylib: code of ours inside the Phone application (see inject.c for how it gets
 * there; build.sh's INJECT names the program).
 *
 * The Phone application is Darwin's own: its keypad, its recent calls, its contacts. What it cannot
 * have here is the daemon it hands a call to (callservicesd, which talks to Apple's baseband through
 * CommCenter). These things are done about that, inside the application:
 *   - its objects are handed a contact store that is nil here, and without one it keeps to two tabs
 *     (recent calls and the keypad): a store is made for them, and the favorites and the contacts
 *     are shown as well;
 *   - TUCallCapabilities is made to say that the phone can make telephone calls, and
 *     TUCallProviderManager to have a provider of them;
 *   - what the application asks of TUCallCenter for a call (a TUDialRequest), or failing that what
 *     is written on the keypad when its call button is pressed, is given to this port's modem driver:
 *     sysctl kern.hakios.cellular_dial for a number, kern.hakios.cellular_ussd for a USSD code
 *     (drivers/modem/hakios.voice.c).
 * The call's own screen is SpringBoard's prompt (inject_sb.c), which follows the driver's call state;
 * a notification tells it that a USSD code was sent, so that it shows the network's answer.
 * Calls are not written into the list of recent calls, and there is no voicemail.
 */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <notify.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PHONE_USSD_SENT "com.hakios.phone.ussd"

static void
say(const char *format, ...)
{
	static int console = -1;
	char line[400];
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

static id
send(id to, const char *name)
{
	SEL sel = sel_registerName(name);

	return to != nil && class_respondsToSelector(object_getClass(to), sel) ? ((id (*)(id, SEL))objc_msgSend)(to, sel) : nil;
}

/* The number of a dial request: what its handle holds, without the spaces and dashes of a formatted number. */
static void
request_number(id request, char *to, size_t room)
{
	id value = send(send(request, "handle"), "value");
	const char *text = value != nil ? ((const char *(*)(id, SEL))objc_msgSend)(value, sel_registerName("UTF8String")) : NULL;
	size_t n = 0;

	for (; text != NULL && *text != 0 && n + 1 < room; text++) {
		if (strchr("0123456789*#+", *text) != NULL) {
			to[n++] = *text;
		}
	}
	to[n] = 0;
}

static unsigned dials;          /* how many times the application came as far as asking for a call */

static void
dial_number(const char *number, const char *through)
{
	static time_t last;
	size_t n = strlen(number);
	time_t now = time(NULL);

	dials++;
	if (n == 0) {
		say("hakios: phone: %s: a request without a number\n", through);
		return;
	}
	if (now - last < 2) {           /* one press of the button, asked twice */
		return;
	}
	last = now;
	if (number[0] == '*' || number[0] == '#' || number[n - 1] == '#') {
		if (sysctlbyname("kern.hakios.cellular_ussd", NULL, NULL, number, n + 1) == 0) {
			notify_post(PHONE_USSD_SENT);
			say("hakios: phone: %s: a USSD code was sent\n", through);
		} else {
			say("hakios: phone: %s: the USSD code could not be sent (%d)\n", through, errno);
		}
	} else if (sysctlbyname("kern.hakios.cellular_dial", NULL, NULL, number, n + 1) == 0) {
		say("hakios: phone: %s: a call was asked for\n", through);
	} else {
		say("hakios: phone: %s: the call could not be asked for (%d)\n", through, errno);
	}
}

static void
dial(id request, const char *through)
{
	char number[80];

	request_number(request, number, sizeof(number));
	dial_number(number, through);
}

/* -[TUCallCenter dialWithRequest:completion:], whose block is given the call: there is none of Darwin's. */
static void
dial_with_completion(__unused id self, __unused SEL sel, id request, void (^completion)(id call))
{
	dial(request, "dialWithRequest:completion:");
	if (completion != NULL) {
		completion(nil);
	}
}

/* -[TUCallCenter dialWithRequest:], which returns the call. */
static id
dial_with_request(__unused id self, __unused SEL sel, id request)
{
	dial(request, "dialWithRequest:");
	return nil;
}

/* -[TUCallCenter launchAppForDialRequest:completion:], whose block is given an error, or none. */
static void
launch_for_request(__unused id self, __unused SEL sel, id request, void (^completion)(id error))
{
	dial(request, "launchAppForDialRequest:completion:");
	if (completion != NULL) {
		completion(nil);
	}
}

static BOOL
yes(__unused id self, __unused SEL sel)
{
	return YES;
}

static bool
replace(Class cls, const char *name, IMP with)
{
	Method method = cls != Nil ? class_getInstanceMethod(cls, sel_registerName(name)) : NULL;

	if (method == NULL) {
		say("hakios: phone: no %s to replace\n", name);
		return false;
	}
	method_setImplementation(method, with);
	return true;
}

/*
 * The provider of telephone calls. The application asks TUCallProviderManager for it before it does
 * anything about a call, and the manager has it from callservicesd: there is none, so one is made here.
 */
static id
telephony_provider(__unused id self, __unused SEL sel)
{
	static id provider;
	Class cls = objc_getClass("TUCallProvider");
	SEL make = sel_registerName("initWithIdentifier:"), types = sel_registerName("setSupportedHandleTypes:");
	SEL audio = sel_registerName("setSupportsAudioOnly:");

	if (provider == nil && cls != Nil && class_respondsToSelector(cls, make)) {
		id name = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"),
		              "com.apple.coretelephony");

		provider = ((id (*)(id, SEL, id))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc")), make, name);
		if (provider != nil && class_respondsToSelector(cls, types)) {
			/* a telephone number (TUHandleTypePhoneNumber, 2) */
			id two = ((id (*)(Class, SEL, long))objc_msgSend)(objc_getClass("NSNumber"), sel_registerName("numberWithInteger:"), 2L);
			id set = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSSet"), sel_registerName("setWithObject:"), two);

			((void (*)(id, SEL, id))objc_msgSend)(provider, types, set);
		}
		if (provider != nil && class_respondsToSelector(cls, audio)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(provider, audio, YES);
		}
		say("hakios: phone: a provider of telephone calls %s\n", provider != nil ? "made" : "could not be made");
	}
	return provider;
}

/*
 * The keypad's call button, seen where UIKit sends a control's action. The application's own method is let run
 * first; where it gives up before it asks TUCallCenter for anything, what is written on the keypad is dialled
 * from here.
 */
static BOOL (*original_send_action)(id self, SEL sel, SEL action, id target, id sender, id event);

static BOOL
send_action(id self, SEL sel, SEL action, id target, id sender, id event)
{
	const char *name = action != NULL ? sel_getName(action) : "";
	unsigned before = dials;
	BOOL result;
	id view, text;
	const char *typed;
	char number[80];
	size_t n = 0;

	if (strcasestr(name, "callButton") == NULL) {
		return original_send_action(self, sel, action, target, sender, event);
	}
	if (target != nil) {
		((void (*)(id, SEL))objc_msgSend)(target, sel_registerName("retain"));
	}
	result = original_send_action(self, sel, action, target, sender, event);
	if (dials == before && target != nil) {
		view = send(target, "dialerView");
		text = send(send(view != nil ? view : target, "lcdView"), "text");
		typed = text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : NULL;
		for (; typed != NULL && *typed != 0 && n + 1 < sizeof(number); typed++) {
			if (strchr("0123456789*#+", *typed) != NULL) {
				number[n++] = *typed;
			}
		}
		number[n] = 0;
		say("hakios: phone: %s of %s: the application asked for nothing, %zu characters on the keypad\n", name,
		    class_getName(object_getClass(target)), n);
		if (n != 0) {
			dial_number(number, "the call button");
		}
	}
	if (target != nil) {
		((void (*)(id, SEL))objc_msgSend)(target, sel_registerName("release"));
	}
	return result;
}

/* An exception that nothing catches ends the application: its last words, for the console. */
extern void NSSetUncaughtExceptionHandler(void (*handler)(id exception));

static void
uncaught(id exception)
{
	id name = send(exception, "name"), reason = send(exception, "reason");

	say("hakios: phone: an exception nothing caught: %s: %s\n",
	    name != nil ? ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : "?",
	    reason != nil ? ((const char *(*)(id, SEL))objc_msgSend)(reason, sel_registerName("UTF8String")) : "?");
	{
		id frames = send(exception, "callStackSymbols");
		unsigned long count = frames != nil ? ((unsigned long (*)(id, SEL))objc_msgSend)(frames, sel_registerName("count")) : 0;

		for (unsigned long f = 0; f < count && f < 16; f++) {
			id frame = ((id (*)(id, SEL, unsigned long))objc_msgSend)(frames, sel_registerName("objectAtIndex:"), f);

			say("hakios: phone:   %s\n", ((const char *(*)(id, SEL))objc_msgSend)(frame, sel_registerName("UTF8String")));
		}
	}
}

/*
 * The contacts. The application's objects are handed a contact store that is nil here, and the tabs that list
 * contacts will not be made without one: where one of the application's own classes answers nil for its store,
 * a store made here is answered instead.
 */
static id
contact_store(void)
{
	static id store;
	static bool tried;
	Class cls = objc_getClass("CNContactStore");

	if (!tried) {
		tried = true;
		store = cls != Nil ? ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(cls, sel_registerName("alloc")),
		            sel_registerName("init")) : nil;
		say("hakios: phone: a contact store %s\n", store != nil ? "made" : "could not be made");
	}
	return store;
}

static void
contact_store_getters(void)
{
	static const char *const getters[] = { "contactStore", "suggestedContactStore" };
	unsigned count = 0, wrapped = 0;
	Class one = objc_getClass("PhoneTabBarController");
	const char *image = one != Nil ? class_getImageName(one) : NULL;
	const char **names = image != NULL ? objc_copyClassNamesForImage(image, &count) : NULL;

	for (unsigned c = 0; names != NULL && c < count; c++) {
		Class cls = objc_getClass(names[c]);
		unsigned methods = 0;
		Method *list = cls != Nil ? class_copyMethodList(cls, &methods) : NULL;

		for (unsigned m = 0; m < methods; m++) {
			SEL sel = method_getName(list[m]);

			for (unsigned g = 0; g < sizeof(getters) / sizeof(getters[0]); g++) {
				if (sel == sel_registerName(getters[g])) {
					id (*original)(id, SEL) = (id (*)(id, SEL))method_getImplementation(list[m]);

					id (^getter)(id) = ^id(id self) {
						id store = original(self, sel);

						return store != nil ? store : contact_store();
					};

					method_setImplementation(list[m], imp_implementationWithBlock((id)getter));
					wrapped++;
				}
			}
		}
		free(list);
	}
	free(names);
	say("hakios: phone: %u classes of the application, %u contact store getters wrapped\n", count, wrapped);
}

static bool
hook(void)
{
	static const char *const can[] = { "supportsTelephonyCalls", "supportsDisplayingTelephonyCalls", "supportsPrimaryCalling" };
	Class center = objc_getClass("TUCallCenter"), capabilities = objc_getClass("TUCallCapabilities");
	unsigned done = 0;

	if (center == Nil) {
		return false;
	}
	done += replace(center, "dialWithRequest:completion:", (IMP)dial_with_completion);
	done += replace(center, "dialWithRequest:", (IMP)dial_with_request);
	done += replace(center, "launchAppForDialRequest:completion:", (IMP)launch_for_request);
	for (unsigned c = 0; capabilities != Nil && c < sizeof(can) / sizeof(can[0]); c++) {
		done += replace(object_getClass((id)capabilities), can[c], (IMP)yes);
	}
	{
		Class manager = objc_getClass("TUCallProviderManager"), application = objc_getClass("UIApplication");
		Method action = application != Nil ? class_getInstanceMethod(application, sel_registerName("sendAction:to:from:forEvent:")) : NULL;

		if (manager != Nil) {
			done += replace(manager, "telephonyProvider", (IMP)telephony_provider);
			done += replace(manager, "defaultProvider", (IMP)telephony_provider);
		}
		if (action != NULL) {
			original_send_action = (BOOL (*)(id, SEL, SEL, id, id, id))method_setImplementation(action, (IMP)send_action);
		} else {
			say("hakios: phone: no -[UIApplication sendAction:to:from:forEvent:] to replace\n");
		}
		NSSetUncaughtExceptionHandler(uncaught);
		contact_store_getters();
	}
	say("hakios: phone: %u of TelephonyUtilities' methods replaced: the call button goes to the modem driver\n", done);
	return true;
}

__attribute__((constructor))
static void
phone_start(void)
{
	if (!hook()) {
		dispatch_async(dispatch_get_main_queue(), ^{
			if (!hook()) {
				say("hakios: phone: no TUCallCenter in this program\n");
			}
		});
	}
}
