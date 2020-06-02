#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/stats.h>
#include <lwip/ip.h>
#include <lwip/init.h>
#include <lwip/tcp_impl.h>
#include <lwip/dns.h>
#include <mqttif.h>

#include "user_interface.h"

#include "tweetnacl.h"

struct mqtt_if_data {
	struct netif netif;
	ip_addr_t ipaddr;
	MQTT_Client *mqttcl;
	uint8_t *topic_pre;
	uint8_t *receive_topic;
	uint8_t *broadcast_topic;
	uint8_t key_set;
	u_char key[crypto_secretbox_KEYBYTES];
	u_char buf[2048];
	u_char cypherbuf_buf[2048];
};

//static pcap_dumper_t *pcap_dumper;

static err_t ICACHE_FLASH_ATTR
mqtt_if_output(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
struct mqtt_if_data *data = netif->state;
//struct ip_hdr *iph;
int len;
uint8_t buf[os_strlen(data->topic_pre) + 20];

	os_sprintf(buf, "%s/" IPSTR , data->topic_pre, IP2STR(ipaddr));
    if (data->key_set) 
	{
		randombytes(data->cypherbuf_buf, crypto_secretbox_NONCEBYTES);
		os_bzero(data->buf, crypto_secretbox_ZEROBYTES);
		len = pbuf_copy_partial(p, data->buf + crypto_secretbox_ZEROBYTES, sizeof(data->buf) - crypto_secretbox_ZEROBYTES, 0);
		crypto_secretbox(data->cypherbuf_buf + crypto_secretbox_NONCEBYTES, data->buf, len+crypto_secretbox_ZEROBYTES, data->cypherbuf_buf, data->key);

		MQTT_Publish(data->mqttcl, buf, data->cypherbuf_buf, len + crypto_secretbox_NONCEBYTES + crypto_secretbox_ZEROBYTES, 0, 1);
	} else {
		len = pbuf_copy_partial(p, data->buf, sizeof(data->buf), 0);

		//iph = (struct ip_hdr *)data->buf;
		//os_printf("packet %d, buf %x\r\n", len, p);
		//os_printf("to: " IPSTR " from: " IPSTR " via " IPSTR "\r\n", IP2STR(&iph->dest), IP2STR(&iph->src), IP2STR(ipaddr));

		MQTT_Publish(data->mqttcl, (const char*)buf, (const char*)data->buf, len, 0, 1);
	}
	return 0;
}

void ICACHE_FLASH_ATTR mqtt_if_input(struct mqtt_if_data *data, const char* topic, uint32_t topic_len, const char *mqtt_data, uint32_t mqtt_data_len)
{
  uint8_t buf[topic_len+1];
  os_strncpy(buf, topic, topic_len);
  buf[topic_len] = '\0';
  struct pbuf *pb;
  //os_printf("Received %s - %d bytes\r\n", buf, mqtt_data_len);

  if ((topic_len == os_strlen(data->receive_topic) && os_strncmp(topic, data->receive_topic, topic_len) == 0) || 
      (topic_len == os_strlen(data->broadcast_topic) && os_strncmp(topic, data->broadcast_topic, topic_len) == 0)) {

    if (data->key_set) 
	{
		unsigned char m[mqtt_data_len];
		uint32_t message_len = mqtt_data_len - crypto_secretbox_NONCEBYTES;

		if (crypto_secretbox_open(m, mqtt_data + crypto_secretbox_NONCEBYTES, message_len, mqtt_data, data->key) == -1)
		{
			os_printf("mqttif decrypt error\r\n");
			return;
		}

		pb = pbuf_alloc(PBUF_LINK, message_len-crypto_secretbox_ZEROBYTES, PBUF_RAM);
		//os_printf("pb: %x len: %d tot_len: %d\r\n", pb, pb->len, pb->tot_len);
		if (pb == NULL)
			return;
		pbuf_take(pb, m+crypto_secretbox_ZEROBYTES, message_len-crypto_secretbox_ZEROBYTES);
	} else {
		pb = pbuf_alloc(PBUF_LINK, mqtt_data_len, PBUF_RAM);
		if (pb == NULL)
			return;	
		pbuf_take(pb, mqtt_data, mqtt_data_len);

	}
	if (data->netif.input(pb, &data->netif) != ERR_OK) {
		pbuf_free(pb);
	}
  }
}

