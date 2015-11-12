#include <iostream>
#include <fstream>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/if_ether.h>
#include <netinet/ether.h>

#include <netpacket/packet.h>

#include <net/ethernet.h>
#include <net/if.h>

#include <arpa/inet.h>

#include <linux/if_link.h>

#include <unistd.h>
#include <getopt.h>
#include <err.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <netdb.h>


#define LAN 0
#define WAN 1
#define DEBUG 0

bool sig_int = false;

const std::string help = "./sixtunel \n\
------------------------------------------------------------------------------ \n\
| --help		    - Zobrazeni napovedy k programu \n\
| --lan [rozhrani]	    - Prichozi rozhrani (IPv6 sit) \n\
| --wan [rozhrani]	    - Odchozi rozhrani (IPv4 sit) \n\
| --remote [IP_adresa]      - IPv4 adresa konce tunelu \n\
| --log [cesta_k_souboru]   - Zadejte cestu k log. souboru \n\
------------------------------------------------------------------------------";

void catchTraffic(struct sockaddr *if1_ip, char **if1_name, char **remote, char **if2_name,
	bool mode);
void decapsPkt(unsigned char **buf, int *size, char **remote, char **if2_name, unsigned char*& sendbuf);
void sendToDest(unsigned char **buf, int size, char **dest);
void sendToTnl(unsigned char **buf, int size, char **remote, char **if2_name, bool mode);
void encapsPkt(int sck, unsigned char **buf, int *buf_size, char **remote, 
	char **wan, unsigned char*& sendbuf);
void decapFromTunnel();
void print_ip4_header(unsigned char *Buffer, int Size);
void checkParams(int argc, char **argv, char **lan, char **wan, char **remote, char **log);
void getAddrFromName(char *dev_name, struct sockaddr *ip, socklen_t ip_len);
void openLogFile(std::ofstream& logfile, char *file);
void handleSignal(int signum);
unsigned short cksum(struct iphdr *ip, int len);
using namespace std;

int main (int argc, char **argv)
{
    signal(SIGINT, handleSignal);
    char *lan,*wan,*remote, *log, *lan_ip_ptr, *wan_ip_ptr; 
    ofstream logfile;
    struct sockaddr lan_ip, wan_ip;
    lan_ip_ptr = new char [INET6_ADDRSTRLEN];
    wan_ip_ptr = new char [INET_ADDRSTRLEN];

    checkParams(argc, argv, &lan, &wan, &remote, &log);  
    cout << "remote = \"" << remote << "\"" << endl;
    cout << "log = \"" << log<< "\"" << endl;
    
    openLogFile(logfile, log);

    getAddrFromName(wan, &wan_ip, INET_ADDRSTRLEN);
    getAddrFromName(lan, &lan_ip, INET6_ADDRSTRLEN);
   

    inet_ntop(AF_INET, &(((struct sockaddr_in *)&wan_ip)->sin_addr), wan_ip_ptr, INET_ADDRSTRLEN);
    
    thread encap(catchTraffic, &lan_ip, &lan, &remote, &wan, true);
//    thread decap(catchTraffic, &wan_ip, &wan, &remote, &lan, false);
    
    
    encap.join();
//    decap.join();
    cout << "Vlakna ukoncena" << endl;
    delete lan_ip_ptr;
    delete wan_ip_ptr;
    return 0;
}

/**
 *  Odchytavani IPv4 komunikace z tunelu na WAN
 */
void decapsPkt(unsigned char **buf, int size, char **remote, char **if2_name, unsigned char
	*&sendbuf)
{
    struct iphdr *iph = (struct iphdr *)buf;
    struct sockaddr_in source,dest; 

    memset(&source, 0, sizeof(source));
    source.sin_addr.s_addr = iph->saddr;
    memset(&dest, 0, sizeof(dest));
    dest.sin_addr.s_addr = iph->daddr;

    printf("   |-source ip        : %s\n",inet_ntoa(source.sin_addr));
    printf("   |-destination ip   : %s\n",inet_ntoa(dest.sin_addr));
    
    unsigned long rmt_addr, if2_addr; // IP adresa v RAW forme 
    rmt_addr = inet_addr(*remote);
    if2_addr = inet_addr(*if2_name);

    if (source.sin_addr.s_addr == rmt_addr && dest.sin_addr.s_addr == if2_addr)
    {
	// odpoj IPv4 hlavicku
	int iphdr_len = sizeof(struct iphdr); // delka IPv4 hlavicky
	int data_len = size - iphdr_len;
	// posun se o delku hlavicky a odecti ji od celkove delky
	sendbuf = new unsigned char[data_len];
	memcpy(sendbuf, (*buf+iphdr_len), data_len); 
    }
    else
	sendbuf = NULL;
    

}

