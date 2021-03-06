#include "dragonet.h"

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "rpmsg_lite.h"
#include "rpmsg_queue.h"
#include "rpmsg_ns.h"

#include <string.h>
}

#include "dragonet_platform.h"
#include <string>
#include <map>
#include <functional>
#include <vector>
#include <unordered_set>

namespace dragonet {

extern "C"
{
static int publisher_ept_rx_cb(void *payload, int payload_len, unsigned long src, void *priv);

static int subscriber_ept_rx_cb(void *payload, int payload_len, unsigned long src, void *queues);

static void publisher_ns_new_ept_cb(unsigned int new_ept, const char *new_ept_name, unsigned long flags, void *data);
}

class DragonetImpl
{
public:
    DragonetImpl()
    {
        //subscribers_mutex_ = xSemaphoreCreateMutexStatic(&subscribers_mutex_buffer_);
        //queue_set_map_mutex_ = xSemaphoreCreateMutexStatic(&queue_set_map_mutex_buffer_);
        //subscriber_queues_mutex_ = xSemaphoreCreateMutexStatic(&subscriber_queues_mutex_buffer_);
        //publish_epts_mutex_ = xSemaphoreCreateMutexStatic(&publish_epts_mutex_buffer_);
        subscribers_mutex_ = xSemaphoreCreateMutex();
        queue_set_map_mutex_ = xSemaphoreCreateMutex();
        subscriber_queues_mutex_ = xSemaphoreCreateMutex();
        publish_epts_mutex_ = xSemaphoreCreateMutex();
        pending_subscriptions_ = xQueueCreate(16, sizeof(PendingSubscription_t));
        env_init();
        rpmsg_ = rpmsg_lite_remote_init(rpmsg_lite_base_, PLATFORM_LINK_ID, RL_NO_FLAGS);
        rpmsg_ns_bind(rpmsg_, publisher_ns_new_ept_cb, this);
        while (!rpmsg_lite_is_link_up(rpmsg_));
    }

    ~DragonetImpl()
    {
        // TODO: Destroy endpoints
        rpmsg_lite_deinit(rpmsg_);
    }

    void RegisterSubscription(const char *channel, std::function<void(char*)> callback, int msg_size, int queue_size)
    {
        TaskHandle_t t = xTaskGetCurrentTaskHandle();
        if (xSemaphoreTake(queue_set_map_mutex_, portMAX_DELAY) == pdTRUE)
        {
            if (queue_set_map_.find(t) == queue_set_map_.end())
            {
                QueueSetHandle_t qs = xQueueCreateSet(1024);
                queue_set_map_[t] = qs;
            }
        }
        else
        {
            return;
        }
        xSemaphoreGive(queue_set_map_mutex_);
        QueueHandle_t q = xQueueCreate(queue_size, msg_size);
        xQueueAddToSet(q, queue_set_map_[t]);
        if (xSemaphoreTake(subscriber_queues_mutex_, portMAX_DELAY) == pdTRUE)
        {
            if (subscriber_queues_.find(std::string(channel)) == subscriber_queues_.end())
            {
                subscriber_queues_[std::string(channel)] = std::vector<std::tuple<QueueHandle_t, TaskHandle_t>>();
                rpmsg_lite_endpoint *ept = rpmsg_lite_create_ept(rpmsg_, RL_ADDR_ANY, subscriber_ept_rx_cb, &subscriber_queues_[std::string(channel)]);
                char name[32];
                sprintf(name, "%s__p", channel);
                rpmsg_ns_announce(rpmsg_, ept, name, RL_NS_CREATE);
            }
            subscriber_queues_[std::string(channel)].push_back(std::make_tuple(q, t));
        }
        else
        {
            return;
        }
        xSemaphoreGive(subscriber_queues_mutex_);
        if (xSemaphoreTake(subscribers_mutex_, portMAX_DELAY) == pdTRUE)
        {
            subscribers_[q] = callback;
        }
        else
        {
            return;
        }
        xSemaphoreGive(subscribers_mutex_);
    }

    void PublishMessage(const char *channel, const char *msg, int size)
    {
        TaskHandle_t t = xTaskGetCurrentTaskHandle();
        // Intra-core subscribers
        for (std::tuple<QueueHandle_t, TaskHandle_t> &qt : subscriber_queues_[std::string(channel)])
        {
            // Intra-task subscribers
            if (std::get<1>(qt) == t)
            {
                subscribers_[std::get<0>(qt)](const_cast<char*>(msg));
            }
            // Inter-task subscribers
            else
            {
                xQueueSend(std::get<0>(qt), msg, 0);
            }
        }
        // Inter-core subscribers
        PendingSubscription_t pending;
        while(xQueueReceive(pending_subscriptions_, &pending, 0) == pdTRUE)
        {
            std::string name(pending.channel);
            name = name.substr(0, name.length() - 3);
            if (pending.create)
            {
                RegisterPublishEndpoint(name, pending.dst);
            }
            else
            {
                UnregisterPublishEndpoint(name, pending.dst);
            }
        }
        if (publish_epts_.find(std::string(channel)) != publish_epts_.end())
        {
            rpmsg_lite_endpoint *ept = std::get<0>(publish_epts_[std::string(channel)]);
            for (unsigned long dst : std::get<1>(publish_epts_[std::string(channel)]))
            {
                rpmsg_lite_send(rpmsg_, ept, dst, const_cast<char*>(msg), size, 0);
            }
        }
    }

