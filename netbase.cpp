// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2011 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#define BSD_SOURCE

#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "netbase.h"
#include "util.h"

#ifndef WIN32
#include <sys/fcntl.h>
#endif

using namespace std;

string strprintf(const std::string &format, ...)
{
    char buffer[50000];
    char* p = buffer;
    int limit = sizeof(buffer);
    int ret;
    loop
    {
        va_list arg_ptr;
        va_start(arg_ptr, format);
        ret = vsnprintf(p, limit, format.c_str(), arg_ptr);
        va_end(arg_ptr);
        if (ret >= 0 && ret < limit)
            break;
        if (p != buffer)
            delete[] p;
        limit *= 2;
        p = new char[limit];
        if (p == NULL)
            throw std::bad_alloc();
    }
    string str(p, p+ret);
    if (p != buffer)
        delete[] p;
    return str;
}


int nConnectTimeout = 5000;

static const unsigned char pchIPv4[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

bool static LookupIntern(const char *pszName, std::vector<CIP>& vIP, int nMaxSolutions, bool fAllowLookup)
{
    vIP.clear();
    struct addrinfo aiHint = {};
    aiHint.ai_socktype = SOCK_STREAM;
    aiHint.ai_protocol = IPPROTO_TCP;
#ifdef WIN32
#  ifdef USE_IPV6
    aiHint.ai_family = AF_UNSPEC;
    aiHint.ai_flags = fAllowLookup ? 0 : AI_NUMERICHOST;
#  else
    aiHint.ai_family = AF_INET;
    aiHint.ai_flags = fAllowLookup ? 0 : AI_NUMERICHOST;
#  endif
#else
#  ifdef USE_IPV6
    aiHint.ai_family = AF_UNSPEC;
    aiHint.ai_flags = AI_ADDRCONFIG | (fAllowLookup ? 0 : AI_NUMERICHOST);
#  else
    aiHint.ai_family = AF_INET;
    aiHint.ai_flags = AI_ADDRCONFIG | (fAllowLookup ? 0 : AI_NUMERICHOST);
#  endif
#endif
    struct addrinfo *aiRes = NULL;
    int nErr = getaddrinfo(pszName, NULL, &aiHint, &aiRes);
    if (nErr)
        return false;

    struct addrinfo *aiTrav = aiRes;
    while (aiTrav != NULL && (nMaxSolutions == 0 || vIP.size() < nMaxSolutions))
    {
        if (aiTrav->ai_family == AF_INET)
        {
            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in));
            vIP.push_back(CIP(((struct sockaddr_in*)(aiTrav->ai_addr))->sin_addr));
        }

#ifdef USE_IPV6
        if (aiTrav->ai_family == AF_INET6)
        {
            assert(aiTrav->ai_addrlen >= sizeof(sockaddr_in6));
            vIP.push_back(CIP(((struct sockaddr_in6*)(aiTrav->ai_addr))->sin6_addr));
        }
#endif

        aiTrav = aiTrav->ai_next;
    }

    freeaddrinfo(aiRes);

    return (vIP.size() > 0);
}

bool LookupHost(const char *pszName, std::vector<CIP>& vIP, int nMaxSolutions, bool fAllowLookup)
{
    if (pszName[0] == 0)
        return false;
    char psz[256];
    char *pszHost = psz;
    strncpy(psz, pszName, sizeof(psz)-1);
    psz[255] = 0;
    if (psz[0] == '[' && psz[strlen(psz)-1] == ']')
    {
        pszHost = psz+1;
        psz[strlen(psz)-1] = 0;
    }

    return LookupIntern(pszHost, vIP, nMaxSolutions, fAllowLookup);
}

bool LookupHostNumeric(const char *pszName, std::vector<CIP>& vIP, int nMaxSolutions)
{
    return LookupHost(pszName, vIP, nMaxSolutions, false);
}

