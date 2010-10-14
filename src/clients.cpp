/*
 * clients.cpp
 *
 * Copyright 2009 microcai <microcai@sina.com>
 *
 * See COPYING for more details about this software's license
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>
#include <string>
#include <hash_map>
#include <list>
#include <map>
#include <syslog.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdio.h>

using namespace __gnu_cxx;

static void inet_ntoa(std::string & ret, in_addr_t ip)
{
	char buf[64];
	buf[63]=0;
	snprintf(buf,63,"%d.%d.%d.%d", ((u_char*) (&ip))[0],
			((u_char*) (&ip))[1], ((u_char*) (&ip))[2], ((u_char*) (&ip))[3]);
	ret = buf;
}
static void nat_disable_ip(const std::string & ip )
{
	GString * cmd = g_string_new("");
	g_string_printf(cmd,"iptables -t nat -D  POSTROUTING  --source  %s "
			   "-j MASQUERADE -o eth+",
			   ip.c_str());

	run_cmd(cmd->str);

	g_string_free(cmd,1);
}

static void nat_enbale_ip(const std::string & ip)
{
	GString * cmd = g_string_new("");
	g_string_printf(cmd,"iptables -t nat -A POSTROUTING --source %s "
				"-j MASQUERADE -o eth+", ip.c_str());

	run_cmd(cmd->str);

	g_string_free(cmd,1);
}

struct _HashFn
{
	size_t operator ()(in_addr_t __x) const
	{
		return ((u_char*)&__x)[3];
	}
	size_t operator ()(in_addr_t __x)
	{
		return ((u_char*)&__x)[3];
	}
};


//unordered_map;

struct MACADDR
{
	u_char madd[6];
	MACADDR(){}
	MACADDR(u_char mac[6]){
		memcpy(madd,mac,6);
	}
};

struct myless: public std::binary_function<MACADDR, MACADDR, bool>
{
	bool operator ()(const MACADDR & x, const MACADDR & y)
	{
		return memcmp(&x, &y, 6) < 0;
	}
};

struct ROOM;
struct client{
	in_addr_t	current_ip;
	struct MACADDR current_mac;
	struct ROOM *proom;
	std::string name;
	std::string ip;
	std::string mac;
	std::string id;
	std::string idtype;
    std::string RoomNum;
    std::string Floor;
    std::string Build;
    int nIndex;
};

struct ROOM{
	uint build;
	uint floor;
	uint room;
	std::map<MACADDR, client, myless> clients;
	MACADDR				bind_mac;
};

static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static std::map<MACADDR,in_addr_t,myless>	allowed_mac;
static hash_map<in_addr_t, MACADDR,_HashFn> enabled_ip(256);

static std::list<ROOM>							room_list;
static std::map<MACADDR,client*,myless>				clients;

gboolean mac_is_alowed(u_char mac[6])
{
	bool ret;
	std::map<MACADDR,in_addr_t>::iterator it;

	pthread_rwlock_rdlock(&lock);

	it = allowed_mac.find(mac);
	ret = (it != allowed_mac.end());
	pthread_rwlock_unlock(&lock);
	return ret;
}

gboolean mac_is_alowed_with_ip(u_char mac[6],in_addr_t ip)
{
	std::string  sip;
	std::map<MACADDR,in_addr_t>::iterator it;
	hash_map<in_addr_t, MACADDR,_HashFn>::iterator hit;
	bool ret;

	pthread_rwlock_rdlock(&lock);

	it =  allowed_mac.find(mac);

	ret = (it != ((std::map<MACADDR,in_addr_t,myless>*)&allowed_mac)->end());

	if (ret) // 允许的mac
	{

		/********************************************
		 * 更新 iptables 如果 ip 和 mac  对应有变
		 ********************************************/
		hit = ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->find(ip);

		if (hit == ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->end()) //  ip 不在 iptables 允许范围内
		{
			pthread_rwlock_unlock(&lock);
			pthread_rwlock_wrlock(&lock);

			hit = enabled_ip.find(ip);

			if (hit == enabled_ip.end())
			{
				enabled_ip.insert(std::pair<in_addr_t, MACADDR>(ip, mac));
				pthread_rwlock_unlock(&lock);
				//这个 ip 地址看来没有入 iptables 啊，得，改改。
				inet_ntoa(sip, ip);
				nat_enbale_ip(sip);
				// 存入 库存
				GString * sql = g_string_new("");

				std::map<MACADDR,client*,myless>::iterator cit;

				client *pct;

				cit = clients.find(mac);

				if(cit!=clients.end())
				{
					pct= cit->second ;
					pct->current_ip = ip;
					pct->ip = sip;

					g_string_printf(sql,"update roomer_list set IP_ADDR='%s' where nIndex='%d'",
							sip.c_str(),pct->nIndex);
					ksql_run_query_async(sql->str);

					syslog(LOG_NOTICE,"map ip %s to %s\n",sip.c_str(),sql->str);

					g_string_free(sql,1);

				}
				return ret;
			}
		}