    void DispatchCallbacks()
    {
        TaskHandle_t t = xTaskGetCurrentTaskHandle();
        if (queue_set_map_.find(t) != queue_set_map_.end())
        {
            QueueHandle_t q = xQueueSelectFromSet(queue_set_map_[t], 0xFFFF);
            if (q != NULL)
            {
                char buf[MAX_MESSAGE_SIZE];
                xQueueReceive(q, (char*) buf, 0);
                subscribers_[q]((char*) buf);
            }
        }
    }

    void RegisterPublishEndpoint(std::string name, unsigned long dst)
    {
        rpmsg_lite_endpoint *ept = rpmsg_lite_create_ept(rpmsg_, RL_ADDR_ANY, publisher_ept_rx_cb, NULL);
        // TODO: move this out of ISR so simultaneous subscribes don't fail
        if (xSemaphoreTake(publish_epts_mutex_, portMAX_DELAY) == pdTRUE)
        {
            if (publish_epts_.find(name) == publish_epts_.end())
            {
                publish_epts_[name] = std::make_tuple(ept, std::unordered_set<unsigned long>());
            }
            std::get<1>(publish_epts_[name]).insert(dst);
        }
        else
        {
            return;
        }
        xSemaphoreGive(publish_epts_mutex_);
    }

    void UnregisterPublishEndpoint(std::string name, unsigned long dst)
    {
        if (xSemaphoreTake(publish_epts_mutex_, portMAX_DELAY) == pdTRUE)
        {
            if (publish_epts_.find(name) != publish_epts_.end())
            {
                std::get<1>(publish_epts_[name]).erase(dst);
            }
        }
        else
        {
            return;
        }
        xSemaphoreGive(publish_epts_mutex_);
    }

    void PendSubscription(const char *channel, unsigned long dst, bool create)
    {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        PendingSubscription_t pend{channel, dst, create};
        xQueueSendFromISR(pending_subscriptions_, &pend, &higherPriorityTaskWoken);
        if (higherPriorityTaskWoken == pdTRUE)
        {
            taskYIELD();
        }
    }

private:
    void *rpmsg_lite_base_{BOARD_SHARED_MEMORY_BASE};
    struct rpmsg_lite_instance *rpmsg_;
    std::map<QueueHandle_t, std::function<void(char*)>> subscribers_;
    std::map<TaskHandle_t, QueueSetHandle_t> queue_set_map_;
    std::map<std::string, std::vector<std::tuple<QueueHandle_t, TaskHandle_t>>> subscriber_queues_;
    std::map<std::string, std::tuple<rpmsg_lite_endpoint*, std::unordered_set<unsigned long>>> publish_epts_;

    //StaticSemaphore_t subscribers_mutex_buffer_;
    SemaphoreHandle_t subscribers_mutex_;
    //StaticSemaphore_t queue_set_map_mutex_buffer_;
    SemaphoreHandle_t queue_set_map_mutex_;
    //StaticSemaphore_t subscriber_queues_mutex_buffer_;
    SemaphoreHandle_t subscriber_queues_mutex_;
    //static StaticSemaphore_t publish_epts_mutex_buffer_;
    SemaphoreHandle_t publish_epts_mutex_;

    QueueHandle_t pending_subscriptions_;

    typedef struct
    {
        const char *channel;
        unsigned long dst;
        bool create;
    } PendingSubscription_t;
};

extern "C"
{
static int publisher_ept_rx_cb(void *payload, int payload_len, unsigned long src, void *priv)
{
    return 0;
}

static int subscriber_ept_rx_cb(void *payload, int payload_len, unsigned long src, void *queues)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (queues)
    {
        for (std::tuple<QueueHandle_t, TaskHandle_t> &qt : *(std::vector<std::tuple<QueueHandle_t, TaskHandle_t>>*) queues)
        {
            BaseType_t higherPriorityTaskWokenCur = pdFALSE;
            xQueueSendFromISR(std::get<0>(qt), payload, &higherPriorityTaskWokenCur);
            if (higherPriorityTaskWokenCur == pdTRUE)
            {
                higherPriorityTaskWoken = pdTRUE;
            }
        }
    }
    if (higherPriorityTaskWoken == pdTRUE)
    {
        taskYIELD();
    }
    return 0;
}

static void publisher_ns_new_ept_cb(unsigned int new_ept, const char *new_ept_name, unsigned long flags, void *impl_instance)
{
    if (strcmp(&new_ept_name[strlen(new_ept_name) - 3], "__s") == 0)
    {
        ((DragonetImpl*)impl_instance)->PendSubscription(new_ept_name, new_ept, flags == RL_NS_CREATE);
    }
}
}

Dragonet::Dragonet()
{
}

Dragonet::~Dragonet() = default;

void Dragonet::Init()
{
    if (initialized_)
    {
        return;
    }
    vTaskSuspendAll();
    if (!initialized_)
    {
        impl_.reset(new DragonetImpl());
        initialized_ = true;
    }
    xTaskResumeAll();
}

void Dragonet::Spin()
{
    while (true)
    {
        impl_->DispatchCallbacks();
    }
}

int Dragonet::serializeAndPublish(const char *channel, const char *msg, int size)
{
    impl_->PublishMessage(channel, (char*) msg, size);
    return 0;
}

void Dragonet::subscribeSerialized(const char *channel, std::function<void(char*)> callback, int msg_size, int queue_size)
{
    impl_->RegisterSubscription(channel, callback, msg_size, queue_size);
}

}
