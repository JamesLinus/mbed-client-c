/**
 * \file 	connected-home_full_ipv4.c
 *
 * \brief	Connected Home CoAP Server. Emulates a power node.
 *
 * \author 	Zach Shelby <zach@sensinode.com>
 *
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <signal.h> /* For SIGIGN and SIGINT */
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "arguments.h"

/* libCoap includes */
#include "sn_nsdl.h"
#include "sn_coap_header.h"
#include "sn_coap_protocol.h"
#include "sn_nsdl_lib.h"

#include "resource_generation_help.h"

#define BUFLEN 1024

/* Resource paths and registration parameters */
static uint8_t res_mgf[] = {"dev/mfg"};
static uint8_t res_mgf_val[] = {"Sensinode"};
static uint8_t res_mdl[] = {"dev/mdl"};
static uint8_t res_mdl_val[] = {"NSDL-C power node"};
static uint8_t res_bat[] = {"dev/bat"};
static uint8_t res_bat_val[] = {'1'};
static uint8_t res_pwr[] = {"pwr/0/w"};
static uint8_t res_pwr_val[] = {"80"};
static uint8_t res_pwr_val_off[] = {"0"};
static uint8_t res_rel[] = {"pwr/0/rel"};
static uint8_t res_temp[] = {"sen/temp"};
static uint8_t res_temp_val[] = {"25.4"};
static uint8_t res_type_test[] = {"t"};

static uint8_t ep[] = {"nsdl-power"};
static uint8_t ep_type[] = {"PowerNode"};
static uint8_t lifetime_ptr[] = {"1200"};

extern void stop_pgm();

/* Function templates */
int16_t svr_receive_msg(uint8_t *buf);
void *own_alloc(uint16_t size);
void own_free(void* ptr);
uint8_t tx_function(sn_nsdl_capab_e protocol, uint8_t *data_ptr, uint16_t data_len, sn_nsdl_addr_s *address_ptr);
uint8_t rx_function(sn_coap_hdr_s *coap_header, sn_nsdl_addr_s *address_ptr);
static void ctrl_c_handle_function(void);
typedef void (*signalhandler_t)(int); /* Function pointer type for ctrl-c */
static void coap_exec_poll_function(void);
static uint8_t relay_resource_cb(sn_coap_hdr_s *coap_ptr, sn_nsdl_addr_s *address, sn_proto_info_s * proto);
static uint8_t general_resource_cb(sn_coap_hdr_s *coap_ptr, sn_nsdl_addr_s *address, sn_proto_info_s * proto);
static int8_t compare_uripaths(sn_coap_hdr_s *coap_header, const uint8_t *uri_path_to_compare);
void print_array(uint8_t *ptr, uint16_t len);

/* Socket globals */
static struct sockaddr_in sa_dst, sa_src;
static int sock_server, slen_sa_dst=sizeof(sa_dst);

/* Thread globals */
static	pthread_t 	coap_exec_thread 				= 0; /* Thread for coap_exec-function */

/* CoAP related globals*/
uint16_t current_mid = 0;
uint8_t	 text_plain = COAP_CT_TEXT_PLAIN;
uint8_t	 link_format = COAP_CT_LINK_FORMAT;

uint8_t nsp_registered = 0;

/* Resource related globals*/
char relay_state = '1';
uint8_t *reg_location = 0;
int8_t reg_location_len;
uint8_t nsp_addr[16];
uint8_t obs_token[8];
uint8_t obs_token_len = 0;

uint8_t obs_number = 0;

/* Common globals */
uint8_t domain[] = {"domain"};

sn_nsdl_addr_s received_packet_address;
uint8_t received_address[4];

