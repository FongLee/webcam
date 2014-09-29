#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

extern "C" {
#	include <libswscale/swscale.h>
#	include <libavcodec/avcodec.h>
};

struct Ctx
{
	Display *display;
	int screen;
	Window window;
	GC gc;
	XVisualInfo vinfo;
	XImage *image;
	XShmSegmentInfo segment;

	SwsContext *sws;
	PixelFormat target_pixfmt;
	AVPicture pic_target;

	int v_width, v_height;
	int curr_width, curr_height;
};
typedef struct Ctx Ctx;

void *vs_open (int v_width, int v_height)
{
	Ctx *ctx = new Ctx;
	ctx->v_width = v_width;
	ctx->v_height = v_height;

	// window
	/*在程式开始向 X Server 进行任何的动作之前，程式必需先和 X Server 之间建立一个连线
	(connection)，我们称之为 display。 XOpenDisplay 即为 Xlib 提供给我建立 display 的函数。 

        Display *XOpenDisplay(display_name)
                char *display_name;
        display_name        指定要连接之 server。如果 display_name 设定为
                        NULL，则内定使用环境变数(environment variable)
                        DISPLAY 的内容为连接对像。

	呼叫 XOpenDisplay 之後，会传回一个 Display 结构。 Display 结构 存放着一些关於
	 display 的资讯。*/
	ctx->display = XOpenDisplay(0);
	if (!ctx->display) {
		fprintf(stderr, "%s: XOpenDisplay err\n", __func__);
		exit(-1);
	}
	/*我现在开始建立新视窗(window)。视窗(window)建立之後，并不会马上 在我们指定的显示器
	(screen)上显示出来。我们要经过一道 map 的手 序後，视窗(window)才会正式在显示器上
	显示出来。在我们建立视窗( window)之後，在 map 之前，我们可以对新视窗(window)
	做一些设定的 动作，以设定视窗(window)的行为特性。建立新视窗(window)要透过 Xlib 
	所提供的 XCreateWindow 函数或者 XCreateSimpleWindow 函数，XCreateSimpleWindow 
	是 XCreateWindow 的简化版。这两个函数可用来建立新的子视窗。*/
	ctx->window = XCreateSimpleWindow(ctx->display, RootWindow(ctx->display, 0),
			100, 100, v_width, v_height, 0, BlackPixel(ctx->display, 0),
			WhitePixel(ctx->display, 0));
	ctx->screen = 0;
	/*如我们已经提到的，一个图形上下文定义一些参数来使用绘图函数。因此，为了绘制不同的风格，
	我们可以在一个窗口里使用多个图形上下文。使用函数XCreateGC()可以申请到一个新的图形
	上下文，如以下例（在这段代码里，我们假设"display"指向一个显示结构，"win"是当前创建的
	一个窗口的ID）*/
	ctx->gc = XCreateGC(ctx->display, ctx->window, 0, 0);
	/*事实上我们创建窗口并不意味着它将会被立刻显示在屏幕上，在缺省情况下，新建的窗口将
	不会被映射到屏幕上-它们是不可见的。为了能让我们创建的窗口能被显示到屏幕上，我们使用
	函数XMapWindow()*/
	XMapWindow(ctx->display, ctx->window);

	// current screen pix fmt
	Window root;
	unsigned int cx, cy, border, depth;
	int x, y;
	XGetGeometry(ctx->display, ctx->window, &root, &x, &y, &cx, &cy, &border, &depth);

	// visual info
	XMatchVisualInfo(ctx->display, ctx->screen, depth, DirectColor, &ctx->vinfo);

	// image
	ctx->image = XShmCreateImage(ctx->display, ctx->vinfo.visual, depth, ZPixmap, 0,
			&ctx->segment, cx, cy);
	if (!ctx->image) {
		fprintf(stderr, "%s: can't XShmCreateImage !\n", __func__);
		exit(-1);
	}
	ctx->segment.shmid = shmget(IPC_PRIVATE,
			ctx->image->bytes_per_line * ctx->image->height, 
			IPC_CREAT | 0777);
	if (ctx->segment.shmid < 0) {
		fprintf(stderr, "%s: shmget err\n", __func__);
		exit(-1);
	}

	ctx->segment.shmaddr = (char*)shmat(ctx->segment.shmid, 0, 0);
	if (ctx->segment.shmaddr == (char*)-1) {
		fprintf(stderr, "%s: shmat err\n", __func__);
		exit(-1);
	}

	ctx->image->data = ctx->segment.shmaddr;
	ctx->segment.readOnly = 0;
	XShmAttach(ctx->display, &ctx->segment);

	PixelFormat target_pix_fmt = PIX_FMT_NONE;
	switch (ctx->image->bits_per_pixel) {
		case 32:
			target_pix_fmt = PIX_FMT_RGB32;
			break;
		case 24:
			target_pix_fmt = PIX_FMT_RGB24;
			break;
		default:
			break;
	}

	if (target_pix_fmt == PIX_FMT_NONE) {
		fprintf(stderr, "%s: screen depth format err\n", __func__);
		delete ctx;
		return 0;
	}

	// sws
	ctx->target_pixfmt = target_pix_fmt;
	ctx->curr_width = cx;
	ctx->curr_height = cy;
	ctx->sws = sws_getContext(v_width, v_height, PIX_FMT_YUV420P,
			cx, cy, target_pix_fmt,
			SWS_FAST_BILINEAR, 0, 0, 0);
	/**
	 * Allocate memory for the pixels of a picture and setup the AVPicture
	 * fields for it.
	 *
	 * Call avpicture_free() to free it.
	 *
	 * @param ctx->pic_target           the picture structure to be filled in
	 * @param target_pix_fmt            the pixel format of the picture
	 * @param cx             the width of the picture
	 * @param cy             the height of the picture
	 * @return zero if successful, a negative error code otherwise
	 *
	 * @see av_image_alloc(), avpicture_fill()
	 */
	avpicture_alloc(&ctx->pic_target, target_pix_fmt, cx, cy);

	XFlush(ctx->display);

	return ctx;
}

