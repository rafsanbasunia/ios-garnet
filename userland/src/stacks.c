/*
 * stacks NAME...: where every thread of the named processes is, for a process that has stopped answering.
 *
 * For each thread: its run state, its name, and its call chain (pc, lr, then the frame-pointer chain), each address
 * as image+offset and the nearest symbol. Symbols are looked up here, in this process: the images the target has
 * loaded are opened here too, and an address is moved by the difference of the two load addresses (for the dyld
 * shared cache that is nothing: every process maps it at the same slide). The nearest symbol is the nearer of what
 * dladdr() says and the nearest Objective-C method of the image's classes, since most of the system's code is
 * methods, which dladdr() does not know.
 *
 * Needs a task port of the target: tried as read, then control, then inspect (entitlements: src/stacks.entitlements).
 * A system process's port is refused to a program that is not a platform binary unless the kernel is started with
 * cs_relax_platform_task_ports=1 (a development kernel only).
 *
 * Build (tools/garnet-wifi-image.sh puts out/stacks into the image as /redwood/bin/stacks):
 *   xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -Wall -o out/stacks userland/src/stacks.c -lobjc
 *   ldid -Suserland/src/stacks.entitlements out/stacks
 * Run: the image's jobs from devices/garnet/rc.jobs.stacks, the kernel with cs_relax_platform_task_ports=1; the
 * stacks are in the kernel log.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
extern kern_return_t mach_vm_read_overwrite(vm_map_read_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t, mach_vm_size_t *);
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int proc_listallpids(void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, uint32_t buffersize);
extern kern_return_t task_read_for_pid(mach_port_t target_tport, int pid, mach_port_t *t);
extern kern_return_t task_inspect_for_pid(mach_port_t target_tport, int pid, mach_port_t *t);

#define MAX_FRAMES 48

struct image {
	uint64_t base, size;
	char path[512];
	intptr_t here;          /* our load address of the same image, 0 if not opened here */
};
static struct image *images;
static unsigned image_count;

static int
peek(task_t task, uint64_t addr, void *out, size_t size)
{
	mach_vm_size_t got = 0;

	return mach_vm_read_overwrite(task, addr, size, (mach_vm_address_t)out, &got) == KERN_SUCCESS && got == size ? 0 : -1;
}

/* the extent of the image's __TEXT, from its header in the target */
static uint64_t
text_size(task_t task, uint64_t base)
{
	struct mach_header_64 mh;
	uint8_t cmds[16384];
	uint32_t off = 0;

	if (peek(task, base, &mh, sizeof(mh)) || mh.magic != MH_MAGIC_64 || mh.sizeofcmds > sizeof(cmds) ||
	    peek(task, base + sizeof(mh), cmds, mh.sizeofcmds)) {
		return 0;
	}
	for (uint32_t i = 0; i < mh.ncmds && off + sizeof(struct load_command) <= mh.sizeofcmds; i++) {
		struct load_command *lc = (struct load_command *)(cmds + off);
		if (lc->cmd == LC_SEGMENT_64) {
			struct segment_command_64 *sc = (struct segment_command_64 *)lc;
			if (strcmp(sc->segname, "__TEXT") == 0) {
				return sc->vmsize;
			}
		}
		off += lc->cmdsize;
	}
	return 0;
}

static void
load_images(task_t task)
{
	struct task_dyld_info info;
	mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos all;

	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &count) != KERN_SUCCESS ||
	    peek(task, info.all_image_info_addr, &all, sizeof(all))) {
		printf("stacks: no image list\n");
		return;
	}
	images = calloc(all.infoArrayCount + 1, sizeof(*images));
	for (uint32_t i = 0; i < all.infoArrayCount; i++) {
		struct dyld_image_info ii;
		struct image *im = &images[image_count];

		if (peek(task, (uint64_t)(uintptr_t)all.infoArray + i * sizeof(ii), &ii, sizeof(ii))) {
			continue;
		}
		im->base = (uint64_t)(uintptr_t)ii.imageLoadAddress;
		im->size = text_size(task, im->base);
		/* the path: read a piece at a time, a string may end near a page's end */
		for (unsigned n = 0; n < sizeof(im->path) - 1; n += 64) {
			if (peek(task, (uint64_t)(uintptr_t)ii.imageFilePath + n, im->path + n, 64)) {
				break;
			}
			if (memchr(im->path + n, 0, 64)) {
				break;
			}
		}
		im->path[sizeof(im->path) - 1] = 0;
		image_count++;
	}
	printf("stacks: %u images, the shared cache's slide 0x%lx\n", image_count, (unsigned long)all.sharedCacheSlide);
}

static struct image *
image_of(uint64_t addr)
{
	for (unsigned i = 0; i < image_count; i++) {
		if (addr >= images[i].base && addr < images[i].base + (images[i].size ? images[i].size : 0x1000000)) {
			return &images[i];
		}
	}
	return NULL;
}

/* opens the image here (once) and finds where it is */
static intptr_t
here_of(struct image *im)
{
	if (im->here) {
		return im->here;
	}
	void *h = dlopen(im->path, RTLD_LAZY | RTLD_NOLOAD);
	if (!h) {
		h = dlopen(im->path, RTLD_LAZY | RTLD_LOCAL);
	}
	if (!h) {
		im->here = -1;
		return -1;
	}
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		if (strcmp(_dyld_get_image_name(i), im->path) == 0) {
			im->here = (intptr_t)_dyld_get_image_header(i);
			return im->here;
		}
	}
	im->here = -1;
	return -1;
}

