// Brian Chrzanowski
// 2021-11-20 14:36:29
//
// Christmas 2021 Project
//
// This is the IOT Server, for the Christmas Plugs, but also for whatever other IOT devices we
// decide to have make and setup.
//
// TODO (Brian):
//
// 1. Hook up the webserver functionality for this.
// 2. All IO needs to be ASYNC (I'd like one process / thread for this app)
//    I think we should / can achieve this with io_uring, so basically we'll
//    just have a loop spinning and waiting for data to get through.

#define COMMON_IMPLEMENTATION
#include "common.h"

#include "ht.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h> 
#include <unistd.h>

#include <magic.h>

#include <pthread.h>

#define HTTPSERVER_IMPL
#include "httpserver.h"

// NOTE (Brian): these constants are basically magic constants
// 'MAGIC' ensures that the micro controller got the message without much bit mangling
//
// 'VERSION' allows IOT devices the ability to verify that the 'NetworkMessage' structure
// is what they expect.
#define MAGIC   (0x08675309)
#define VERSION (1)

// NetworkMessage (v1) : a structure that gets directly sent to the consuming device
struct NetworkMessage {
	uint32_t magic;
	uint32_t version;
	uint32_t time_hi;
	uint32_t time_lo;
	uint32_t command;
	uint32_t args[4];
};

// StoreHeader : the storage header
struct StoreHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t devices;
};

#define DEVICEFLAG_INUSE (0x01)

// NetworkDevice : this is what we store on the server, not very complicated
struct NetworkDevice {
	uint32_t id;
	uint32_t flags;
	uint32_t device1;
	uint32_t device2;
	uint32_t device3;
	uint32_t device4;
	char name[BUFSMALL];
};

// NOTE (Brian): we probably want to change and come up with a port mangling system for the server.

#define HTTPSERVER_PORT (5002)
#define CTRLSERVER_PORT (3000)

// NOTE	(Brian): these must also match on the micro-controller

#define COMMAND_NONE    (0x00)
#define COMMAND_DISABLE (0x01)
#define COMMAND_ENABLE  (0x02)

// COMMANDS:
//
// NONE       - does nothing - used for updating the time on the client
//
// DISABLE    - toggle the device(s) denoted in args 1-4 off
// ENABLE     - toggle the device(s) denoted in args 1-4 on

int listenfd;

// NetworkDevices : a runtime structure to maintain the list of devices
struct NetworkDevices {
	struct NetworkDevice *devices;
	size_t devices_len, devices_cap;
};

static struct NetworkDeviceList *devices;

// libmagic stuff
static magic_t MAGIC_COOKIE;

// sighandler : closes the listening socket
void sighandler(int signal)
{
	close(listenfd);
	exit(1);
}

// HTTPServerSetup : this sets up the HTTP Server
void *HTTPServerSetup(void *arg);
// HTTPServerCallback : the HTTP Server Callback
void HTTPServerCallback(struct http_request_s *req);

// HTTPSendError : sends an HTTP error code to the user
int HTTPSendError(struct http_request_s *req, struct http_response_s *res, int errcode);

// CTRLServerSetup : this sets up the micro-controller server
void *CTRLServerSetup(void *arg);

// DumpNetworkMessage : prints out the contents of the NetworkMessage to Serial
void DumpNetworkMessage(struct NetworkMessage *message);

struct ht *routes;

int main(int argc, char **argv)
{
	int rc;
	pthread_t http_thread, ctrl_thread;
	pthread_attr_t attr;
	ssize_t stack_size;
	void *res;

	signal(SIGINT, sighandler);

	// setup libmagic
	MAGIC_COOKIE = magic_open(MAGIC_MIME);
	if (MAGIC_COOKIE == NULL) {
		fprintf(stderr, "%s", magic_error(MAGIC_COOKIE));
		exit(1);
	}

	stack_size = 0x800000;

	rc = pthread_attr_init(&attr);

	rc = pthread_attr_setstacksize(&attr, stack_size);

	rc = pthread_create(&http_thread, &attr, &HTTPServerSetup, NULL);

	rc = pthread_create(&ctrl_thread, &attr, &CTRLServerSetup, NULL);

	pthread_join(http_thread, &res);
	pthread_join(ctrl_thread, &res);

	return 0;
}

// DeviceAPIGetList : returns a list of devices
int DeviceAPIGetList(struct http_request_s *req, struct http_response_s *res);
// DeviceAPIPut : allows a user to update the state of a single device
int DeviceAPIPut(struct http_request_s *req, struct http_response_s *res);
// DeviceAPIDelete : allows a user to delete a device from the list
int DeviceAPIDelete(struct http_request_s *req, struct http_response_s *res);

// HTTPSendFilePath : sends a file to the user at path 'path'
int HTTPSendFilePath(struct http_request_s *req, struct http_response_s *res, char *path);