bool Lookup(const char *pszName, CIPPort& addr, int portDefault, bool fAllowLookup)
{
    if (pszName[0] == 0)
        return false;
    int port = portDefault;
    char psz[256];
    char *pszHost = psz;
    strncpy(psz, pszName, sizeof(psz)-1);
    psz[255] = 0;
    char* pszColon = strrchr(psz+1,':');
    char *pszPortEnd = NULL;
    int portParsed = pszColon ? strtoul(pszColon+1, &pszPortEnd, 10) : 0;
    if (pszColon && pszPortEnd && pszPortEnd[0] == 0)
    {
        if (psz[0] == '[' && pszColon[-1] == ']')
        {
            pszHost = psz+1;
            pszColon[-1] = 0;
        }
        else
            pszColon[0] = 0;
        if (port >= 0 && port <= USHRT_MAX)
            port = portParsed;
    }
    else
    {
        if (psz[0] == '[' && psz[strlen(psz)-1] == ']')
        {
            pszHost = psz+1;
            psz[strlen(psz)-1] = 0;
        }

    }

    std::vector<CIP> vIP;
    bool fRet = LookupIntern(pszHost, vIP, 1, fAllowLookup);
    if (!fRet)
        return false;
    addr = CIPPort(vIP[0], port);
    return true;
}

bool LookupNumeric(const char *pszName, CIPPort& addr, int portDefault)
{
    return Lookup(pszName, addr, portDefault, false);
}

bool CIPPort::ConnectSocket(SOCKET& hSocketRet, int nTimeout) const
{
    hSocketRet = INVALID_SOCKET;

    SOCKET hSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hSocket == INVALID_SOCKET)
    {
        // printf("Failed to create socket: %s\n", strerror(errno));
        return false;
    }

#ifdef SO_NOSIGPIPE
    int set = 1;
    setsockopt(hSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&set, sizeof(int));
#endif

    struct sockaddr_in sockaddr;
    GetSockAddr(&sockaddr);

#ifdef WIN32
    u_long fNonblock = 1;
    if (ioctlsocket(hSocket, FIONBIO, &fNonblock) == SOCKET_ERROR)
#else
    int fFlags = fcntl(hSocket, F_GETFL, 0);
    if (fcntl(hSocket, F_SETFL, fFlags | O_NONBLOCK) == -1)
#endif
    {
        // printf("Failed to set socket NONBLOCK\n");
        closesocket(hSocket);
    }


    if (connect(hSocket, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == SOCKET_ERROR)
    {
        // WSAEINVAL is here because some legacy version of winsock uses it
        if (WSAGetLastError() == WSAEINPROGRESS || WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINVAL)
        {
            struct timeval timeout;
            timeout.tv_sec  = nTimeout / 1000;
            timeout.tv_usec = (nTimeout % 1000) * 1000;

            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(hSocket, &fdset);
            int nRet = select(hSocket + 1, NULL, &fdset, NULL, &timeout);
            if (nRet == 0)
            {
                // printf("connection timeout\n");
                closesocket(hSocket);
                return false;
            }
            if (nRet == SOCKET_ERROR)
            {
                // printf("select() for connection failed: %s\n",strerror(WSAGetLastError()));
                closesocket(hSocket);
                return false;
            }
            socklen_t nRetSize = sizeof(nRet);
#ifdef WIN32
            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, (char*)(&nRet), &nRetSize) == SOCKET_ERROR)
#else
            if (getsockopt(hSocket, SOL_SOCKET, SO_ERROR, &nRet, &nRetSize) == SOCKET_ERROR)
#endif
            {
                // printf("getsockopt() for connection failed: %s\n",strerror(WSAGetLastError()));
                closesocket(hSocket);
                return false;
            }
            if (nRet != 0)
            {
                // printf("connect() failed after select(): %s\n",strerror(nRet));
                closesocket(hSocket);
                return false;
            }
        }
#ifdef WIN32
        else if (WSAGetLastError() != WSAEISCONN)
#else
        else
#endif
        {
            // printf("connect() failed: %i\n",WSAGetLastError());
            closesocket(hSocket);
            return false;
        }
    }

    // this isn't even strictly necessary
    // CNode::ConnectNode immediately turns the socket back to non-blocking
    // but we'll turn it back to blocking just in case
#ifdef WIN32
    fNonblock = 0;
    if (ioctlsocket(hSocket, FIONBIO, &fNonblock) == SOCKET_ERROR)
#else
    fFlags = fcntl(hSocket, F_GETFL, 0);
    if (fcntl(hSocket, F_SETFL, fFlags & !O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        // printf("Failed to set socket blocking\n");
        closesocket(hSocket);
        return false;
    }

    hSocketRet = hSocket;
    return true;
}

void CIP::Init()
{
    memset(ip, 0, 16);
}

void CIP::SetIP(const CIP& ipIn)
{
    memcpy(ip, ipIn.ip, sizeof(ip));
}

