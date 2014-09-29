#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev2.h>
extern "C" {
#	include <libavcodec/avcodec.h>
#	include <libswscale/swscale.h>
}
#include "capture.h"

struct Buffer
{
	void *start;
	size_t length;
};
typedef struct Buffer Buffer;

struct Ctx
{
	int vid;
	int width, height;	// 输出图像大小
	struct SwsContext *sws;	// 用于转换
	int rows;	// 用于 sws_scale()
	int bytesperrow; // 用于cp到 pic_src
	AVPicture pic_src, pic_target;	// 用于 sws_scale
	Buffer bufs[2];		// 用于 mmap
        PixelFormat fmt;
};
typedef struct Ctx Ctx;

int capture_get_output_ptr (void *c, unsigned char***ptr, int **ls)
{
    Ctx *ctx = (Ctx*)c;
    *ptr = ctx->pic_target.data;
    *ls = ctx->pic_target.linesize;
    return 1;
}

void *capture_open (const char *dev_name, int t_width, int t_height, PixelFormat tarfmt)
{
	int id = open(dev_name, O_RDWR);
	if (id < 0) return 0;

	Ctx *ctx = new Ctx;
	ctx->vid = id;

	// to query caps 这个设备的功能，比如是否是视频输入设备
	v4l2_capability caps;
	//查询驱动功能 
	ioctl(id, VIDIOC_QUERYCAP, &caps);

	// 是否支持图像获取
	if (caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
		if (caps.capabilities & V4L2_CAP_READWRITE) {
			// TODO: ...
		}
		////judge whether or not to supply the form of video stream
		if (caps.capabilities & V4L2_CAP_STREAMING) {
			// 检查是否支持 MMAP, 还是 USERPTR
			///申请一个拥有四个缓冲帧的缓冲区
			v4l2_requestbuffers bufs;
			memset(&bufs, 0, sizeof(bufs));
			bufs.count = 2;
			bufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			bufs.memory = V4L2_MEMORY_MMAP;
			if (ioctl(id, VIDIOC_REQBUFS, &bufs) < 0) {
				fprintf(stderr, "%s: don't support MEMORY_MMAP mode!\n", __func__);
				close(id);
				delete ctx;
				return 0;
			}

			fprintf(stderr, "%s: using MEMORY_MMAP mode, buf cnt=%d\n", __func__, bufs.count);

			// mmap, 映射
			for (int i = 0; i < 2; i++) {
				v4l2_buffer buf;
				memset(&buf, 0, sizeof(buf));
				buf.type = bufs.type;
				buf.memory = bufs.memory;
				// 查询序号为n_buffers 的缓冲区，得到其起始物理地址和大小
				if (ioctl(id, VIDIOC_QUERYBUF, &buf) < 0) {
					fprintf(stderr, "%s: VIDIOC_QUERYBUF ERR\n", __func__);
					close(id);
					delete ctx;
					return 0;
				}

				ctx->bufs[i].length = buf.length;
				// 映射内存
				ctx->bufs[i].start = mmap(0, buf.length, PROT_READ|PROT_WRITE,
						MAP_SHARED, id, buf.m.offset);
			}
		}
		else {
			fprintf(stderr, "%s: can't support read()/write() mode and streaming mode\n", __func__);
			close(id);
			delete ctx;
			return 0;
		}
	}
	else {
		fprintf(stderr, "%s: can't support video capture!\n", __func__);
		close(id);
		delete ctx;
		return 0;
	}

	int rc;

	// enum all support image fmt
	v4l2_fmtdesc fmt_desc;
	uint32_t index = 0;

        // 看起来, 不支持 plane fmt, 直接使用 RGB 吧, 然后使用 libswscale 转换
#if 1
	do {
		fmt_desc.index = index;
		fmt_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		// 显示设备所有支持的格式
		rc = ioctl(id, VIDIOC_ENUM_FMT, &fmt_desc);

		if (rc >= 0) {
			fprintf(stderr, "\t support %s\n", fmt_desc.description);
		}
		index++;
	} while (rc >= 0);
#endif // 0
	v4l2_format fmt;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	// 查看当前帧格式
	rc = ioctl(id, VIDIOC_G_FMT, &fmt);
	if (rc < 0) {
		fprintf(stderr, "%s: can't VIDIOC_G_FMT...\n", __func__);
		return 0;
	}

//	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
//	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
//	rc = ioctl(id, VIDIOC_S_FMT, &fmt);
//	if (rc < 0) {
//		fprintf(stderr, "%s: can't support V4L2_PIX_FMT_MJPG\n", __func__);
//		return 0;
//	}
//	if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
//		fprintf(stderr, "%s: can't support V4L2_PIX_FMT_MJPG\n", __func__);
//		return 0;
//	}

	PixelFormat pixfmt = PIX_FMT_NONE;
	switch (fmt.fmt.pix.pixelformat) {
	case V4L2_PIX_FMT_YUYV:
		pixfmt = PIX_FMT_YUYV422;
		break;

	case V4L2_PIX_FMT_MJPEG:
		// pixfmt = PIX_FMT_YUVJ422P;
		// 使用 mjpeg 应该能够满足 640x480x25, 但是需要解码 mjpeg
		
		break;
	}

	if (pixfmt == PIX_FMT_NONE) {
		fprintf(stderr, "%s: can't support %4s\n", __func__, (char*)&fmt.fmt.pix.pixelformat);
		return 0;
	}

	// 构造转换器
	fprintf(stderr, "capture_width=%d, height=%d, stride=%d\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
			fmt.fmt.pix.bytesperline);
	ctx->width = t_width;
	ctx->height = t_height;
	/**
	 * Allocate and return an SwsContext. You need it to perform
	 * scaling/conversion operations using sws_scale().
	 *
	 * @param srcW the width of the source image
	 * @param srcH the height of the source image
	 * @param srcFormat the source image format
	 * @param dstW the width of the destination image
	 * @param dstH the height of the destination image
	 * @param dstFormat the destination image format
	 * @param flags specify which algorithm and options to use for rescaling
	 * @return a pointer to an allocated context, or NULL in case of error
	 * @note this function is to be removed after a saner alternative is
	 *       written
	 */
	ctx->sws = sws_getContext(fmt.fmt.pix.width, fmt.fmt.pix.height, pixfmt,
                        ctx->width, ctx->height, tarfmt, 	// PIX_FMT_YUV420P 对应 X264_CSP_I420
			SWS_FAST_BILINEAR, 0, 0, 0);

	ctx->rows = fmt.fmt.pix.height;
	ctx->bytesperrow = fmt.fmt.pix.bytesperline;

	/**
	 * Allocate memory for the pixels of a picture and setup the AVPicture
	 * fields for it.
	 *
	 * Call avpicture_free() to free it.
	 *
	 * @param picture            the picture structure to be filled in
	 * @param pix_fmt            the pixel format of the picture
	 * @param width              the width of the picture
	 * @param height             the height of the picture
	 * @return zero if successful, a negative error code otherwise
	 *
	 * @see av_image_alloc(), avpicture_fill()
	 */
        avpicture_alloc(&ctx->pic_target, tarfmt, ctx->width, ctx->height);

	// queue buf
	for (int i = 0; i < sizeof(ctx->bufs)/sizeof(Buffer); i++) {
		v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		// 把帧放入队列
		if (ioctl(id, VIDIOC_QBUF, &buf) < 0) {
			fprintf(stderr, "%s: VIDIOC_QBUF err\n", __func__);
			exit(-1);
		}
	}

	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	// 启动数据流
	if (ioctl(id, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "%s: VIDIOC_STREAMON err\n", __func__);
		exit(-1);
	}

        ctx->fmt = tarfmt;

	return ctx;
}

