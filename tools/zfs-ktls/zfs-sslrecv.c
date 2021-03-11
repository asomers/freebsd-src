#include <sys/types.h>
#include <sys/ktls.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <err.h>
#include <netdb.h>
#include <stdbool.h>
#include <unistd.h>

#include <libzfs.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static void
init_openssl()
{ 
    SSL_load_error_strings();	
    OpenSSL_add_ssl_algorithms();
}

static SSL_CTX
*create_context()
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = SSLv23_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
	perror("Unable to create SSL context");
	ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    return ctx;
}

static void
configure_context(SSL_CTX *ctx)
{
    SSL_CTX_set_ecdh_auto(ctx, 1);
    long flags;

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
	exit(EXIT_FAILURE);
    }

    // TODO: set preferred ciphers, since KERN_TLS supports only a few

    // Use TLS 1.2, because TLS 1.3 does not support ktls for both send and
    // receive simultaneously.
    flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1_3;
    SSL_CTX_set_options(ctx, flags);
}

static int
usage() {
	fprintf(stderr, "usage: zfs-sslrecv [-hs] [-p port] filesystem|volume\n");
	exit(2);
}

static int
opensock(const char *port)
{
	struct addrinfo hints, *res;
	const char *sockhost;
	int fd, nfd, soval;

	sockhost = "127.0.0.1";
	bzero(&hints, sizeof(hints));
	hints.ai_flags = AI_ADDRCONFIG;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(sockhost, port, &hints, &res)) {
		err(1, "getaddrinfo");
	}

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		err(1, "socket");
	}
	soval = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &soval, sizeof(int)) < 0)
	    err(1, "setsockopt(SO_REUSEADDR)");

	if (bind(fd, res->ai_addr, res->ai_addrlen) == -1)
		err(1, "bind()\n");
	if (listen(fd, 1) == -1)
		err(1, "listen()");
	nfd = accept(fd, 0, 0);
	if (nfd == -1)
		err(1, "accept()");

	return (nfd);
}

static int
openssock(const char *port, SSL **ssl)
{
	SSL_CTX *ctx;
	int sock, fd;

	init_openssl();
	ctx = create_context();
	configure_context(ctx);
	sock = opensock(port);
        *ssl = SSL_new(ctx);
        SSL_set_fd(*ssl, sock);
        if (SSL_accept(*ssl) <= 0) {
            ERR_print_errors_fp(stderr);
        }
	fd = SSL_get_fd(*ssl);
	if (BIO_get_ktls_send(SSL_get_wbio(*ssl)))
		printf("ktls enabled for TX\n");
	if (BIO_get_ktls_recv(SSL_get_wbio(*ssl)))
		printf("ktls enabled for RX\n");

	return (fd);
}

int
main(int argc, char**argv) {
	libzfs_handle_t *hdl;
	recvflags_t flags = {0};
	char *filesystem;
	SSL *ssl = NULL;
	nvlist_t *props = NULL;
	int sock;
	int error;
	int ch;
	const char *port = "8080";
	bool use_ssl = false;

	while ((ch = getopt(argc, argv, "sp:")) != -1) {
		switch(ch) {
		case 'p':
			port = optarg;
			break;
		case 's':
			use_ssl = true;
			break;
		case 'h':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage();
	}
	filesystem = argv[0];

	if ((hdl = libzfs_init()) == NULL) {
		err(1, "libzfs_init");
	}

	if (use_ssl) {
		sock = openssock(port, &ssl);
	} else {
		sock = opensock(port);
	}


	error = zfs_receive(hdl, filesystem, props, &flags, sock, NULL);
	if (error != 0) {
		errx(1, "zfs_receive returned %d", error);
	}

	if (use_ssl) {
		SSL_shutdown(ssl);
		SSL_free(ssl);
	}

	close(sock);
		
	return (0);
}
