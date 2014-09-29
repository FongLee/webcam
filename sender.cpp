#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "sender.h"

struct Ctx
{
	int sock;
	sockaddr_in target;
};
typedef struct Ctx Ctx;

void *sender_open (const char *ip, int port)
{
	Ctx *ctx = new Ctx;
	//调用socket函数建立套接字
    //AF_INET 表示IPv4网络协议
    //SOCK_DGRAM：数据报文服务或者数据报文套接字
	ctx->sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (ctx->sock == -1) {
		fprintf(stderr, "%s: create sock err\n", __func__);
		exit(-1);
	}

	ctx->target.sin_family = AF_INET;
	ctx->target.sin_port = htons(port);
	ctx->target.sin_addr.s_addr = inet_addr(ip);

	return ctx;
}

void sender_close (void *snd)
{
	Ctx *c = (Ctx*)snd;
	close(c->sock);
	delete c;
}

int sender_send (void *snd, const void *data, int len)
{
	assert(len < 65536);
	Ctx *c = (Ctx*)snd;

	/*定义函数：int sendto(int s, const void * msg, int len, unsigned int flags, 
	const struct sockaddr * to, int tolen);
	函数说明：sendto() 用来将数据由指定的socket 传给对方主机. 参数s 为已建好连线的socket, 
	如果利用UDP协议则不需经过连线操作. 参数msg 指向欲连线的数据内容, 参数flags 一般设0, 
	详细描述请参考send(). 参数to 用来指定欲传送的网络地址, 结构sockaddr 请参考bind(). 
	参数tolen 为sockaddr 的结果长度.*/
	return sendto(c->sock, data, len, 0, (sockaddr*)&c->target, sizeof(sockaddr_in));
}