static err_t ICACHE_FLASH_ATTR
mqtt_if_init(struct netif *netif)
{
	NETIF_INIT_SNMP(netif, snmp_ifType_other, 0);
	netif->name[0] = 'm';
	netif->name[1] = 'q';

	netif->output = mqtt_if_output;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_LINK_UP;

	return 0;
}

struct mqtt_if_data ICACHE_FLASH_ATTR *
mqtt_if_add(MQTT_Client *cl, uint8_t *topic_prefix, uint8_t *password)
{
	struct mqtt_if_data *data;
    unsigned char h[crypto_hash_BYTES];

	data = calloc(1, sizeof(*data));
	data->mqttcl = cl;

	data->topic_pre = (uint8_t *)os_malloc(os_strlen(topic_prefix)+1);
	os_strcpy(data->topic_pre, topic_prefix);

	data->receive_topic = (uint8_t *)os_malloc(os_strlen(topic_prefix) + 20);
	os_sprintf(data->receive_topic, "%s/0.0.0.0", data->topic_pre);
	data->broadcast_topic = (uint8_t *)os_malloc(os_strlen(topic_prefix) + 20);
	os_sprintf(data->broadcast_topic, "%s/255.255.255.255", data->topic_pre);

	if (os_strlen(password) > 0) 
	{
		crypto_hash(h, password, os_strlen(password));
		os_memcpy(data->key, h, crypto_secretbox_KEYBYTES);
		data->key_set = 1;
	} else {
		data->key_set = 0;
	}
	netif_add(&data->netif, NULL, NULL, NULL, data, mqtt_if_init, ip_input);
//	netif_set_default(&data->netif);
	return data;
}

void ICACHE_FLASH_ATTR
mqtt_if_del(struct mqtt_if_data *data)
{
	mqtt_if_set_down(data);
	netif_remove(&data->netif);
	free(data->topic_pre);
	free(data->receive_topic);
	free(data->broadcast_topic);
	free(data);
}

void ICACHE_FLASH_ATTR
mqtt_if_subscribe(struct mqtt_if_data *data)
{
	MQTT_Subscribe(data->mqttcl, data->receive_topic, 0);
	MQTT_Subscribe(data->mqttcl, data->broadcast_topic, 0);
	//os_printf("Subscribe %s\r\n", data->receive_topic);
	//os_printf("Subscribe %s\r\n", data->broadcast_topic);

	data->netif.flags != NETIF_FLAG_LINK_UP;
}

void ICACHE_FLASH_ATTR
mqtt_if_unsubscribe(struct mqtt_if_data *data)
{
	MQTT_UnSubscribe(data->mqttcl, data->receive_topic);
	MQTT_UnSubscribe(data->mqttcl, data->broadcast_topic);
	//os_printf("Unsubscribe %s\r\n", data->receive_topic);
	//os_printf("Unsubscribe %s\r\n", data->broadcast_topic);

	data->netif.flags &= ~NETIF_FLAG_LINK_UP;
}

void ICACHE_FLASH_ATTR
mqtt_if_set_ipaddr(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_ipaddr(&data->netif, &ipaddr);
	data->ipaddr = ipaddr;

	os_sprintf(data->receive_topic, "%s/" IPSTR, data->topic_pre, IP2STR(&data->ipaddr));
}

void ICACHE_FLASH_ATTR
mqtt_if_set_netmask(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_netmask(&data->netif, &ipaddr);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_gw(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_gw(&data->netif, &ipaddr);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_up(struct mqtt_if_data *data)
{
	netif_set_up(&data->netif);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_down(struct mqtt_if_data *data)
{
	netif_set_down(&data->netif);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_mtu(struct mqtt_if_data *data, int mtu)
{
	data->netif.mtu = mtu;
}

void ICACHE_FLASH_ATTR
mqtt_if_set_flag(struct mqtt_if_data *data, int flag)
{
	data->netif.flags |= flag;
}

void ICACHE_FLASH_ATTR
mqtt_if_clear_flag(struct mqtt_if_data *data, int flag)
{
	data->netif.flags &= ~flag;
}

static int dns_count;

void ICACHE_FLASH_ATTR
mqtt_if_clear_dns(void)
{
	ip_addr_t addr;
//	addr.addr = INADDR_ANY;
	int i;
	for (i = 0; i < DNS_MAX_SERVERS; i++)
		dns_setserver(i, &addr);
	dns_count = 0;
}

void ICACHE_FLASH_ATTR
mqtt_if_add_dns(uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	dns_setserver(dns_count++, &ipaddr);
}