//		else //ip 在 iptables 允许范围内
//		{
//		  不做任何操作，哈哈
//		}
	}else // 不被允许的 mac
	{
		/********************************************
		 * 更新 iptables 如果 ip 和 mac  对应有变
		 ********************************************/
		hit = ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->find(ip);

		if (hit != ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->end())
		{
			//  iptables 居然允许你？？断开他，妈的

			pthread_rwlock_unlock(&lock);
			pthread_rwlock_wrlock(&lock);

			hit = ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->find(ip);

			if (hit != ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->end())
			{
				inet_ntoa(sip, ip);
				nat_disable_ip(sip);
				enabled_ip.erase(ip);
			}
		}
		// else 这个 ip 地址看来没有入 iptables 啊，ok, 操作完成
	}
	pthread_rwlock_unlock(&lock);
	return ret;
}

void mac_set_allowed(u_char mac[6],gboolean allow ,in_addr_t ip)
{
	std::map<MACADDR,in_addr_t>::iterator it;

	pthread_rwlock_wrlock(&lock);

	it =  allowed_mac.find(mac);

	if (it == allowed_mac.end())
	{
		if(allow)
			allowed_mac.insert(std::pair<MACADDR,in_addr_t>(mac,0));
	}
	else
	{
		if(!allow)
			allowed_mac.erase(it);
	}

	if(!allow) // erase client data
	{
		std::map<MACADDR,client*,myless>::iterator it;

		it = clients.find(mac);

		if(it!=clients.end())
		{
			//有此数据啊,哈哈
			it->second->proom->clients.erase(mac);

			clients.erase(it);
		}
	}else if (ip && ip!=INADDR_NONE)
	{
		// add ip
		if (((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->find(ip) == ((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->end())
		{
			std::string sip;
			((hash_map<in_addr_t, MACADDR,_HashFn>*)&enabled_ip)->insert(std::pair<in_addr_t, MACADDR>(ip, mac));
			inet_ntoa(sip, ip);
			nat_enbale_ip(sip);
		}
	}

	pthread_rwlock_unlock(&lock);
}

gboolean set_client_data( u_char mac[6],  Clients_DATA * pcd )
{
	uint b, f, r;
	b = atoi(pcd->Build);
	f = atoi(pcd->Floor);
	r = atoi(pcd->RoomNum);

	ROOM room;
	client	ct;
	client * pclient;

	ct.idtype = pcd->CustomerIDType;
	ct.current_ip = pcd->ip;
	memcpy(ct.current_mac.madd,mac,6);
	ct.id = pcd->CustomerID;
	ct.ip = pcd->ip_addr;
	ct.mac = pcd->mac_addr;
	ct.name = pcd->CustomerName;
	ct.RoomNum = pcd->RoomNum;
	ct.Floor = pcd->Floor;
	ct.Build = pcd->Build;
	ct.proom = NULL;
	ct.nIndex = pcd->nIndex;


	std::list<ROOM>::iterator it;

	pthread_rwlock_wrlock(&lock);

	if(pcd)
	{
		do
		{
			// 首先， 你要寻找 客房
			for (it = room_list.begin(); it != room_list.end(); ++it)
			{
				ROOM *p = it.operator ->();
				if (p->build == b && p->floor == f && p->room == r)
				{ //就是这个房间了，哈哈
					p->clients.erase(mac);

					ct.proom = p;

					pclient = &p->clients.insert(
							std::pair<MACADDR, client>(mac, ct)).first->second;
					break;
				}
			}
			if(!ct.proom)
			{
				//房间都还没添加啊，呵呵
				memcpy(room.bind_mac.madd,mac,6);
				room.build = b;
				room.floor = f;
				room.room = r;
				room_list.insert(room_list.end(),room);
			}
		} while (ct.proom == NULL);
		//接着，插入表
		if(ct.proom)
		{
			clients.erase(mac);
			clients.insert(std::pair<MACADDR,client*>(mac,pclient));
		}
	}else
	{
		clients.erase(mac);
		for (it = room_list.begin(); it != room_list.end(); ++it)
		{
			ROOM *p = it.operator ->();
			p->clients.erase(mac);
		}
	}
	pthread_rwlock_unlock(&lock);
	return ct.proom?TRUE:FALSE;
}

gboolean get_client_data(u_char mac[6],Clients_DATA * pcd )
{
	std::map<MACADDR,client*,myless>::iterator it;

	client *pct;
	pthread_rwlock_rdlock(&lock);
	it = clients.find(mac);

	if(it!=clients.end())
	{
		pct= it->second ;

		strcpy(pcd->Build,pct->Build.c_str());
		strcpy(pcd->CustomerID,pct->id.c_str());
		strcpy(pcd->CustomerIDType , pct->idtype.c_str());
		strcpy(pcd->CustomerName ,pct->name.c_str());
		strcpy(pcd->Floor, pct->Floor.c_str());
		memcpy(pcd->MAC_ADDR,pct->current_mac.madd,6);
		strcpy(pcd->RoomNum, pct->RoomNum.c_str());
		pcd->ip = pct->current_ip;
		strcpy(pcd->ip_addr, pct->ip.c_str());
		strcpy(pcd->mac_addr , pct->mac.c_str());
	}
	bool ret = (it==clients.end());
	pthread_rwlock_unlock(&lock);
	return ret;
}