/**
 *  Odchytavani komunikace na rozhrani if1_name
 */
void catchTraffic(struct sockaddr *if1_ip, char **if1_name, char **remote, char **if2_name, bool
	mode)
{
    int insck, buf_len = 65536, rcv_len;
    unsigned int if1_ip_size = sizeof if1_ip;
    struct ifreq if_id;
    struct sockaddr_ll if1_bind;
    unsigned char *buf = new unsigned char[buf_len];

    // pro uvolneni socketu
    const int       optVal = 1;
    const socklen_t optLen = sizeof(optVal);

    // nastav odpooslouchavaci socket
    if((insck = socket(AF_PACKET,SOCK_DGRAM | SOCK_NONBLOCK, htons(41))) == -1){
	perror("socket(): ");
	exit(errno);
    }
    
    // Index if1_name rozhrani pro umozneni bindu
    // bindujeme aby nechytal i odesilane packet a nedelal tak nekonecnou smicku
    memset(&if_id, 0, sizeof(struct ifreq));
    strncpy(if_id.ifr_name, *if1_name, IFNAMSIZ-1);
    if (ioctl(insck, SIOCGIFINDEX, &if_id) < 0)
	perror("SIOCGIFINDEX");
    if1_bind.sll_family = AF_PACKET;
    if1_bind.sll_protocol = htons(ETH_P_ALL);
    if1_bind.sll_ifindex = if_id.ifr_ifru.ifru_ivalue; 

    if (bind(insck, (struct sockaddr*) &if1_bind, sizeof(struct sockaddr_ll)) == -1){
	perror("bind() failed: ");
	exit(errno);
    }
    if (!mode)
	cout << "Ahooj jsem tu a muj sock je #"<< insck << endl;
    else 
	cout << "Ahooj jsem tu a i ja muj sock je #"<< insck << endl;
    while(!sig_int)
    {
	memset(buf,0,buf_len);
	rcv_len = recvfrom(insck, buf, buf_len, 0, if1_ip, &if1_ip_size);
	if (rcv_len < 0 && errno != EWOULDBLOCK){
	    perror("recvfrom() failed: ");
	    exit(errno);
	}
	else if (errno == EWOULDBLOCK)
	    // pokud neprecetl zadny data uspi vlakno na 50ms (snizeni zatizeni CPU)
	    this_thread::sleep_for(chrono::milliseconds(50));
	if (rcv_len > 0)	
	{
	    cout << "Prijaty packet" << endl;
	    print_ip4_header(buf, rcv_len);
	    cout << "Odesif1_namey packet" << endl;
	    sendToTnl(&buf, rcv_len, remote, if2_name, mode);
	}
    }
    // jen si po sobe poklid
    delete[] buf;
    setsockopt(insck, SOL_SOCKET, SO_REUSEADDR, (void*) &optVal, optLen);
    close(insck);
}

/**
 *  Ziskej IPv6 adresu z hlavicky packetu ulozeneho v SENDBUF
 */
void getIPv6Addr(unsigned char *sendbuf, unsigned char **dest_ip)
{
    struct ip6_hdr *ip6h = (struct ip6_hdr *)sendbuf;
    *dest_ip = ip6h->ip6_dst.s6_addr; 
}

/**
 *  Odeslani 6in4 skrze WAN rozhrani
 */