CIP::CIP()
{
    Init();
}

CIP::CIP(const struct in_addr& ipv4Addr)
{
    memcpy(ip,    pchIPv4, 12);
    memcpy(ip+12, &ipv4Addr, 4);
}

#ifdef USE_IPV6
CIP::CIP(const struct in6_addr& ipv6Addr)
{
    memcpy(ip, &ipv6Addr, 16);
}
#endif

CIP::CIP(const char *pszIp, bool fAllowLookup)
{
    Init();
    std::vector<CIP> vIP;
    if (LookupHost(pszIp, vIP, 1, fAllowLookup))
        *this = vIP[0];
}

CIP::CIP(const std::string &strIp, bool fAllowLookup)
{
    Init();
    std::vector<CIP> vIP;
    if (LookupHost(strIp.c_str(), vIP, 1, fAllowLookup))
        *this = vIP[0];
}

int CIP::GetByte(int n) const
{
    return ip[15-n];
}

bool CIP::IsIPv4() const
{
    return (memcmp(ip, pchIPv4, sizeof(pchIPv4)) == 0);
}

bool CIP::IsRFC1918() const
{
    return IsIPv4() && (
        GetByte(3) == 10 || 
        (GetByte(3) == 192 && GetByte(2) == 168) || 
        (GetByte(3) == 172 && (GetByte(2) >= 16 && GetByte(2) <= 31)));
}

bool CIP::IsRFC3927() const
{
    return IsIPv4() && (GetByte(3) == 169 && GetByte(2) == 254);
}

bool CIP::IsRFC3849() const
{
    return GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x0D && GetByte(12) == 0xB8;
}

bool CIP::IsRFC3964() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x02);
}

bool CIP::IsRFC6052() const
{
    static const unsigned char pchRFC6052[] = {0,0x64,0xFF,0x9B,0,0,0,0,0,0,0,0};
    return (memcmp(ip, pchRFC6052, sizeof(pchRFC6052)) == 0);
}

bool CIP::IsRFC4380() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0 && GetByte(12) == 0);
}

bool CIP::IsRFC4862() const
{
    static const unsigned char pchRFC4862[] = {0xFE,0x80,0,0,0,0,0,0};
    return (memcmp(ip, pchRFC4862, sizeof(pchRFC4862)) == 0);
}

bool CIP::IsRFC4193() const
{
    return ((GetByte(15) & 0xFE) == 0xFC);
}

bool CIP::IsRFC6145() const
{
    static const unsigned char pchRFC6145[] = {0,0,0,0,0,0,0,0,0xFF,0xFF,0,0};
    return (memcmp(ip, pchRFC6145, sizeof(pchRFC6145)) == 0);
}

bool CIP::IsRFC4843() const
{
    return (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 && GetByte(12) & 0xF0 == 0x10);
}