// HTTPSendFileStatic : sends the static data JSON blob
int HTTPSendFileStatic(struct http_request_s *req, struct http_response_s *res)
{
	return HTTPSendFilePath(req, res, "src/static.json");
}

// HTTPSendFileUiJs : sends the javascript for the ui to the user
int HTTPSendFileUiJs(struct http_request_s *req, struct http_response_s *res)
{
	return HTTPSendFilePath(req, res, "src/ui.js");
}

// HTTPSendFileStyles : sends the styles file to the user
int HTTPSendFileStyles(struct http_request_s *req, struct http_response_s *res)
{
	char *file_data;
	char *path;
	size_t len;

    // NOTE (Brian) there's a small issue with libmagic setting the mime type for css correctly, so
    // I'm basically doing this myself. Without this, the server just sends "text/plain" as the
    // type, and the browser doesn't like that.

	path = "src/styles.css";

	if (access(path, F_OK) == 0) {
		file_data = sys_readfile(path, &len);

		http_response_status(res, 200);
		http_response_header(res, "Content-Type", "text/css");
		http_response_body(res, file_data, len);

		http_respond(req, res);

		free(file_data);
	} else {
		HTTPSendError(req, res, 404);
	}

	return 0;
}

// HTTPSendFileIndex : sends the index file to the user
int HTTPSendFileIndex(struct http_request_s *req, struct http_response_s *res)
{
	return HTTPSendFilePath(req, res, "src/index.html");
}

// HTTPSendFilePath : sends a file to the user at path 'path'
int HTTPSendFilePath(struct http_request_s *req, struct http_response_s *res, char *path)
{
	char *file_data;
	char *mime_type;
	size_t len;

	// NOTE (Brian): To make this routing fit well in a hashtable (hopefully, to make routing
	// easier), this function distributes the two static files that we give a shit about.

	if (access(path, F_OK) == 0) {
		file_data = sys_readfile(path, &len);

		mime_type = (char *)magic_buffer(MAGIC_COOKIE, file_data, len);

		http_response_status(res, 200);
		http_response_header(res, "Content-Type", mime_type);
		http_response_body(res, file_data, len);

		http_respond(req, res);

		free(file_data);
	} else {
		HTTPSendError(req, res, 404);
	}

	return 0;
}

// HTTPServerSetup : this sets up the HTTP Server
void *HTTPServerSetup(void *arg)
{
	struct http_server_s *server;

	// setup the routing hashtable
	routes = ht_create();

	// NOTE (Brian): I don't really know if we want / need more interfaces here.  I have a feeling
	// we'll find out for certain when Emma goes to use the thing and she says, "babe, I really want
	// it to be like this".

	ht_set(routes, "GET /api/v1/device/list", (void *)DeviceAPIGetList);
	// ht_set(routes, "GET /api/v1/device/:id",  (void *)device_api_get);
	ht_set(routes, "PUT /api/v1/device/:id",  (void *)DeviceAPIPut);
	ht_set(routes, "DELETE /api/v1/device/:id",  (void *)DeviceAPIDelete);

	ht_set(routes, "GET /ui.js", (void *)HTTPSendFileUiJs);
	ht_set(routes, "GET /styles.css", (void *)HTTPSendFileStyles);
	ht_set(routes, "GET /index.html", (void *)HTTPSendFileIndex);
	ht_set(routes, "GET /", (void *)HTTPSendFileIndex);

	server = http_server_init(HTTPSERVER_PORT, HTTPServerCallback);

	http_server_listen(server);

	return NULL;
}

// format_target_string : format the incomming requets for the routing hashtable
int format_target_string(char *s, struct http_request_s *req, size_t len)
{
	size_t buflen;
	struct http_string_s target, method;
	char *tok;
	char *hasq;
	char copy[BUFLARGE];
	char readfrom[BUFLARGE];

	// TODO (Brian):
	//
	// - make sure we uppercase (normalize, really) all of the data from the user. HTTP blows.
	// - ensure 'len' isn't larger than BUFLARGE

	memset(copy, 0, sizeof copy);
	memset(readfrom, 0, sizeof readfrom);

	assert(s != NULL);

	method = http_request_method(req);
	target = http_request_target(req);

	strncpy(readfrom, target.buf, MIN(sizeof(readfrom), target.len));

	buflen = 0;

	for (tok = strtok(readfrom, "/"); tok != NULL && buflen <= len; tok = strtok(NULL, "/")) {
		if (isdigit(*tok)) {
			buflen += snprintf(copy + buflen, sizeof(copy) - buflen, "/:id");
		} else {
			hasq = strchr(tok, '?');
			if (hasq) {
				buflen += snprintf(copy + buflen, sizeof(copy) - buflen, "/%.*s", (int)(hasq - tok), tok);
			} else {
				buflen += snprintf(copy + buflen, sizeof(copy) - buflen, "/%s", tok);
			}
		}
	}

	if (copy[0] == '\0') {
		copy[0] = '/';
	}

	snprintf(s, len, "%.*s %s", method.len, method.buf, copy);

	return 0;
}

