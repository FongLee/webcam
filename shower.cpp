#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "vshow.h"
#include "recver.h"

/** 绑定 127.0.0.1:3020 udp 端口, 接收数据, 一个udp包, 总是完整帧, 交给 libavcodec 解码, 交给
 *  vshow 显示
 */

#define RECV_PORT 3020

int main (int argc, char **argv)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		fprintf(stderr, "ERR: create sock err\n");
		exit(-1);
	}

	sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(RECV_PORT);
	sin.sin_addr.s_addr = inet_addr("192.168.1.116");

	//调用bind函数把套接字绑定到一个监听端口上。注意bind函数需要接受一个sockaddr_in结构体
    //作为参数，因此在调用bind函数之前, 程序要先声明一个 sockaddr_in结构体,用memset函数将
    //其清零，然后将其中的sin_family设置为AF_INET，接下来，程序需要设置其sin_port成员变量，
    //即监听端口。需要说明的是，sin_port中的端口号需要以网络字节序存储，因此需要调用htons
    //函数对端口号进行转换（函数名是"host to network short"的缩写）。
	if (bind(sock, (sockaddr*)&sin, sizeof(sin)) < 0) {
		fprintf(stderr, "ERR: bind %d\n", RECV_PORT);
		exit(-1);
	}
	/*内存分配函数,与malloc,calloc,realloc类似.
	但是注意一个重要的区别,_alloca是在栈(stack)上申请空间,用完马上就释放.
	包含在头文件malloc.h中.
	在某些系统中会宏定义成_alloca使用.*/
	unsigned char *buf = (unsigned char*)alloca(65536);
	if (!buf) {
		fprintf(stderr, "ERR: alloca 65536 err\n");
		exit(-1);
	}

	/**
	 * Register the codec codec and initialize libavcodec.
	 *
	 * @warning either this function or avcodec_register_all() must be called
	 * before any other libavcodec functions.
	 *
	 * @see avcodec_register_all()
	 */
	avcodec_register_all();

	/**
	 * Find a registered decoder with a matching codec ID.
	 *
	 * @param id AVCodecID of the requested decoder
	 * @return A decoder if one was found, NULL otherwise.
	 */
	AVCodec *codec = avcodec_find_decoder(CODEC_ID_H264);

	/**
	 * Allocate an AVCodecContext and set its fields to default values. The
	 * resulting struct should be freed with avcodec_free_context().
	 *
	 * @param codec if non-NULL, allocate private data and initialize defaults
	 *              for the given codec. It is illegal to then call avcodec_open2()
	 *              with a different codec.
	 *              If NULL, then the codec-specific defaults won't be initialized,
	 *              which may result in suboptimal default settings (this is
	 *              important mainly for encoders, e.g. libx264).
	 *
	 * @return An AVCodecContext filled with default values or NULL on failure.
	 * @see avcodec_get_context_defaults
	 */
	AVCodecContext *dec = avcodec_alloc_context3(codec);

	/**
	 * Initialize the AVCodecContext to use the given AVCodec. Prior to using this
	 * function the context has to be allocated with avcodec_alloc_context3().
	 *
	 * The functions avcodec_find_decoder_by_name(), avcodec_find_encoder_by_name(),
	 * avcodec_find_decoder() and avcodec_find_encoder() provide an easy way for
	 * retrieving a codec.
	 *
	 * @warning This function is not thread safe!
	 *
	 * @code
	 * avcodec_register_all();
	 * av_dict_set(&opts, "b", "2.5M", 0);
	 * codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	 * if (!codec)
	 *     exit(1);
	 *
	 * context = avcodec_alloc_context3(codec);
	 *
	 * if (avcodec_open2(context, codec, opts) < 0)
	 *     exit(1);
	 * @endcode
	 *
	 * @param dec The context to initialize.
	 * @param codec The codec to open this context for. If a non-NULL codec has been
	 *              previously passed to avcodec_alloc_context3() or
	 *              avcodec_get_context_defaults3() for this context, then this
	 *              parameter MUST be either NULL or equal to the previously passed
	 *              codec.
	 * @param options:NULL A dictionary filled with AVCodecContext and codec-private 
	 *                	options.On return this object will be filled with options that 
	 *					were not found.
	 * @return zero on success, a negative value on error
	 * @see avcodec_alloc_context3(), avcodec_find_decoder(), avcodec_find_encoder(),
	 *      av_dict_set(), av_opt_find().
	 */
	if (avcodec_open2(dec, codec, NULL) < 0) {
		fprintf(stderr, "ERR: open H264 decoder err\n");
		exit(-1);
	}

	AVFrame *frame = avcodec_alloc_frame();

	void *shower = 0;	// 成功解码第一帧后, 才知道大小

	for (; ; ) {
		sockaddr_in from;
		socklen_t fromlen = sizeof(from);

		/*定义函数：int recvfrom(int s, void *buf, int len, unsigned int flags, 
			struct sockaddr *from,int *fromlen);

		函数说明：recv()用来接收远程主机经指定的socket 传来的数据, 并把数据存到由参数buf 
		指向的内存空间, 参数len 为可接收数据的最大长度. 参数flags 一般设0, 其他数值定义
		请参考recv(). 参数from 用来指定欲传送的网络地址, 结构sockaddr 请参考bind(). 
		参数fromlen 为sockaddr 的结构长度.

		返回值：成功则返回接收到的字符数, 失败则返回-1, 错误原因存于errno 中.*/
		int rc = recvfrom(sock, buf, 65536, 0, (sockaddr*)&from, &fromlen);
		if (rc > 0) {
			// 解压
			int got;
			AVPacket pkt;
			pkt.data = buf;
			pkt.size = rc;
			/**
			 * Decode the video frame of size pkt->size from pkt->data into frame.
			 * Some decoders may support multiple frames in a single AVPacket, such
			 * decoders would then just decode the first frame.
			 *
			 * @warning The input buffer must be FF_INPUT_BUFFER_PADDING_SIZE larger than
			 * the actual read bytes because some optimized bitstream readers read 32 or 64
			 * bits at once and could read over the end.
			 *
			 * @warning The end of the input buffer buf should be set to 0 to ensure that
			 * no overreading happens for damaged MPEG streams.
			 *
			 * @note Codecs which have the CODEC_CAP_DELAY capability set have a delay
			 * between input and output, these need to be fed with avpkt->data=NULL,
			 * avpkt->size=0 at the end to return the remaining frames.
			 *
			 * @param dec the codec context
			 * @param[out] frame The AVFrame in which the decoded video frame will be stored.
			 *             Use av_frame_alloc() to get an AVFrame. The codec will
			 *             allocate memory for the actual bitmap by calling the
			 *             AVCodecContext.get_buffer2() callback.
			 *             When AVCodecContext.refcounted_frames is set to 1, the frame is
			 *             reference counted and the returned reference belongs to the
			 *             caller. The caller must release the frame using av_frame_unref()
			 *             when the frame is no longer needed. The caller may safely write
			 *             to the frame if av_frame_is_writable() returns 1.
			 *             When AVCodecContext.refcounted_frames is set to 0, the returned
			 *             reference belongs to the decoder and is valid only until the
			 *             next call to this function or until closing or flushing the
			 *             decoder. The caller may not write to it.
			 *
			 * @param[in] pkt The input AVPacket containing the input buffer.
			 *            You can create such packet with av_init_packet() and by then setting
			 *            data and size, some decoders might in addition need other fields like
			 *            flags&AV_PKT_FLAG_KEY. All decoders are designed to use the least
			 *            fields possible.
			 * @param[in,out] got Zero if no frame could be decompressed, otherwise, it is nonzero.
			 * @return On error a negative value is returned, otherwise the number of bytes
			 * used or zero if no frame could be decompressed.
			 */
			int ret = avcodec_decode_video2(dec, frame, &got, &pkt);
			if (ret > 0 && got) {
				// 解码成功
				if (!shower) {
					shower = vs_open(dec->width, dec->height);
					if (!shower) {
						fprintf(stderr, "ERR: open shower window err!\n");
						exit(-1);
					}
				}

				// 显示
				vs_show(shower, frame->data, frame->linesize);
			}
		}
	}

	avcodec_close(dec);
	av_free(dec);
	av_free(frame);
	close(sock);
	vs_close(shower);

	return 0;
}