/* the nearest method of the image's classes at or under addr */
static const char *
nearest_method(const char *path, uintptr_t addr, uintptr_t *at, char *buf, size_t size)
{
	unsigned int n = 0;
	const char **names = objc_copyClassNamesForImage(path, &n);
	uintptr_t best = 0;

	buf[0] = 0;
	for (unsigned i = 0; names && i < n; i++) {
		Class c = objc_getClass(names[i]);
		for (int meta = 0; c && meta < 2; meta++) {
			Class k = meta ? object_getClass((id)c) : c;
			unsigned int m = 0;
			Method *ms = class_copyMethodList(k, &m);
			for (unsigned j = 0; j < m; j++) {
				uintptr_t imp = (uintptr_t)method_getImplementation(ms[j]);
				if (imp <= addr && imp > best) {
					best = imp;
					snprintf(buf, size, "%c[%s %s]", meta ? '+' : '-', names[i], sel_getName(method_getName(ms[j])));
				}
			}
			free(ms);
		}
	}
	free(names);
	*at = best;
	return buf[0] ? buf : NULL;
}

static void
say_frame(int n, uint64_t addr)
{
	struct image *im = image_of(addr);
	char method[512];

	if (!im) {
		printf("    %2d 0x%llx\n", n, (unsigned long long)addr);
		return;
	}
	const char *file = strrchr(im->path, '/') ? strrchr(im->path, '/') + 1 : im->path;
	uint64_t off = addr - im->base;
	intptr_t here = here_of(im);
	if (here == -1) {
		printf("    %2d %s+0x%llx\n", n, file, (unsigned long long)off);
		return;
	}
	uintptr_t local = (uintptr_t)here + off;
	Dl_info dl = { 0 };
	uintptr_t at_m = 0;
	const char *m = nearest_method(im->path, local, &at_m, method, sizeof(method));
	dladdr((void *)local, &dl);
	uintptr_t at_s = dl.dli_sname ? (uintptr_t)dl.dli_saddr : 0;
	if (m && at_m >= at_s) {
		printf("    %2d %s+0x%llx %s+%lu\n", n, file, (unsigned long long)off, m, (unsigned long)(local - at_m));
	} else if (at_s) {
		printf("    %2d %s+0x%llx %s+%lu\n", n, file, (unsigned long long)off, dl.dli_sname, (unsigned long)(local - at_s));
	} else {
		printf("    %2d %s+0x%llx\n", n, file, (unsigned long long)off);
	}
}

static void
dump(int pid, const char *name)
{
	task_t task = MACH_PORT_NULL;
	const char *kind = "read";

	/* the read port first: a control port of another process is refused without Developer Mode */
	if (task_read_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
		kind = "control";
		if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
			kind = "inspect";
			if (task_inspect_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
				printf("stacks: %s (%d): no task port of any kind\n", name, pid);
				return;
			}
		}
	}
	printf("stacks: === %s (%d), %s port ===\n", name, pid, kind);
	free(images);
	images = NULL;
	image_count = 0;
	load_images(task);

	thread_act_array_t threads;
	mach_msg_type_number_t nthreads = 0;
	if (task_threads(task, &threads, &nthreads) != KERN_SUCCESS) {
		printf("stacks: no threads\n");
		return;
	}
	for (unsigned t = 0; t < nthreads; t++) {
		thread_extended_info_data_t ext;
		mach_msg_type_number_t c = THREAD_EXTENDED_INFO_COUNT;
		arm_thread_state64_t st;
		mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
		static const char *states[] = { "?", "running", "stopped", "waiting", "uninterruptible", "halted" };

		memset(&ext, 0, sizeof(ext));
		thread_info(threads[t], THREAD_EXTENDED_INFO, (thread_info_t)&ext, &c);
		printf("  thread %u \"%s\" %s, %u%% cpu, %.3f s user %.3f s system\n", t, ext.pth_name,
		    ext.pth_run_state >= 1 && ext.pth_run_state <= 5 ? states[ext.pth_run_state] : "?",
		    ext.pth_cpu_usage / 10, ext.pth_user_time / 1e9, ext.pth_system_time / 1e9);
		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) != KERN_SUCCESS) {
			printf("    (no registers)\n");
			continue;
		}
		uint64_t pc = arm_thread_state64_get_pc(st), lr = arm_thread_state64_get_lr(st);
		uint64_t fp = arm_thread_state64_get_fp(st);
		say_frame(0, pc);
		say_frame(1, lr);
		for (int n = 2; n < MAX_FRAMES && fp; n++) {
			uint64_t frame[2];
			if (peek(task, fp, frame, sizeof(frame)) || frame[1] == 0) {
				break;
			}
			say_frame(n, frame[1] & 0x0000000fffffffffULL);
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
	}
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	int pids[4096];
	int n = proc_listallpids(pids, sizeof(pids)) ;

	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int a = 1; a < argc; a++) {
		int found = 0;
		for (int i = 0; i < n; i++) {
			char name[256] = "";
			proc_name(pids[i], name, sizeof(name));
			if (strcmp(name, argv[a]) == 0) {
				dump(pids[i], name);
				found = 1;
			}
		}
		if (!found) {
			printf("stacks: no process named %s\n", argv[a]);
		}
	}
	return 0;
}