static void _save_pic (void *start, int len)
{
	FILE *fp = fopen("data.d", "wb");
	fwrite(start, 1, len, fp);
	fclose(fp);
}

int capture_get_picture (void *id, Picture *pic)
{
	// 获取, 转换
	Ctx *ctx = (Ctx*)id;
	v4l2_buffer buf;
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	// 从队列中取出帧
	if (ioctl(ctx->vid, VIDIOC_DQBUF, &buf) < 0) {
		fprintf(stderr, "%s: VIDIOC_DQBUF err\n", __func__);
		return -1;
	}

//	_save_pic(ctx->bufs[buf.index].start, buf.length);
//	__asm("int $3");
	ctx->pic_src.data[0] = (unsigned char*)ctx->bufs[buf.index].start;
	ctx->pic_src.data[1] = ctx->pic_src.data[2] = ctx->pic_src.data[3] = 0;
	ctx->pic_src.linesize[0] = ctx->bytesperrow;
	ctx->pic_src.linesize[1] = ctx->pic_src.linesize[2] = ctx->pic_src.linesize[3] = 0;

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
	// sws_scale
	int rs = sws_scale(ctx->sws, ctx->pic_src.data, ctx->pic_src.linesize,
			0, ctx->rows, ctx->pic_target.data, ctx->pic_target.linesize);

	// out
	for (int i = 0; i < 4; i++) {
		pic->data[i] = ctx->pic_target.data[i];
		pic->stride[i] = ctx->pic_target.linesize[i];
	}

	// re queue buf 把帧放入队列
	if (ioctl(ctx->vid, VIDIOC_QBUF, &buf) < 0) {
		fprintf(stderr, "%s: VIDIOC_QBUF err\n", __func__);
		return -1;
	}

	return 1;
}

int capture_close (void *id)
{
	Ctx *ctx = (Ctx*)id;
	for (int i = 0; i < sizeof(ctx->bufs)/sizeof(Buffer); i++) {
		munmap(ctx->bufs[i].start, ctx->bufs[i].length);
	}
	avpicture_free(&ctx->pic_target);
	sws_freeContext(ctx->sws);
	delete ctx;

	return 1;
}

