#include "SNTPClient.h"
#include <app/server/Server.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include <openthread/dns_client.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#include <openthread/udp.h>
#include <system/TimeSource.h>

#include <lib/core/CHIPEncoding.h>
#include <lib/core/CHIPError.h>
#include <inet/IPAddress.h>
#include <inet/InetInterface.h>
#include <inet/UDPEndPoint.h>

const char *TAG = "matter_protocol";

static constexpr int      SNTP_PORT  = 123;
static constexpr uint32_t TimeAt1970 = 2208988800UL;

namespace {
struct PacketHeader
{
    uint8_t  Flags;
    uint8_t  Stratum;
    uint8_t  Poll;
    uint8_t  Precision;
    uint32_t RootDelay;
    uint32_t RootDispersion;
    uint32_t ReferenceIdentifier;
    uint64_t ReferenceTimestamp;
    uint64_t OriginateTimestamp;
    uint64_t ReceiveTimestamp;
    uint64_t TransmitTimestamp;
} __attribute__((packed));
}  // namespace

static constexpr const int mHeaderSize = sizeof(struct PacketHeader);
static_assert(mHeaderSize == 48, "PacketHeader should be 48 Bytes");

static struct otInstance *gInstance     = nullptr;
SNTPCallback              sntp_callbcak = nullptr;
static otUdpSocket        gUDPSocket;

static void DnsAddressCallback(otError aError, const otDnsAddressResponse *aResponse, void *aContext);
static void SNTPReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo);

void SNTPClientInit(struct otInstance *aInstance)
{
    gInstance = aInstance;

    if (gInstance != nullptr)
        otUdpClose(gInstance, &gUDPSocket);
}

void SNTPClientQuery(const char *hostname, SNTPCallback callback, void *context)
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
    {
        ESP_LOGE(TAG, "Didn't connect to any fabric");
        return;
    }

    if (gInstance == nullptr)
    {
        ESP_LOGE(TAG, "gInstance is null");
        return;
    }

    if (callback)
        sntp_callbcak = callback;

    const otDnsQueryConfig *config = otDnsClientGetDefaultConfig(gInstance);

    otError error = otDnsClientResolveIp4Address(gInstance, hostname, DnsAddressCallback, context, config);
    if (error != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "Failed to start DNS resolution: %d", error);
    }
};

static void DnsAddressCallback(otError aError, const otDnsAddressResponse *aResponse, void *aContext)
{
    if (aError != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "DNS query failed %d", aError);
        return;
    }

    char         hostname[OT_DNS_MAX_NAME_SIZE];
    otIp6Address address;
    uint32_t     ttl;

    otDnsAddressResponseGetHostName(aResponse, hostname, sizeof(hostname));

    if (otDnsAddressResponseGetAddress(aResponse, 0, &address, &ttl) != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "No address found for %s", hostname);
        return;
    }

    struct PacketHeader header;
    memset(&header, 0, mHeaderSize);
    header = {
        .Flags             = 0x23,
        .TransmitTimestamp = 114514,
    };

    otError    error;
    otSockAddr sockaddr;

    memset(&sockaddr, 0, sizeof(sockaddr));

    if (!otUdpIsOpen(gInstance, &gUDPSocket))
    {
        otUdpOpen(gInstance, &gUDPSocket, SNTPReceive, aContext);
    }

    otMessage *message = otUdpNewMessage(gInstance, NULL);
    if (message == nullptr)
    {
        ESP_LOGE(TAG, "Failed to allocate message");
        return;
    }

    otMessageInfo messageInfo;
    error = otMessageAppend(message, &header, sizeof(header));
    if (error != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "Failed to append data: %d", error);
        otMessageFree(message);
        return;
    }

    memset(&messageInfo, 0, sizeof(messageInfo));
    messageInfo.mPeerAddr = address;
    messageInfo.mPeerPort = SNTP_PORT;

    error = otUdpSend(gInstance, &gUDPSocket, message, &messageInfo);
    if (error != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "Failed to send UDP message: %d", error);
        otMessageFree(message);
    }
}

static void SNTPReceive(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    PacketHeader response;
    uint16_t     length = otMessageGetLength(aMessage) - otMessageGetOffset(aMessage);

    ESP_LOGD(TAG, "Received UDP packet, length: %d", length);

    if (length != sizeof(PacketHeader))
    {
        ESP_LOGE(TAG, "Invalid SNTP packet length");
        return;
    }

    if (otMessageRead(aMessage, otMessageGetOffset(aMessage), &response, sizeof(response)) != sizeof(response))
    {
        ESP_LOGE(TAG, "Failed to read message");
        return;
    }

    uint64_t current_time =
        (chip::Encoding::BigEndian::Get64((uint8_t *) &response.TransmitTimestamp) >> 32) - TimeAt1970;

    ESP_LOGI(TAG, "TransmitTimestamp is %llu %llu", response.TransmitTimestamp, current_time);

    if (sntp_callbcak)
        sntp_callbcak(current_time, 0, aContext);
}