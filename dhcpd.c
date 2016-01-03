#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include "dhcp_common.h"
#include "queue.h"

#define STAT_WAIT_DISCOVER  1
#define STAT_WAIT_REQUEST   2
#define STAT_WAIT_RELEASE   3

#define RECV_TIMEOUT		10
#define TIME_TO_LIVE		40

struct client {
    struct client *fp;
    struct client *bp;
    struct client *tout_fp;
    struct client *tout_bp;
    int start_time;
    int exp_time;
    short stat; 
    struct in_addr cli_id;
    uint16_t cli_port;
    struct in_addr alloc_addr;
    uint32_t netmask;
};
struct client client_list;

int check_requested_data(struct in_addr, uint32_t, uint16_t);
int check_address_used(struct in_addr, uint32_t, struct client*);
void timeout_handler(int);
void update_alarm();
void set_alarm(int);
void set_signal();
void set_client_timeout(struct client* c, uint16_t ttl);
void delete_tout_list(struct client*);
void insert_tout_list(struct client*);
struct client* get_client(struct in_addr*, int);
struct client* create_client();
void release_client(struct client*);
void print_client(struct client*);
void print_status_change(int pre, int post, struct in_addr* id);
void read_config(char*);

int main(int argc, char* argv[])
{
    int s, msgtype, count;
    char buf[512];
	struct dhcph head;
    in_port_t myport = DHCP_PORT;
    struct client *cli;
    struct sockaddr_in myskt;
    struct sockaddr_in skt;
    socklen_t sktlen = sizeof(skt);
	

	// arguments
	if(argc != 2){
		fprintf(stderr, "Usage: server <config_file>\n");
		exit(1);
	}

	read_config(argv[1]);

	// signal
	set_signal();

	// socket
    if((s = socket(PF_INET, SOCK_DGRAM, 0)) == -1){
        perror("socket");
        exit(1);
    }
    bzero(&myskt, sizeof(myskt));
    myskt.sin_family = AF_INET;
    myskt.sin_port = htons(myport);
    myskt.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(s, (struct sockaddr*)&myskt, sizeof(myskt)) < 0){
        perror("bind");
        exit(1);
    }
    
	// initialize
	bzero(&head, sizeof(head));
    
	client_list.fp = &client_list;
	client_list.bp = &client_list;
	client_list.tout_fp = &client_list;
	client_list.tout_bp = &client_list;

	// main loop
    for(;;){
        if((count = recvfrom(s, &head, sizeof(struct dhcph), 0, (struct sockaddr*)&skt, &sktlen)) < 0){
            perror("recvfrom");
            exit(1);
        }

		// output received data.
		fprintf(stderr, "Receive\n");
		print_dhcp_header(&head);

		// get client
		cli = get_client(&skt.sin_addr, head.type);
		
		if(cli == NULL){
			// type error
			fprintf(stderr, "%s: Type field should be DHCPDISCOVER when initial connection to this server.\n", inet_ntoa(skt.sin_addr));
			continue;
		}

		if(head.type == DHCPRELEASE){
			fprintf(stderr, "%s: release client\n", inet_ntoa(cli->cli_id));
			release_client(cli);
			continue;
		}
        
        switch(cli->stat) {
            case STAT_WAIT_DISCOVER:
                if(head.type == DHCPDISCOVER) {
					struct in_addr ip;
					uint32_t mask;

					head.type = DHCPOFFER;
					if(queue_pop(&ip, &mask) == -1){
						// no resources of ip address
						head.code = DHCP_CODE_ERR_NONE;
					} else {
						cli->alloc_addr = ip;
						cli->netmask = mask;
						head.address = ip.s_addr;
						head.netmask = mask;
						head.ttl = TIME_TO_LIVE;
						head.code = DHCP_CODE_OK;
					}
					if ((count = sendto(s, &head, sizeof(struct dhcph), 0, (struct sockaddr*)&skt, sktlen)) < 0){
						perror("sendto");
						exit(1);
					}
					set_client_timeout(cli, RECV_TIMEOUT);
					fprintf(stderr, "Send\n");
					print_dhcp_header(&head);

					cli->stat = STAT_WAIT_REQUEST;
					print_status_change(STAT_WAIT_DISCOVER, STAT_WAIT_REQUEST, &cli->cli_id);
                } else {
                    fprintf(stderr, "Type of received data is not DHCPDISCOVER. Ignore.\n");
                }
                break;
            case STAT_WAIT_REQUEST:
                if(head.type == DHCPREQUEST){
					switch(head.code){
						case DHCP_CODE_REQ_ALC:
							{
								struct in_addr ip = { head.address };
								uint32_t mask = head.netmask;
								struct client* c;
								int ttl = head.ttl;

								print_client(cli);

								bzero(&head, sizeof(head));

								// check header
								if(check_requested_data(ip, mask, ttl) < 0){
									break;
								}

								head.type = DHCPACK;

								if(check_address_used(ip, mask, cli) < 0){
									// already allocated address
									head.code = DHCP_CODE_ERR_OVL;
									cli->stat = STAT_WAIT_DISCOVER;
								} else {
									cli->stat = STAT_WAIT_RELEASE;
									head.code = DHCP_CODE_OK;
									head.ttl = ttl;
									head.address = cli->alloc_addr.s_addr;
									head.netmask = cli->netmask;
								}

								if ((count = sendto(s, &head, sizeof(struct dhcph), 0, (struct sockaddr*)&skt, sktlen)) < 0){
									perror("sendto");
									exit(1);
								}
								fprintf(stderr, "Send\n");
								print_dhcp_header(&head);

								set_client_timeout(cli,head.ttl);
								print_status_change(STAT_WAIT_REQUEST, cli->stat, &cli->cli_id);
							}
							break;
						default:
							fprintf(stderr, "Received invalid code. Ignore.\n");
							break;
					}
                } else {
                    fprintf(stderr, "Received unexpected type of data. Ignore.\n");
                }
                break;
			case STAT_WAIT_RELEASE:
				if(head.type == DHCPREQUEST){
					switch(head.code){
						case DHCP_CODE_REQ_EXT:
							{
								struct in_addr ip = { head.address };
								uint32_t mask = head.netmask;
								uint16_t ttl = head.ttl;

								bzero(&head, sizeof(head));
								head.type = DHCPACK;

								// check header
								if(check_requested_data(ip, mask, ttl) < 0){
									break;
								}
								if(check_address_used(ip, mask, cli) < 0){
									head.code = DHCP_CODE_ERR_OVL;
									cli->stat = STAT_WAIT_DISCOVER;
								} else {
									head.ttl = ttl;
									head.address = cli->alloc_addr.s_addr;
									head.netmask = cli->netmask;
								}

								if ((count = sendto(s, &head, sizeof(struct dhcph), 0, (struct sockaddr*)&skt, sktlen)) < 0){
									perror("sendto");
									exit(1);
								}
								fprintf(stderr, "Send\n");
								print_dhcp_header(&head);

								if(cli->stat != STAT_WAIT_RELEASE){
									print_status_change(STAT_WAIT_RELEASE, cli->stat, &cli->cli_id);
								} else {
									set_client_timeout(cli, head.ttl);
								}
							}
							break;
						default:
							fprintf(stderr, "Received invalid code. Ignore.\n");
							break;
					}
				} else {
					fprintf(stderr, "Received unexpected type of data. Ignore.\n");
				}
				break;

			default:
				fprintf(stderr, "Reached undefined status. Release this client.\n");
        		release_client(cli);
				break;
		}
    }
    
    if(close(s) < 0){
        perror("close");
        exit(1);
    }

	free(cli);
}