int vs_close (void *ctx)
{
	return 1;
}

inline int MIN(int a, int b)
{
	return a < b ? a : b;
}

int vs_show (void *ctx, unsigned char *data[4], int stride[4])
{
	// 首选检查 sws 是否有效, 根据当前窗口大小决定
	Ctx *c = (Ctx*)ctx;
	Window root;
	int x, y;
	unsigned int cx, cy, border, depth;
	XGetGeometry(c->display, c->window, &root, &x, &y, &cx, &cy, &border, &depth);
	if (cx != c->curr_width || cy != c->curr_height) {
		avpicture_free(&c->pic_target);
		sws_freeContext(c->sws);

		c->sws = sws_getContext(c->v_width, c->v_height, PIX_FMT_YUV420P,
				cx, cy, c->target_pixfmt, 
				SWS_FAST_BILINEAR, 0, 0, 0);
		avpicture_alloc(&c->pic_target, c->target_pixfmt, cx, cy);

		c->curr_width = cx;
		c->curr_height = cy;

		// re create image
		XShmDetach(c->display, &c->segment);
		shmdt(c->segment.shmaddr);
		shmctl(c->segment.shmid, IPC_RMID, 0);
		XDestroyImage(c->image);

		c->image = XShmCreateImage(c->display, c->vinfo.visual, depth, ZPixmap, 0,
			&c->segment, cx, cy);

		c->segment.shmid = shmget(IPC_PRIVATE,
				c->image->bytes_per_line * c->image->height,
				IPC_CREAT | 0777);
		c->segment.shmaddr = (char*)shmat(c->segment.shmid, 0, 0);
		c->image->data = c->segment.shmaddr;
		c->segment.readOnly = 0;
		XShmAttach(c->display, &c->segment);
	}

	//
	/**
	 * Scale the image slice in srcSlice and put the resulting scaled
	 * slice in the image in dst. A slice is a sequence of consecutive
	 * rows in an image.
	 *
	 * Slices have to be provided in sequential order, either in
	 * top-bottom or bottom-top order. If slices are provided in
	 * non-sequential order the behavior of the function is undefined.
	 *
	 * @param c         the scaling context previously created with
	 *                  sws_getContext()
	 * @param srcSlice  the array containing the pointers to the planes of
	 *                  the source slice
	 * @param srcStride the array containing the strides for each plane of
	 *                  the source image
	 * @param srcSliceY the position in the source image of the slice to
	 *                  process, that is the number (counted starting from
	 *                  zero) in the image of the first row of the slice
	 * @param srcSliceH the height of the source slice, that is the number
	 *                  of rows in the slice
	 * @param dst       the array containing the pointers to the planes of
	 *                  the destination image
	 * @param dstStride the array containing the strides for each plane of
	 *                  the destination image
	 * @return          the height of the output slice
	 */ 
	sws_scale(c->sws, data, stride, 0, c->v_height, c->pic_target.data, c->pic_target.linesize);

	// cp to image
	unsigned char *p = c->pic_target.data[0], *q = (unsigned char*)c->image->data;
	int xx = MIN(c->image->bytes_per_line, c->pic_target.linesize[0]);
	for (int i = 0; i < c->curr_height; i++) {
		memcpy(q, p, xx);
		p += c->image->bytes_per_line;
		q += c->pic_target.linesize[0];
	}

	// 显示到 X 上
	XShmPutImage(c->display, c->window, c->gc, c->image, 0, 0, 0, 0, c->curr_width, c->curr_height, 1);

	return 1;
}

