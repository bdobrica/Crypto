#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>

#include <pthread.h>
#include <sqlite3.h>

#define CRYPTO_SOCK_MAX_LISTENERS	5
#define CRYPTO_SOCK_BUFFER		128

#define CRYPTO_QUERY_BUFFER		128

#define CRYPTO_OTP_LIFETIME		120

#define CRYPTO_SMS_STATUS_HOME		-1
#define CRYPTO_SMS_STATUS_SENT		0
#define CRYPTO_SMS_STATUS_PENDING	1
#define CRYPTO_SMS_STATUS_ERROR		2

#define CRYPTO_DAEMON_NAME		"cryptoid"
#define CRYPTO_DAEMON_PID		"/var/run/cryptoid.pid"
#define CRYPTO_DAEMON_UID		1001
#define CRYPTO_DAEMON_GID		1001
#define CRYPTO_DAEMON_ROOT		"/"

struct smsnode {
	char * number;
	char * key;
	int expire;
	unsigned short int status;

	struct smsnode * next;
	struct smsnode * prev;
	};

struct thrdarg {
	int s;
	sqlite3 * db;
	struct smsnode * sq;
	};

/** sockets **/

int mksock (const char * file) {
	int s;
	struct sockaddr_un sun;

	if ((s = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror ("server socket : failed!");
		exit (1);
		}

	printf ("socket: %s\n", file);
	
	sun.sun_family = AF_UNIX;
	memcpy (sun.sun_path, file, strlen(file)+1);
	unlink (file);
	if (bind (s, (struct sockaddr *) &sun, sizeof(sun.sun_family)+strlen(sun.sun_path)) < 0) {
		perror ("socket bind : failed!");
		exit (1);
		}

	if (listen (s, CRYPTO_SOCK_MAX_LISTENERS) < 0) {
		perror ("socket listen : failed!");
		exit (1);
		}

	return s;
	}

int rdsock (int s, char ** o) {
	int i = 0, f = 1;
	int len, l, r;
	char b[CRYPTO_SOCK_BUFFER];

	len = recv (s, b, CRYPTO_SOCK_BUFFER, 0);
	l = 0;
	while ((i < len) && f) {
		if (b[i] == '\n' || b[i] == '\t' || b[i] == ' ' || b[i] == 0) l = i + 1;
		else f = 0;
		i++;
		}
	i = len, f = 1;
	r = i;
	while ((i > 0) && f) {
		i--;
		if (b[i] == '\n' || b[i] == '\t' || b[i] == ' ' || b[i] == 0) r = i;
		else f = 0;
		}

	printf ("message: ");
	for (i = l; i<r; i++) {
		printf("%c", b[i]);
		}
	printf ("\n");

	len = r - l;
	printf ("%d %d %d\n", len, l, r);
	if (r < l) return 0;

	*o = (char *) realloc (*o, (len+1) * sizeof(char));
	memset (*o, 0, len+1);
	memcpy (*o, b + l, len);
	printf("o = %s\n", *o);
	return len;
	}

int wrsock (int s, char * i, int l) {
	int c, len = l;
	printf("write: %s\n", i);
	while (len) {
		if ((c = write (s, i + (l - len), len)) < 0) {
			if (errno == EINTR || errno == EPIPE) continue;
			close (s);
			return -1;
			}
		len -= c;
		}
	return 0;
	}

/** serial **/
int set_serial (int fd, int speed, int parity) {
	struct termios tty;
	memset (&tty, 0, sizeof(tty));
	if (tcgetattr (fd, &tty) != 0) return -1;

	cfsetospeed (&tty, speed);
	cfsetispeed (&tty, speed);

	tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
	tty.c_iflag &= ~IGNBRK;
	tty.c_lflag = 0;
	tty.c_oflag = 0;
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 5;
	tty.c_iflag &= ~(IXOFF | IXANY); // IXON
	tty.c_cflag |=  (CLOCAL | CREAD);
	tty.c_cflag &= ~(PARENB | PARODD);
	tty.c_cflag |= parity;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	if (tcsetattr (fd, TCSANOW, &tty) !=0 ) return -1;
	return 0;
	}

int set_blocking (int fd, int speed, int blocking) {
	struct termios tty;
	memset (&tty, 0, sizeof(tty));
	if (tcgetattr (fd, &tty) != 0) return -1;

	cfsetospeed (&tty, speed);
	cfsetispeed (&tty, speed);

	tty.c_cc[VMIN] = blocking ? 1 : 0;
	tty.c_cc[VTIME] = 5;

	if (tcsetattr (fd, TCSANOW, &tty) !=0 ) return -1;
	return 0;
	}

/** sms **/
static void * sms_thread (void * arg) {
	struct smsnode * tmp, * sq = ((struct thrdarg *) arg)->sq;
	char ttyb[64];
	int tty, ttyr, ttyw;

	while (1) {
		sq = sq->next;
		if (sq->status == CRYPTO_SMS_STATUS_HOME) continue;
		if (sq->status == CRYPTO_SMS_STATUS_SENT) {
			printf ("sms sent. leaving queue %s\n", sq->number);
			tmp = sq;
			sq->prev->next = sq->next;
			sq->next->prev = sq->prev;
			sq = sq->prev;
			free (tmp->key);
			free (tmp->number);
			free (tmp);
			}
		if (sq->status == CRYPTO_SMS_STATUS_PENDING) {
			printf ("sending sms key %s\nto number %s\n", sq->key, sq->number);
			tty = open ("/dev/ttyUSB0", O_RDWR | O_NOCTTY | O_NDELAY);
			if (tty < 0) continue;
			set_serial (tty, B2400, 0);
			set_blocking (tty, B9600, 0);


			ttyw = write (tty, "AT+CMGF=1\r\n", 11);
			printf ("ttyw=%d\n", ttyw);
			sleep (1);
			sprintf (ttyb, "AT+CMGS=\"%s\"\r%s%c", sq->number, sq->key, (char) 26);
			ttyw = write (tty, ttyb, strlen(ttyb));
			printf ("ttyb=%s,\n", ttyb);
			printf ("ttyw=%d\n", ttyw);
			sleep (1);
			close (tty);

			sq->status = CRYPTO_SMS_STATUS_SENT;
			}
		if (sq->status == CRYPTO_SMS_STATUS_ERROR) {
			tmp = sq;
			sq->prev->next = sq->next;
			sq->next->prev = sq->prev;
			sq = sq->prev;
			free (tmp);
			}
		}
	}

/** database **/
unsigned short int chkpass (char * u, char * p, sqlite3 * db) {
	sqlite3_stmt * stmt;
	char query[CRYPTO_QUERY_BUFFER];
	int r = 0;

	sprintf (query, "select login from users where login='%s' and clear='%s'", u, p);
	if (sqlite3_prepare_v2 (db, query, -1, &stmt, 0) == SQLITE_OK) {
		if (SQLITE_ROW == sqlite3_step (stmt))
			r = 1;
		sqlite3_finalize(stmt);
		}

	return r;
	}

unsigned short int chkuotp (char * u, char * p, char * o, sqlite3 * db) {
	sqlite3_stmt * stmt;
	char query[CRYPTO_QUERY_BUFFER];
	unsigned long int t = (unsigned long int) time (NULL) + CRYPTO_OTP_LIFETIME;
	int r = 0;

	sprintf (query, "select login from users where login='%s' and clear='%s' and otp='%s' and %ld<expire", u, p, o, t);
	if (sqlite3_prepare_v2 (db, query, -1, &stmt, 0) == SQLITE_OK) {
		if (SQLITE_ROW == sqlite3_step (stmt))
			r = 1;
		sqlite3_finalize(stmt);
		}
	printf ("query: %s\n", query);

	return r;
	}

void mkpass (int, char **);
unsigned short int makeotp (char * u, char ** o, sqlite3 * db) {
	char query[CRYPTO_QUERY_BUFFER];
	unsigned long int t = (unsigned long int) time (NULL) + CRYPTO_OTP_LIFETIME;
	
	mkpass (8, o);

	sprintf (query, "update users set otp='%s',expire=%ld where login='%s'", *o, t, u);
	return SQLITE_OK == sqlite3_exec (db, query, 0, 0, 0) ? 1 : 0;
	}

unsigned short int getnum (char * u, char ** n, sqlite3 * db) {
	sqlite3_stmt * stmt;
	char query[CRYPTO_QUERY_BUFFER];
	int r = 0;

	*n = (char *) malloc (11 * sizeof (char));
	memset (*n, 0, 11);

	
	sprintf (query, "select phone from users where login='%s'", u);
	if (sqlite3_prepare_v2 (db, query, -1, &stmt, 0) == SQLITE_OK) {
		if (SQLITE_ROW == sqlite3_step (stmt)) {
			memcpy (*n, (char *) sqlite3_column_text (stmt, 0), 10);
			r = 1;
			printf ("phone: %s\n", *n);
			}
		sqlite3_finalize (stmt);
		}
	printf ("query: %s\n", query);

	return r;
	}

/** crypto **/
void mkpass (int len, char ** p) {
	int i, d;
	unsigned int b;
	unsigned int * r;
	const char * c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghujklmnopqrstuvwxyz0123456789";
	*p = (char *) malloc ((len + 1) * sizeof(char));
	r = (unsigned int *) malloc (len * sizeof(unsigned int));
	b = strlen (c);
	memset (*p, 0, len + 1);

	d = open ("/dev/random", O_RDONLY);
	read (d, r, len * sizeof (unsigned int));
	close (d);

	for (i = 0; i<len; i++)
		memset (*p + i, *(c + (*(r + i)) % b), 1);
	}

/** threads **/
static void * start_thread (void * arg) {
	int s = ((struct thrdarg *) arg)->s;
	sqlite3 * db = ((struct thrdarg *) arg)->db;
	struct smsnode * ns, * sq = ((struct thrdarg *) arg)->sq;
	int len;
	char * o, * user, * pass, * otp, * otpid;

	printf("thread\n");

	// send greeting
	if (wrsock (s, "Greetings!", 10) < 0) return;
	o = (char *) malloc (sizeof(char));
	while ((len = rdsock (s, &o)) >= 0) {
		if (len < 1) continue;
		printf ("server received: %s\nlen: %d\n", o, len);
		if (strstr (o, "quit") != NULL) {
			printf ("quit: %s\n", o);
			free (o);
			close (s);
			return;
			}
		if (strstr(o, "generate otp for") != NULL) {
			printf ("gen pass: %s\n", o);
			user = (char *) malloc ((len - 16) * sizeof(char));
			memset (user, 0, len - 16);
			memcpy (user, o + 17, len - 17);
			free (o);
			o = (char *) malloc (sizeof(char));

			printf ("user: %s\n", user);
			ns = (struct smsnode *) malloc (sizeof(struct smsnode));
			ns->status = CRYPTO_SMS_STATUS_PENDING;

			(void) makeotp (user, &ns->key, db);
			(void) getnum (user, &ns->number, db);

			ns->prev = sq;
			ns->next = sq->next;
			sq->next = ns;
			ns->next->prev = ns;

			if (wrsock (s, ns->key, 6) < 0) {
				free (o);
				return;
				}
			continue;
			}
		if (strstr(o, "check otp for") != NULL) {
			printf ("otp for: %s\n", o);
			user = (char *) malloc ((len - 13) * sizeof(char));
			memset (user, 0, len - 13);
			memcpy (user, o + 14, len - 14);
			free (o);
			o = (char *) malloc (sizeof(char));

			printf ("user: %s\n", user);

			if (wrsock (s, "OK", 3) < 0) {
				free (o);
				return;
				}
			continue;
			}

		if (strstr(o, "user otp is") != NULL) {
			printf ("otp: %s\n", o);
			otp = (char *) malloc ((len - 11) * sizeof (char));
			memset (otp, 0, len - 11);
			memcpy (otp, o + 12, len - 12);
			free (o);
			o = (char *) malloc (sizeof(char));

			printf ("otp: %s\n", otp);

			if (wrsock (s, "\n", 1) < 0) {
				printf ("cound not reply\n");
				free (o);
				return;
				}
			continue;
			}

		if (strstr(o, "otp id is") != NULL) {
			printf ("otpid: %s\n", o);
			otpid = (char *) malloc ((len - 9) * sizeof (char));
			memset (otpid, 0, len - 9);
			memcpy (otpid, o + 10, len - 10);
			free (o);
			o = (char *) malloc (sizeof(char));

			printf ("otpid: %s\n", otpid);

			if (wrsock (s, "\n", 1) < 0) {
				free (o);
				return;
				}
			continue;
			}

		if (strstr(o, "get check result") != NULL) {
			printf ("check: %s\n", o);
			free (o);
			o = (char *) malloc (sizeof(char));

			if (chkuotp(user, otp, otpid, db)) {
				if (wrsock (s, "OK", 2) < 0) {
					free (o);
					return;
					}
				}
			else {
				if (wrsock (s, "FAILED", 2) < 0) {
					free (o);
					return;
					}
				}
			continue;
			}
		bzero (o, len);
		}
	free (o);
	close (s);
	return;
	}

/** accept **/
int acsock (int s, sqlite3 * db, struct smsnode * sq) {
	int c, l;
	struct sockaddr_un cun;
	struct thrdarg arg;

	pthread_attr_t attr;
	pthread_t tid;

	if (s < 0) return -1;
	l = sizeof(struct sockaddr);
	if ((c = accept (s, (struct sockaddr *) &cun, &l))<0) return -1;
	printf ("accepted connection\n");
	
	arg.s = c;
	arg.sq = sq;
	arg.db = db;
	
	pthread_attr_init (&attr);
	pthread_create ( &tid, &attr, &start_thread, (void *) &arg);
	}

void signal_handler (int sig) {
	switch (sig) {
		case SIGHUP:
			break;
		case SIGTERM:
			break;
		default:
			break;
		}
	}

int main (int argc, char ** argv) {
	int sock;
	sqlite3 * db;
	struct smsnode * sq;
	
	struct thrdarg arg;
	pthread_attr_t attr;
	pthread_t tid;

	const char * dbi;
	pid_t pid, sid;

	//signal (SIGHUP, signal_handler);
	//signal (SIGTERM, signal_handler);
	//signal (SIGINT, signal_handler);
	//signal (SIGQUIT, signal_handler);
	
	
	/*pid = fork ();
	if (pid < 0) exit (EXIT_FAILURE);
	if (pid > 0) exit (EXIT_SUCCESS);

	umask (0);
	sid = setsid();
	if (sid < 0) exit (EXIT_FAILURE);

	if (chroot("/") < 0) exit (EXIT_FAILURE);
	if (chdir("/") < 0) exit (EXIT_FAILURE);
	if (setuid(CRYPTO_DAEMON_UID) < 0) exit (EXIT_FAILURE);
	if (setuid(CRYPTO_DAEMON_GID) < 0) exit (EXIT_FAILURE);

	close (STDIN_FILENO);
	close (STDOUT_FILENO);
	close (STDERR_FILENO);*/

	dbi = "create table if not exists users (login varchar(32) primary key, clear varchar(64), phone varchar(10), otp varchar(32), expire bigint, active tinyint)";

	if (sqlite3_open("/tmp/user", &db)) {
		printf ("error opening database");
		exit (1);
		}
	else
		sqlite3_exec (db, dbi, 0, 0, 0);

	sock = mksock ("/tmp/otpd");

	sq = (struct smsnode *) malloc (sizeof(struct smsnode));
	sq->status = -1;
	sq->next = sq;
	sq->prev = sq;

	arg.s = 0;
	arg.sq = sq;
	arg.db = db;

	pthread_attr_init (&attr);
	pthread_create (&tid, &attr, &sms_thread, (void *) &arg);

	while (1) (void) acsock (sock, db, sq);

	close (sock);
	sqlite3_close (db);

	return 0;
	}