// HTTPServerCallback : the HTTP Server Callback
void HTTPServerCallback(struct http_request_s *req)
{
	struct http_response_s *res;
	int rc;
	int (*func)(struct http_request_s *, struct http_response_s *);
	char buf[BUFLARGE];

#define SNDERR(E) HTTPSendError(req, res, (E))
#define CHKERR(E) do { if ((rc) < 0) { HTTPSendError(req, res, (E)); } } while (0)

	memset(buf, 0, sizeof buf);

	rc = format_target_string(buf, req, sizeof buf);

	printf("%s\n", buf);

	res = http_response_init();

	func = ht_get(routes, buf);

	if (func == NULL) {
        SNDERR(404);
	} else {
		rc = func(req, res);
		CHKERR(503);
	}
}

// CTRLServerSetup : this sets up the micro-controller server
void *CTRLServerSetup(void *arg)
{
	int connfd;
	struct sockaddr_in serv_addr;
	struct NetworkMessage message;
	time_t currtime;
	int rc;

	uint32_t command;
	uint32_t args[4];

	assert(sizeof(time_t) == 8);

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd == -1) {
		perror("socket");
		exit(1);
		return NULL;
	}

	rc = 1;
	rc = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &rc, sizeof rc);
	if (rc == -1) {
		perror("setsockopt");
		exit(1);
		return NULL;
	}

	memset(&serv_addr, 0, sizeof serv_addr);
	memset(&message, 0, sizeof message);

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(CTRLSERVER_PORT);

	bind(listenfd, (struct sockaddr *)&serv_addr, sizeof serv_addr);

	rc = listen(listenfd, 10);
	if (rc == -1) {
		perror("listen");
		exit(1);
		return NULL;
	}

	printf("listening on localhost:%d\n", CTRLSERVER_PORT);

	memset(&args, 0, sizeof args);

	command = COMMAND_DISABLE;

	args[0] = 1;
	args[1] = 2;
	args[2] = 3;
	args[3] = 0;

	for (;;) {
		struct sockaddr addr;
		socklen_t addrlen;

		memset(&addr, 0, sizeof addr);
		memset(&addrlen, 0, sizeof addrlen);

		connfd = accept(listenfd, (struct sockaddr *)&addr, &addrlen);
		if (connfd == -1) {
			perror("accept");
			return NULL;
		}

		currtime = time(NULL);
		printf("Curr Time: 0x%lX\n", currtime);

		// pack up the message
		memset(&message, 0, sizeof message);

		message.magic = MAGIC;
		message.version = VERSION;

		message.time_hi = (uint32_t)(((uint64_t)currtime) >> 0x20);
		message.time_lo = (uint32_t)(0xffffffff & currtime);

		// this is just a small test
		if (command == COMMAND_DISABLE) {
			command = COMMAND_ENABLE;
		} else {
			command = COMMAND_DISABLE;
		}

		message.command = command;

		memcpy(&message.args, &args, sizeof message.args);

		printf("Sending this NetworkMessage!\n");
		DumpNetworkMessage(&message);

		rc = write(connfd, &message, sizeof message);
		if (rc == -1) {
			printf("an error happened! %s\n", strerror(errno));
		}

		close(connfd);
	}

	return NULL;
}

// DeviceAPIGetList : returns a list of devices
int DeviceAPIGetList(struct http_request_s *req, struct http_response_s *res)
{
	return 0;
}

// DeviceAPIPut : allows a user to update the state of a single device
int DeviceAPIPut(struct http_request_s *req, struct http_response_s *res)
{
	return 0;
}

// DeviceAPIDelete : allows a user to delete a device from the list
int DeviceAPIDelete(struct http_request_s *req, struct http_response_s *res)
{
	return 0;
}

// HTTPSendError : sends an HTTP error code to the user
int HTTPSendError(struct http_request_s *req, struct http_response_s *res, int errcode)
{
	http_response_status(res, errcode);
	http_respond(req, res);

	return 0;
}

// DumpNetworkMessage : prints out the contents of the NetworkMessage to Serial
void DumpNetworkMessage(struct NetworkMessage *message)
{
	int i;

	printf("Network Message:\n");
	printf("\tmagic:   0x%X\n", message->magic);
	printf("\tversion:  0x%X\n", message->version);
	printf("\ttime_hi:  0x%X\n", message->time_hi);
	printf("\ttime_lo:  0x%X\n", message->time_lo);
	printf("\tcommand:  0x%X\n", message->command);

	for (i = 0; i < (sizeof(message->args) / sizeof(message->args[0])); i++) {
		printf("\targs[%1d]: 0x%X\n", i, message->args[i]);
	}

	printf("\n");
}