int check_requested_data(struct in_addr ip, uint32_t netmask, uint16_t ttl)
{
	if(find_address(ip, netmask) == NULL){
		// Requested invalid ip address
		fprintf(stderr, "Requested invalid ip address.\nignore.\n");
		return -1;
	}

	if(ttl > TIME_TO_LIVE){
		// Requested too long time.
		fprintf(stderr, "Attempted using too long time.\nignore.\n");
		return -1;
	}
	return 0;
}

int check_address_used(struct in_addr ip, uint32_t netmask, struct client* cli)
{
	struct client *c = &client_list;

	while((c = c->fp) != &client_list){
		if(c == cli) continue;
		if(c->alloc_addr.s_addr == ip.s_addr && cli->netmask == netmask){
			break;
		}
	}

	if(c == &client_list) return 0;
	else return -1;
}

void timeout_handler(int sig)
{
	struct client *c = client_list.tout_fp;

	switch(c->stat){
		case STAT_WAIT_REQUEST:
			fprintf(stderr, "%s: Timeout. Client state back to STAT_INITIAL.\n", inet_ntoa(c->cli_id));
			release_client(c);
			break;
		case STAT_WAIT_RELEASE:
			fprintf(stderr, "%s: Allocated IP Address expired. Exit this client.\n", inet_ntoa(c->cli_id));
			release_client(c);
			break;
	}

	delete_tout_list(c);
	update_alarm();
}

void update_alarm()
{
	struct client* c = client_list.tout_fp;
	struct timeval tp;
	int diff;
	if(c == &client_list){
		set_alarm(0);
		return;
	}

	gettimeofday(&tp, NULL);
	diff = c->exp_time - (int)tp.tv_sec;

	if(diff > 0){
		set_alarm(diff);
	} else {
		fprintf(stderr, "call timeout handler\n");
		timeout_handler(0);
	}
}

void set_alarm(int time)
{
//	fprintf(stderr, "set alarm in %d seconds.\n", time);
	alarm(time);
}

void set_signal()
{
	struct sigaction act;
	act.sa_handler = &timeout_handler;
	act.sa_flags = SA_RESTART;

	if(sigaction(SIGALRM, &act, NULL) < 0){
		perror("sigaction");
		exit(1);
	}
}