/*****************************************************/
/* This is called from main to start the CoAP server */
/*****************************************************/
int svr_ipv4(void)
{
	/* Local variables */
	uint8_t buf[BUFLEN];
	int16_t rcv_size=0;
	sn_nsdl_mem_s memory_struct;
	sn_nsdl_ep_parameters_s *endpoint_ptr = 0;
	sn_nsdl_resource_info_s	*resource_ptr = 0;

	memset(&received_packet_address, 0, sizeof(sn_nsdl_addr_s));
	received_packet_address.addr_ptr = received_address;

	/* Catch ctrl-c */
	if (signal(SIGINT, (signalhandler_t)ctrl_c_handle_function) == SIG_ERR)
	{
		printf("Error with SIGINT: %s\n", strerror(errno));
		return -1;
	}

#ifdef HAVE_DEBUG
	printf("\nCoAP server\nport: %i\n", arg_port);
#endif

	/* Open the server socket*/
	if ((sock_server=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1)
		stop_pgm("socket() error");

	/* Init the listen port addr*/
	memset((char *) &sa_src, 0, sizeof(sa_src));
	sa_src.sin_family = AF_INET;

	sa_src.sin_port = htons(arg_port);

	/* Listen to the port */
	sa_src.sin_addr.s_addr = INADDR_ANY;
	if (bind(sock_server, (struct sockaddr *) &sa_src, sizeof(sa_src))==-1)
		stop_pgm("bind() error");

	/* Initialize the libNsdl */
	memory_struct.sn_nsdl_alloc = &own_alloc;
	memory_struct.sn_nsdl_free = &own_free;

	sn_nsdl_init(&tx_function ,&rx_function, &memory_struct);

	inet_pton(AF_INET, arg_dst, &nsp_addr);

	set_NSP_address(nsp_addr, arg_dport, SN_NSDL_ADDRESS_TYPE_IPV4);

	pthread_create(&coap_exec_thread, NULL, (void *)coap_exec_poll_function, NULL);

	/* Create resources */

	resource_ptr = own_alloc(sizeof(sn_nsdl_resource_info_s));
	if(!resource_ptr)
		return 0;
	memset(resource_ptr, 0, sizeof(sn_nsdl_resource_info_s));

	resource_ptr->resource_parameters_ptr = own_alloc(sizeof(sn_nsdl_resource_parameters_s));
	if(!resource_ptr->resource_parameters_ptr)
	{
		own_free(resource_ptr);
		return 0;
	}
	memset(resource_ptr->resource_parameters_ptr, 0, sizeof(sn_nsdl_resource_parameters_s));

	CREATE_STATIC_RESOURCE(resource_ptr, sizeof(res_mgf)-1, (uint8_t*) res_mgf, sizeof(res_type_test)-1, (uint8_t*)res_type_test,  (uint8_t*) res_mgf_val, sizeof(res_mgf_val)-1);
	CREATE_STATIC_RESOURCE(resource_ptr, sizeof(res_mdl)-1, (uint8_t*) res_mdl, sizeof(res_type_test)-1, (uint8_t*)res_type_test,  (uint8_t*) res_mdl_val, sizeof(res_mdl_val)-1);

	CREATE_DYNAMIC_RESOURCE(resource_ptr, sizeof(res_bat)-1, (uint8_t*) res_bat, sizeof(res_type_test)-1, (uint8_t*)res_type_test, 1, &general_resource_cb) /* Observable resource */
	CREATE_DYNAMIC_RESOURCE(resource_ptr, sizeof(res_pwr)-1, (uint8_t*) res_pwr, sizeof(res_type_test)-1, (uint8_t*)res_type_test, 0, &general_resource_cb)
	CREATE_DYNAMIC_RESOURCE(resource_ptr, sizeof(res_rel)-1, (uint8_t*) res_rel, sizeof(res_type_test)-1, (uint8_t*)res_type_test, 0, &relay_resource_cb)
	CREATE_DYNAMIC_RESOURCE(resource_ptr, sizeof(res_temp)-1, (uint8_t*) res_temp, sizeof(res_type_test)-1, (uint8_t*)res_type_test, 0, &general_resource_cb)

	/* Register with NSP */
	INIT_REGISTER_NSDL_ENDPOINT(endpoint_ptr, ep, ep_type, lifetime_ptr);

	sn_nsdl_register_endpoint(endpoint_ptr);

	CLEAN_REGISTER_NSDL_ENDPOINT(endpoint_ptr);

	own_free(resource_ptr->resource_parameters_ptr);
	own_free(resource_ptr);

	/* 				Main loop.				*/
	/* Listen and process incoming messages */

	sleep(1);

	while (1)
	{
		usleep(100);
		memset(buf, 0, BUFLEN);
		rcv_size = svr_receive_msg(buf);
		if(rcv_size > 0)
		{
			sn_nsdl_process_coap(buf, rcv_size, &received_packet_address);
		}
	}
	return 0;
}

/****************************/
/* Message receive function */
/****************************/
int16_t svr_receive_msg(uint8_t *buf)
{
  char rcv_in_addr[32];
  int16_t rcv_size=0;

  memset(rcv_in_addr,0,32);

  if ((rcv_size=recvfrom(sock_server, buf, BUFLEN, 0, (struct sockaddr *)&sa_dst, (socklen_t*)&slen_sa_dst))==-1)
		stop_pgm("recvfrom()");
  else
  {
	inet_ntop(AF_INET, &(sa_dst.sin_addr),rcv_in_addr,INET_ADDRSTRLEN);

	received_packet_address.port = ntohs(sa_dst.sin_port);
	memcpy(received_packet_address.addr_ptr, &sa_dst.sin_addr, 4);
	received_packet_address.type = SN_NSDL_ADDRESS_TYPE_IPV4;
	received_packet_address.addr_len = 4;

#ifdef HAVE_DEBUG
	printf("\nRX %s.%d [%d B] - ", rcv_in_addr, ntohs(sa_dst.sin_port), rcv_size);
#endif

}

 return rcv_size;
}


/* These alloc and free functions are required for libCoap */
void *own_alloc(uint16_t size)
{
	if(size)
		return malloc(size);
	else
		return 0;
}

void own_free(void *ptr)
{
	if(ptr)
		free(ptr);
}

/* Unused function needed for libCoap protocol initialization */
uint8_t tx_function(sn_nsdl_capab_e protocol, uint8_t *data_ptr, uint16_t data_len, sn_nsdl_addr_s *address_ptr)
{

	/* Set NSP address and port */
	sa_dst.sin_family = AF_INET;
	sa_dst.sin_port = htons(address_ptr->port);
	memcpy(&sa_dst.sin_addr, address_ptr->addr_ptr, address_ptr->addr_len);

#ifdef HAVE_DEBUG
	printf("libNSDL TX [%d B]",data_len);
	printf(" - len = %d - type = %d\n", address_ptr->addr_len, address_ptr->type);
#endif

	/* Send the message */
	if (sendto(sock_server, data_ptr, data_len, 0, (const struct sockaddr *)&sa_dst, slen_sa_dst)==-1)
				stop_pgm("sendto() failed");

	return 1;
}

uint8_t rx_function(sn_coap_hdr_s *coap_header, sn_nsdl_addr_s *address_ptr)
{

	uint8_t i;

	if(!coap_header)
		return 0;

	if((coap_header->msg_code == COAP_MSG_CODE_RESPONSE_CREATED) && !nsp_registered)
	{
		reg_location_len = coap_header->options_list_ptr->location_path_len;

		if(reg_location)
			free(reg_location);

		reg_location = malloc(reg_location_len);

		if(!reg_location)
			return 0;

		memcpy(reg_location, coap_header->options_list_ptr->location_path_ptr, reg_location_len);
#ifdef HAVE_DEBUG
		printf("Registered to NSP: ");
		for(i = 0; i < reg_location_len; i++)
			printf("%c", *(reg_location+i));
		printf("\n");
#endif
		nsp_registered = 1;
	}

	return 0;
}

static void ctrl_c_handle_function(void)
{
#ifdef HAVE_DEBUG
	printf("Pressed ctrl-c\n");
#endif
	sn_nsdl_unregister_endpoint();
	usleep(100);

	if(reg_location)
		own_free(reg_location);

	exit(1);
}

static void coap_exec_poll_function(void)
{
	static uint32_t ns_system_time = 1;
	static uint8_t i = 0;

	while(1)
	{
		sleep(1);
		sn_nsdl_exec(ns_system_time);
		ns_system_time++;

		/* If observation received, start sending notifications */
		if(obs_token_len)
		{
			if(i >= 10)
			{
				printf("observation message ID %d\n", sn_nsdl_send_observation_notification(obs_token, obs_token_len, res_bat_val, sizeof(res_bat_val), &obs_number, 1, COAP_MSG_TYPE_NON_CONFIRMABLE, 0));
				if(res_bat_val[0] >= '4')
					res_bat_val[0] = '0';
				else
					res_bat_val[0] += 1;

				obs_number++;
				i = 0;
			}
			else
				i++;
		}

		/* Check if reregistration needed */
		if(!(ns_system_time % (uint32_t)120) && ns_system_time)
		{
			printf("reregister!\n");
			sn_nsdl_ep_parameters_s *endpoint_ptr = 0;

			INIT_REGISTER_NSDL_ENDPOINT(endpoint_ptr, ep, ep_type, lifetime_ptr);

			sn_nsdl_register_endpoint(endpoint_ptr);

			CLEAN_REGISTER_NSDL_ENDPOINT(endpoint_ptr);

		}
	}
}

static uint8_t relay_resource_cb(sn_coap_hdr_s *received_coap_ptr, sn_nsdl_addr_s *address, sn_proto_info_s * proto)
{
	sn_coap_hdr_s *coap_res_ptr = 0;


	printf("Relay callback\n");

	if (received_coap_ptr->msg_code == COAP_MSG_CODE_REQUEST_GET)
	{
		coap_res_ptr = sn_coap_build_response(received_coap_ptr, COAP_MSG_CODE_RESPONSE_CONTENT);
		coap_res_ptr->content_type_ptr = &text_plain;
		coap_res_ptr->content_type_len = sizeof(text_plain);
		coap_res_ptr->payload_len = sizeof(relay_state);
		coap_res_ptr->payload_ptr = &relay_state;
		sn_nsdl_send_coap_message(address, coap_res_ptr);

	}
	else if (received_coap_ptr->msg_code == COAP_MSG_CODE_REQUEST_PUT)
	{
		if (received_coap_ptr->payload_ptr && received_coap_ptr->payload_len < 2)
		{
			relay_state = received_coap_ptr->payload_ptr[0];
		}

		coap_res_ptr = sn_coap_build_response(received_coap_ptr, COAP_MSG_CODE_RESPONSE_CHANGED);
		if (received_coap_ptr->msg_type == COAP_MSG_TYPE_NON_CONFIRMABLE)
		{
			coap_res_ptr->msg_type = COAP_MSG_TYPE_NON_CONFIRMABLE;
			coap_res_ptr->msg_id = current_mid++;
		}
		sn_nsdl_send_coap_message(address, coap_res_ptr);
	}
	 /* Method not supported */
	else
	{
		printf("Method not supported\n");
		coap_res_ptr = sn_coap_build_response(coap_res_ptr, COAP_MSG_CODE_RESPONSE_METHOD_NOT_ALLOWED);
		sn_nsdl_send_coap_message(address, coap_res_ptr);
	}

	if(coap_res_ptr->token_ptr)
	{
		own_free(coap_res_ptr->token_ptr);
	}
	own_free(coap_res_ptr);

	return 0;
}

static uint8_t general_resource_cb(sn_coap_hdr_s *received_coap_ptr, sn_nsdl_addr_s *address, sn_proto_info_s * proto)
{
	sn_coap_hdr_s *coap_res_ptr = 0;
	uint8_t i = 0;

	printf("General callback\n");


	if (received_coap_ptr->msg_code == COAP_MSG_CODE_REQUEST_GET)
	{
		coap_res_ptr = sn_coap_build_response(received_coap_ptr, COAP_MSG_CODE_RESPONSE_CONTENT);
		coap_res_ptr->content_type_ptr = &text_plain;
		coap_res_ptr->content_type_len = sizeof(text_plain);



		/* res bat */
		if(compare_uripaths(received_coap_ptr, res_bat))
		{
			coap_res_ptr->options_list_ptr = own_alloc(sizeof(sn_coap_options_list_s));
			if(!coap_res_ptr->options_list_ptr)
				sn_coap_parser_release_allocated_coap_msg_mem(coap_res_ptr);

			memset(coap_res_ptr->options_list_ptr, 0, sizeof(sn_coap_options_list_s));
			coap_res_ptr->options_list_ptr->observe_len = 1;
			coap_res_ptr->options_list_ptr->observe_ptr = &obs_number;

			obs_number ++;

			coap_res_ptr->payload_len = sizeof(res_bat_val);
			coap_res_ptr->payload_ptr = res_bat_val;

			if(received_coap_ptr->options_list_ptr)
			{
				if(received_coap_ptr->options_list_ptr->observe)
					printf("Observe\n");
			}
			if(received_coap_ptr->token_ptr)
			{
				printf("token:");
				while(i < received_coap_ptr->token_len)
				{
					printf("%x:", *(received_coap_ptr->token_ptr + i));
					i++;
				}
				printf("\n");
				memset(obs_token, 0, 8);
				memcpy(obs_token, received_coap_ptr->token_ptr, received_coap_ptr->token_len);
				obs_token_len = received_coap_ptr->token_len;
			}
		}

		/* res pwr */
		else if(compare_uripaths(received_coap_ptr, res_pwr))
		{
			i = 0;
			coap_res_ptr->options_list_ptr = own_alloc(sizeof(sn_coap_options_list_s));
			if(!coap_res_ptr->options_list_ptr)
				sn_coap_parser_release_allocated_coap_msg_mem(coap_res_ptr);

			memset(coap_res_ptr->options_list_ptr, 0, sizeof(sn_coap_options_list_s));

			coap_res_ptr->options_list_ptr->max_age_ptr = &i;
			coap_res_ptr->options_list_ptr->max_age_len = 1;

			if(relay_state == '1')
			{
				coap_res_ptr->payload_len = sizeof(res_pwr_val)-1;
				coap_res_ptr->payload_ptr = res_pwr_val;
			}
			else if(relay_state == '0')
			{
				coap_res_ptr->payload_len = sizeof(res_pwr_val_off)-1;
				coap_res_ptr->payload_ptr = res_pwr_val_off;
			}
		}

		/* res temp */
		else if(compare_uripaths(received_coap_ptr, res_temp))
		{
			coap_res_ptr->payload_len = sizeof(res_temp_val)-1;
			coap_res_ptr->payload_ptr = res_temp_val;
		}
		sn_nsdl_send_coap_message(address, coap_res_ptr);

	}
	 /* Method not supported */
	else
	{
		printf("Method not supported\n");
		coap_res_ptr = sn_coap_build_response(received_coap_ptr, COAP_MSG_CODE_RESPONSE_METHOD_NOT_ALLOWED);
		sn_nsdl_send_coap_message(address, coap_res_ptr);
	}

	if(coap_res_ptr->token_ptr)
	{
		own_free(coap_res_ptr->token_ptr);
	}

	if(coap_res_ptr->options_list_ptr)
		own_free(coap_res_ptr->options_list_ptr);
	own_free(coap_res_ptr);

	return 0;
}

static int8_t compare_uripaths(sn_coap_hdr_s *coap_header, const uint8_t *uri_path_to_compare)
{
    if(memcmp(coap_header->uri_path_ptr,&uri_path_to_compare[0], coap_header->uri_path_len) == 0)
	{
		return 1;
	}
	return 0;
}

void print_array(uint8_t *ptr, uint16_t len)
{
	uint16_t i = 0;

	while(i < len)
	{
		printf("%x:", *(ptr+i));
		i++;
	}
	printf("\n");
}