bool CIP::IsLocal() const
{
    // IPv4 loopback
   if (IsIPv4() && (GetByte(3) == 127 || GetByte(3) == 0))
       return true;

   // IPv6 loopback (::1/128)
   static const unsigned char pchLocal[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
   if (memcmp(ip, pchLocal, 16) == 0)
       return true;

   return false;
}

bool CIP::IsMulticast() const
{
    return    (IsIPv4() && (GetByte(3) & 0xF0) == 0xE0)
           || (GetByte(15) == 0xFF);
}

bool CIP::IsValid() const
{
    // Clean up 3-byte shifted addresses caused by garbage in size field
    // of addr messages from versions before 0.2.9 checksum.
    // Two consecutive addr messages look like this:
    // header20 vectorlen3 addr26 addr26 addr26 header20 vectorlen3 addr26 addr26 addr26...
    // so if the first length field is garbled, it reads the second batch
    // of addr misaligned by 3 bytes.
    if (memcmp(ip, pchIPv4+3, sizeof(pchIPv4)-3) == 0)
        return false;

    // unspecified IPv6 address (::/128)
    unsigned char ipNone[16] = {};
    if (memcmp(ip, ipNone, 16) == 0)
        return false;

    // documentation IPv6 address
    if (IsRFC3849())
        return false;

    if (IsIPv4())
    {
        // INADDR_NONE
        uint32_t ipNone = INADDR_NONE;
        if (memcmp(ip+12, &ipNone, 4) == 0)
            return false;

        // 0
        ipNone = 0;
        if (memcmp(ip+12, &ipNone, 4) == 0)
            return false;
    }

    return true;
}

bool CIP::IsRoutable() const
{
    return IsValid() && !(IsRFC1918() || IsRFC3927() || IsRFC4862() || IsRFC4193() || IsRFC4843() || IsLocal());
}

std::string CIP::ToString() const
{
    if (IsIPv4())
        return strprintf("%u.%u.%u.%u", GetByte(3), GetByte(2), GetByte(1), GetByte(0));
    else
        return strprintf("%x:%x:%x:%x:%x:%x:%x:%x",
                         GetByte(15) << 8 | GetByte(14), GetByte(13) << 8 | GetByte(12),
                         GetByte(11) << 8 | GetByte(10), GetByte(9) << 8 | GetByte(8),
                         GetByte(7) << 8 | GetByte(6), GetByte(5) << 8 | GetByte(4),
                         GetByte(3) << 8 | GetByte(2), GetByte(1) << 8 | GetByte(0));
}

bool operator==(const CIP& a, const CIP& b)
{
    return (memcmp(a.ip, b.ip, 16) == 0);
}

bool operator!=(const CIP& a, const CIP& b)
{
    return (memcmp(a.ip, b.ip, 16) != 0);
}

bool operator<(const CIP& a, const CIP& b)
{
    return (memcmp(a.ip, b.ip, 16) < 0);
}

bool CIP::GetInAddr(struct in_addr* pipv4Addr) const
{
    if (!IsIPv4())
        return false;
    memcpy(pipv4Addr, ip+12, 4);
    return true;
}

#ifdef USE_IPV6
bool CIP::GetIn6Addr(struct in6_addr* pipv6Addr) const
{
    memcpy(pipv6Addr, ip, 16);
    return true;
}
#endif

// get canonical identifier of an address' group
// no two connections will be attempted to addresses with the same group
std::vector<unsigned char> CIP::GetGroup() const
{
    std::vector<unsigned char> vchRet;
    int nClass = 0; // 0=IPv6, 1=IPv4, 255=unroutable
    int nStartByte = 0;
    int nBits = 16;

    // for unroutable addresses, each address is considered different
    if (!IsRoutable())
    {
        nClass = 255;
        nBits = 128;
    }
    // for IPv4 addresses, '1' + the 16 higher-order bits of the IP
    // includes mapped IPv4, SIIT translated IPv4, and the well-known prefix
    else if (IsIPv4() || IsRFC6145() || IsRFC6052())
    {
        nClass = 1;
        nStartByte = 12;
    }
    // for 6to4 tunneled addresses, use the encapsulated IPv4 address
    else if (IsRFC3964())
    {
        nClass = 1;
        nStartByte = 2;
    }
    // for Teredo-tunneled IPv6 addresses, use the encapsulated IPv4 address
    else if (IsRFC4380())
    {
        vchRet.push_back(1);
        vchRet.push_back(GetByte(3) ^ 0xFF);
        vchRet.push_back(GetByte(2) ^ 0xFF);
        return vchRet;
    }
    // for he.net, use /36 groups
    else if (GetByte(15) == 0x20 && GetByte(14) == 0x11 && GetByte(13) == 0x04 && GetByte(12) == 0x70)
        nBits = 36;
    // for the rest of the IPv6 network, use /32 groups
    else
        nBits = 32;

    vchRet.push_back(nClass);
    while (nBits >= 8)
    {
        vchRet.push_back(GetByte(15 - nStartByte));
        nStartByte++;
        nBits -= 8;
    }
    if (nBits > 0)
        vchRet.push_back(GetByte(15 - nStartByte) | ((1 << nBits) - 1));

    return vchRet;
}

int64 CIP::GetHash() const
{
    if (IsIPv4())
    {
        // reconstruct ip in reversed-byte order
        // (the original definition of the randomizer used network-order integers on little endian architecture)
        int64 ip = GetByte(0) << 24 + GetByte(1) << 16 + GetByte(2) << 8 + GetByte(3);
        return ip * 7789;
    }

    // for IPv6 addresses, use separate multipliers for each byte
    // these numbers are from the hexadecimal expansion of 3/Pi:
    static const int64 nByteMult[16] = 
        {0xF4764525, 0x75661FBE, 0xFA3B03BA, 0xEFCF4CA1, 0x4913E065, 0xDA655862, 0xFD7A1581, 0xCE19A812,
         0x92B6A557, 0x6374BC50, 0x096DC65F, 0x0EBA5B2B, 0x7D2CE0AB, 0x09BE7ADE, 0x5CC350EF, 0xC618E6C7};
    int64 nRet = 0;
    for (int n=0; n<16; n++)
        nRet += nByteMult[n]*GetByte(n);
    return nRet;
}

void CIP::print() const
{
    // printf("CIP(%s)\n", ToString().c_str());
}

void CIPPort::Init()
{
    port = 0;
}

CIPPort::CIPPort()
{
    Init();
}

CIPPort::CIPPort(const CIP& cip, unsigned short portIn) : CIP(cip), port(portIn)
{
}

CIPPort::CIPPort(const struct in_addr& ipv4Addr, unsigned short portIn) : CIP(ipv4Addr), port(portIn)
{
}

#ifdef USE_IPV6
CIPPort::CIPPort(const struct in6_addr& ipv6Addr, unsigned short portIn) : CIP(ipv6Addr), port(portIn)
{
}
#endif

CIPPort::CIPPort(const struct sockaddr_in& addr) : CIP(addr.sin_addr), port(ntohs(addr.sin_port))
{
    assert(addr.sin_family == AF_INET);
}

#ifdef USE_IPV6
CIPPort::CIPPort(const struct sockaddr_in6 &addr) : CIP(addr.sin6_addr), port(ntohs(addr.sin6_port))
{
   assert(addr.sin6_family == AF_INET6);
}
#endif

CIPPort::CIPPort(const char *pszIpPort, bool fAllowLookup)
{
    Init();
    CIPPort ip;
    if (Lookup(pszIpPort, ip, 0, fAllowLookup))
        *this = ip;
}

CIPPort::CIPPort(const char *pszIp, int portIn, bool fAllowLookup)
{
    std::vector<CIP> ip;
    if (LookupHost(pszIp, ip, 1, fAllowLookup))
        *this = CIPPort(ip[0], portIn);
}

CIPPort::CIPPort(const std::string &strIpPort, bool fAllowLookup)
{
    Init();
    CIPPort ip;
    if (Lookup(strIpPort.c_str(), ip, 0, fAllowLookup))
        *this = ip;
}

CIPPort::CIPPort(const std::string &strIp, int portIn, bool fAllowLookup)
{
    std::vector<CIP> ip;
    if (LookupHost(strIp.c_str(), ip, 1, fAllowLookup))
        *this = CIPPort(ip[0], portIn);
}

unsigned short CIPPort::GetPort() const
{
    return port;
}

bool operator==(const CIPPort& a, const CIPPort& b)
{
    return (operator==((CIP)a, (CIP)b) && a.port == b.port);
}

bool operator!=(const CIPPort& a, const CIPPort& b)
{
    return (operator!=((CIP)a, (CIP)b) || a.port != b.port);
}

bool operator<(const CIPPort& a, const CIPPort& b)
{
    return (operator<((CIP)a, (CIP)b) || (operator==((CIP)a, (CIP)b) && (a.port < b.port)));
}

bool CIPPort::GetSockAddr(struct sockaddr_in* paddr) const
{
    if (!IsIPv4())
        return false;
    memset(paddr, 0, sizeof(struct sockaddr_in));
    if (!GetInAddr(&paddr->sin_addr))
        return false;
    paddr->sin_family = AF_INET;
    paddr->sin_port = htons(port);
}

#ifdef USE_IPV6
bool CIPPort::GetSockAddr6(struct sockaddr_in6* paddr) const
{
    memset(paddr, 0, sizeof(struct sockaddr_in6));
    if (!GetIn6Addr(&paddr->sin6_addr))
        return false;
    paddr->sin6_family = AF_INET6;
    paddr->sin6_port = htons(port);
}
#endif

std::vector<unsigned char> CIPPort::GetKey() const
{
     std::vector<unsigned char> vKey;
     vKey.resize(18);
     memcpy(&vKey[0], ip, 16);
     vKey[16] = port / 0x100;
     vKey[17] = port & 0x0FF;
     return vKey;
}

std::string CIPPort::ToString() const
{
     return CIP::ToString() + strprintf(":%i", port);
}

void CIPPort::print() const
{
    // printf("CIPPort(%s)\n", ToString().c_str());
}

void CIPPort::SetPort(unsigned short portIn)
{
    port = portIn;
}