void set_client_timeout(struct client* c, uint16_t ttl)
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	c->start_time = (int)tp.tv_sec;
	c->exp_time = (int)tp.tv_sec + ttl;

	delete_tout_list(c);
	insert_tout_list(c);

	update_alarm();
}

void print_tout_list()
{
    int i;
    struct client* c = &client_list;
    fprintf(stderr, "/////print tout list//////\n");
    for(i = 0; ;i++){
        c = c->tout_fp;
        if(c == &client_list) break;
        fprintf(stderr, "%2d. %s\n", i, inet_ntoa(c->cli_id));
    }
}

void delete_tout_list(struct client* c)
{
	if(c->tout_bp != NULL && c->tout_fp != NULL){
		c->tout_bp->tout_fp = c->tout_fp;
		c->tout_fp->tout_bp = c->tout_bp;
		update_alarm();
	}
	c->tout_bp = NULL;
	c->tout_fp = NULL;

//	print_tout_list();
}

void insert_tout_list(struct client* cl)
{
	struct client* c = &client_list;

	while((c = c->tout_fp) != &client_list){
		if(c->exp_time > cl->exp_time){
			break;
		}
	}
	
	cl->tout_fp = c;
	cl->tout_bp = c->tout_bp;
	c->tout_bp->tout_fp = cl;
	c->tout_bp = cl;
}

struct client* get_client(struct in_addr* ip, int type)
{
	struct client* c = &client_list;

	while((c = c->fp) != &client_list){
		if(c->cli_id.s_addr == ip->s_addr){
			break;
		}
	}

	if(c == &client_list){
		if(type == DHCPDISCOVER){
			struct client *n = create_client();
			n->cli_id = *ip;
			c = n;
		} else {
			c = NULL;
		}
	}

	return c;
}

struct client* create_client()
{
	struct client *n = (struct client*)malloc(sizeof(struct client));
	n->stat = STAT_WAIT_DISCOVER;
	client_list.bp->fp = n;
	n->bp = client_list.bp;
	n->fp = &client_list;
	n->tout_fp = NULL;
	n->tout_bp = NULL;
	client_list.bp = n;
	return n;
}

void release_client(struct client* c)
{
	fprintf(stderr, "%s: release client\n", inet_ntoa(c->cli_id));
	if(queue_push(c->alloc_addr, c->netmask) == 0){
		fprintf(stderr, "free ip:%s netmask:%d\n", inet_ntoa(c->alloc_addr), c->netmask);
	}
	c->bp->fp = c->fp;
	c->fp->bp = c->bp;
	delete_tout_list(c);
	free(c);
}

void print_client(struct client *c)
{
	fprintf(stderr, "*********print client info start*********\n");
	fprintf(stderr, "Client IP Address: %s\n", inet_ntoa(c->cli_id));
	fprintf(stderr, "Given IP Address: %s\n", inet_ntoa(c->alloc_addr));
	fprintf(stderr, "Given Netmask: %d\n", c->netmask);
	fprintf(stderr, "Given TTL: %d\n", c->exp_time - c->start_time);
	fprintf(stderr, "*********print client info end  *********\n");
}

void get_status_string(int stat, char* str, int size)
{
	switch(stat){
		case STAT_WAIT_DISCOVER:
			strncpy(str, "STAT_WAIT_DISCOVER", size);
			break;
		case STAT_WAIT_REQUEST:
			strncpy(str, "STAT_WAIT_REQUEST", size);
			break;
		case STAT_WAIT_RELEASE:
			strncpy(str, "STAT_WAIT_RELEASE", size);
			break;
		default:
			strncpy(str, "UNDEFINED STATE", size);
			break;
	}
}

void print_status_change(int pre, int post, struct in_addr* id)
{
	char str_pre[24] = {0}, str_post[24] = {0};
	get_status_string(pre, str_pre, sizeof(str_pre)-1);
	get_status_string(post, str_post, sizeof(str_post)-1);
	fprintf(stderr, "%s: from %s to %s\n", inet_ntoa(*id), str_pre, str_post);
}

void read_config(char* file)
{
	char line[512];
	char ip[56];
	char mask[56];
	struct in_addr ipaddr;
	uint32_t netmask;
	FILE *fp;

	queue_init();

	if((fp = fopen(file, "r")) == NULL){
		fprintf(stderr, "cannot open config file\n");
		exit(1);
	}

	bzero(line, 512);

	while(fgets(line, 512 - 1, fp) != NULL){
		bzero(ip, 56);
		bzero(mask, 56);
		sscanf(line, "%55s %55s", ip, mask);
		inet_aton(ip, &ipaddr);
		netmask = (uint32_t)atoi(mask);
		queue_push(ipaddr, netmask);
		bzero(line, 512);
	}

	fclose(fp);

	// finish adding ip resources
	freeze_address();

	debug_print();
}
