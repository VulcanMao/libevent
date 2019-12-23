#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <error.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <event2/event.h>
#include <event2/util.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/event_compat.h>
#include <event2/http_compat.h>
#include <event2/buffer.h>

void http_handler(struct evhttp_request *req,void *arg);

int main(int argc,char **argv){
	char *host_ip=(char*)"0.0.0.0";
	int host_port=9876;
	int timeout=3;

	struct evhttp *httpd;
	event_init();

	//����host_ip��host_port����һ��addrinfo�ṹ��,Ȼ�󴴽�һ��socket,
	//�󶨵����socket��,������Щ��Ϣ�õ�һ��event(�ص���������accept_socket),
	//Ȼ�����event��������Ӧ��event_base,֮����뵽&http->sockets������,Ȼ�󷵻�&http
	httpd = evhttp_start(host_ip,host_port);
	if(httpd==NULL){
		fprintf(stderr,"Error : Unable to listen on %s:%d\n\n",host_ip,host_port);
		exit(1);
	}

	//��������ʱʱ��
	evhttp_set_timeout(httpd,timeout);

	//��������Ĵ�����
	evhttp_set_gencb(httpd,http_handler,NULL);

	event_dispatch();
	evhttp_free(httpd);

	return 0;
}

void http_handler(struct evhttp_request *req,void *arg){
	struct evbuffer *buf;
	buf = evbuffer_new();

	//��������
	char *decode_uri = strdup((char*)evhttp_request_uri(req));
	struct evkeyvalq http_query;
	evhttp_parse_query(decode_uri,&http_query);
	free(decode_uri);

	//��httpͷ��ȡ����
	const char *request_value = evhttp_find_header(&http_query,"data");

	//����httpͷ��
	evhttp_add_header(req->output_headers,"Content-Type","text/html; charset=UTF-8");
	evhttp_add_header(req->output_headers,"Server","my_httpd");
	evhttp_add_header(req->output_headers,"Connection","close");
	
	//��Ҫ�����ֵд���������
	if(request_value!=NULL){
		evbuffer_add_printf(buf,"%s",request_value);
	}
	else{
		evbuffer_add_printf(buf,"%s","no error");
	}

	evhttp_send_reply(req,HTTP_OK,"OK",buf);

	//�ڴ��ͷ�
	evhttp_clear_headers(&http_query);
	evbuffer_free(buf);
}