void sendToTnl(unsigned char **buf, int size, char **remote, char **if2_name, bool mode)
{
    int sendsck;
    unsigned char *sendbuf={0};
    void *target_addr; // aby si sendto nestezoval a ja mohl specifikovat typ az v podmince
    socklen_t ta_size; 
    if((sendsck = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1){
	perror("socket() failed: ");
	exit(errno);
    }
    // posilas skrz tunel nebo konecnymu zarizeni?
    if (mode)
    {
	encapsPkt(sendsck, buf, &size, remote, if2_name, sendbuf);
	
	target_addr = new (struct sockaddr_in);
	struct sockaddr_in *ta = ((struct sockaddr_in *)target_addr);
	memset(ta, 0, sizeof(struct sockaddr_in));
	ta->sin_addr.s_addr = inet_addr(*remote);
	ta->sin_family = AF_INET;
	ta->sin_port = 0x00;
	ta_size = sizeof(*ta);
    }
    else
    {
	decapsPkt(buf, size, remote, if2_name, sendbuf);
	
	target_addr = new (struct sockaddr_in6);
	struct sockaddr_in6 *ta = ((struct sockaddr_in6 *)target_addr);
	memset(ta, 0, sizeof(struct sockaddr_in6));
	getIPv6Addr(sendbuf, (unsigned char **)&ta->sin6_addr.s6_addr);
	ta->sin6_family = AF_INET6;
	ta->sin6_port = 0x00;
	ta_size = sizeof(*ta);
    }


    // mam co poslat? zahozeni nechtenych paketu z rozhrani kde je tunel
    if (sendbuf != NULL)
    {
	print_ip4_header(sendbuf, size);
	cout << "sizeof: " << sizeof(*sendbuf) << ", char_traits: " << char_traits<unsigned
	    char>::length(sendbuf) << ", size: " << size << "mode: " << mode << endl;

	if ((sendto(sendsck,sendbuf,size,0,((struct sockaddr *)target_addr),ta_size)) == -1){ 
	    perror("sendto() failed: ");
	    exit(errno);
	}
	cout << "Odeslano" << endl;
    }
    if(mode)
        delete (struct sockaddr_in *)target_addr;
    else
	delete (struct sockaddr_in6 *)target_addr;
    delete[] sendbuf;
    close(sendsck);
}


/**
 *  Zapouzdreni IPv6 packetu IPv4 hlavickou
 */
void encapsPkt(int sck, unsigned char **buf, int *buf_size, char **remote, char **if2_name,
	unsigned char*& sendbuf)
{
    /**
     *	inspirovano: 
     *		https://austinmarton.wordpress.com/2011/09/14/sending-raw-ethernet-packets-from-a-specific-interface-in-c-on-linux/
     * hodnoty z:
     *		https://tools.ietf.org/html/rfc4213#section-3.5
     *		https://tools.ietf.org/html/rfc1700
    **/
    struct ifreq if_idx;

    // Index if2_name rozhrani
    memset(&if_idx, 0, sizeof(struct ifreq));
    strncpy(if_idx.ifr_name, *if2_name, IFNAMSIZ-1);
    if (ioctl(sck, SIOCGIFINDEX, &if_idx) < 0)
	perror("SIOCGIFINDEX");
    // IP if2_name rozhrani 
    struct ifreq if_ip;
    memset(&if_ip, 0, sizeof(struct ifreq));
    strncpy(if_ip.ifr_name, *if2_name, IFNAMSIZ-1);
    if (ioctl(sck, SIOCGIFADDR, &if_ip) < 0)
	perror("SIOCGIFADDR");

    // Delka etherne hlavicky + IP hlavicky + delka prijateho paketu
    int pkt_len = 0, sbs;
    sbs = sizeof(struct iphdr) + *buf_size;
    sendbuf = new unsigned char[sbs];
    memset(sendbuf, 0, sbs);
    
    // IP hlavicka
    struct iphdr *iph = (struct iphdr *)sendbuf;
    iph->ihl = 5;
    iph->version = 4;
    iph->tos = 0;
    iph->id = htons(54321);
    iph->ttl = 64; // time to live (pocet skoku)
    iph->protocol = 41; // IPv6 => 41
    iph->saddr = inet_addr(inet_ntoa(((struct sockaddr_in *)&if_ip.ifr_addr)->sin_addr));
    iph->daddr = inet_addr(*remote);
    pkt_len += sizeof(struct iphdr);

    // Pridej puvodni data z BUF za hlavicky v SENDBUF
    memcpy((sendbuf+sizeof(struct iphdr)), *buf, *buf_size);
    pkt_len += *buf_size;
//    iph->tot_len = htons(pkt_len); 

//    unsigned short checksum = cksum(iph,sizeof(struct iphdr));
//    iph->check = checksum;

    *buf_size = pkt_len;

    //print_ip4_header(sendbuf, *buf_size);

}


void print_ip4_header(unsigned char *buffer, int Size)
{
    struct iphdr *iph = (struct iphdr *)buffer;
    struct sockaddr_in source,dest; 
    memset(&source, 0, sizeof(source));
    source.sin_addr.s_addr = iph->saddr;

    memset(&dest, 0, sizeof(dest));
    dest.sin_addr.s_addr = iph->daddr;

    printf("\n");
    printf("ip header\n");
    printf("   |-ip version        : %d\n",(unsigned int)iph->version);
    printf("   |-ip header length  : %d dwords or %d bytes\n",(unsigned int)iph->ihl,((unsigned
		    int)(iph->ihl))*4);
    printf("   |-type of service   : %d\n",(unsigned int)iph->tos);
    printf("   |-ip total length   : %d  bytes(size of packet)\n",ntohs(iph->tot_len));
    printf("   |-identification :%d\n",ntohs(iph->id));
    printf("   |-ttl : %d\n",(unsigned int)iph->ttl);
    printf("   |-protocol : %d\n",(unsigned int)iph->protocol);
    printf("   |-checksum : %d\n",ntohs(iph->check));
    printf("   |-source ip        : %s\n",inet_ntoa(source.sin_addr));
    printf("   |-destination ip   : %s\n",inet_ntoa(dest.sin_addr));
    long hdr_len = sizeof(struct iphdr)+sizeof(struct ip6_hdr);
    unsigned char* data = buffer+hdr_len;
    cout << "size: " << Size << " hdr_len: " << hdr_len << endl;
    if (Size > hdr_len) 
        printf("Data: %s\n", data);
}


/**
 *  Otevreni logovaciho souboru
 */
void openLogFile(std::ofstream& logfile, char *file)
{
    logfile.open(file, ios::app | ios::out);
    if (!logfile.is_open()){
	perror("open() failed: ");
	exit(errno);
    }
    logfile << "Takto vypada hlavicka logu:\n DateTime SourceIP DestinationIP Proto SrcPort \
	DstPort Time" << endl;
}

/**
 *  Prevedeni jmena rozhrani na IP adresu
 */ 
void getAddrFromName(char *intf_name, struct sockaddr *ip, socklen_t ip_len)
{
    struct ifaddrs *allintfs, *intf;
    if(getifaddrs(&allintfs) == -1){
	perror("getifaddrs() failed: ");
	exit(errno);
    }
    
    // projdi vsechna vyhledana rozhrani
    for (intf = allintfs; intf != NULL; intf = intf->ifa_next)
    {
	if (!strcmp(intf->ifa_name, intf_name))
	{
	    int family = intf->ifa_addr->sa_family;
	    if (ip_len == INET_ADDRSTRLEN && family == AF_INET)
		*ip = *intf->ifa_addr;
		//inet_ntop(AF_INET, &(((struct sockaddr_in *) 
		//		intf->ifa_addr)->sin_addr), *ip, INET_ADDRSTRLEN);
	    else if (ip_len == INET6_ADDRSTRLEN && family == AF_INET6)
		*ip = *intf->ifa_addr;
		//inet_ntop(AF_INET6,&(((struct sockaddr_in6 *)
		//		intf->ifa_addr)->sin6_addr), *ip, INET6_ADDRSTRLEN);
	    //cout << "\t adrress: [" << family << "] ip: <"<< *ip << ">" << endl;
	}
    }
    // nacetl jsi zarizeni tak je zase uvolni
    freeifaddrs(allintfs);
    
}

/**
 *  Zpracovani a rozparsovani agrv do prislusnych promennych
 */

void checkParams(int argc, char **argv, char **lan, char **wan, char **remote, char **log)
{
    using namespace std;
    
    int option_index = 0, opt = -1; 
    static struct option long_options[]=
    {
	{"lan", required_argument, 0, 'a'},
	{"wan", required_argument, 0, 'b'},
	{"remote", required_argument, 0, 'c'},
	{"log", required_argument, 0, 'd'},
	{0,0,0,0}
    };

    if (DEBUG) // dokud ladime vypis pocet parametru, parametry a jejich hodnoty
    {
	cout << "argc = " << argc << endl;
	for (int i =0; i < argc; i++)
	    cout << i << ": " << argv[i] << endl;
    }
    if (argc < 9)
    {
	if (argc == 2 && strcmp(argv[1],"--help") == 0)
	{
	    cout << help << endl;
	    exit(0);
	}
	err(1,"Not enough arguments");
    }
    while((opt = getopt_long(argc,argv, "a:b:c:d:", long_options, &option_index)) != -1)
    {
	switch(opt) {
	    case 'a': *lan = optarg; break;
	    case 'b': *wan = optarg; break;
	    case 'c': *remote = optarg; break;
	    case 'd': *log = optarg; break;
	}
    }
}

void handleSignal(int signum){
    sig_int = true;
}

/* 
* Copyright (c) 1982, 1986, 1988, 1990, 1993, 1994
*     The Regents of the University of California.  All rights reserved.
*
* Redistribution and use in source and binary form, with or without
* modifications, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
* 3. All advertising materials mentioning features or use of this software
*    must display the following acknowledgement:
*      This product includes software developed by the University of 
*      California, Berkeley and its contributors.
* 4. Neither the name of the University nor the names of its contributors
*    may be used to endorse or promote products derived from this software
*    without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSITUTE GOODS
* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
* 
* Code of this function is slightly modified for easier usage in this code.
*/
unsigned short cksum(struct iphdr *ip, int len)
{
    long sum = 0;  /* assume 32 bit long, 16 bit short */

    while(len > 1){
	sum += *(((unsigned short*)ip++));
	if(sum & 0x80000000)  
	    sum = (sum & 0xFFFF) + (sum	>> 16);
	len -= 2;
    }

    if(len)       /* take care of left over byte */
	sum += (unsigned short) *(unsigned char *)ip;

    while(sum>>16)
	sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}
