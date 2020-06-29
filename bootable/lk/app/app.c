/*
 * Copyright (c) 2009 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <debug.h>
#include <app.h>
#include <kernel/thread.h>

/* 
__apps_start,__apps_end 都在 system-onesegment.ld 文件中定义 
再见 lk/include/app.h 里面的定义 
	#define APP_START(appname) struct app_descriptor _app_##appname __SECTION(".apps") = { .name = #appname,
	#define APP_END };
	
添加启动 的宏使用在 lk/app 目录下的各功能块里面，比如: lk/app/aboot/aboot.c 定义 
	APP_START(aboot)
		.init = aboot_init,
	APP_END
*/
extern const struct app_descriptor __apps_start;
extern const struct app_descriptor __apps_end;

static void start_app(const struct app_descriptor *app);

/* one time setup */
void apps_init(void)
{
	const struct app_descriptor *app;

	/* call all the init routines */
	for (app = &__apps_start; app != &__apps_end; app++) {
		if (app->init)
			/* 
			当前代码下依次调用 app_descriptor->init ：
			lk/app/aboot/aboot.c
			lk/app/shell/shell.c
			lk/app/tests/tests.c
			*/
			app->init(app);
	}

	/* start any that want to start on boot */
	/* 单独启动一个线程去启动一连串的功能 lk/app */
	for (app = &__apps_start; app != &__apps_end; app++) {
		if (app->entry && (app->flags & APP_FLAG_DONT_START_ON_BOOT) == 0) {
			/* 
			当前代码下依次调用 app_descriptor->entry ：
			lk/app/shell/shell.c
			*/
			start_app(app);
		}
	}
}

static int app_thread_entry(void *arg)
{
	const struct app_descriptor *app = (const struct app_descriptor *)arg;

	app->entry(app, NULL);

	return 0;
}

static void start_app(const struct app_descriptor *app)
{
	thread_t *thr;
	printf("starting app %s\n", app->name);

	thr = thread_create(app->name, &app_thread_entry, (void *)app, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
	if(!thr)
	{
		return;
	}
	thread_resume(thr);
}

