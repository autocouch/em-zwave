#include "ruby.h"

#include <pthread.h>
#include <iostream>
#include <sstream>
#include <string.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

#include <limits.h>
#include <float.h>

#include <openzwave/Options.h>
#include <openzwave/Manager.h>
#include <openzwave/Driver.h>
#include <openzwave/Node.h>
#include <openzwave/Group.h>
#include <openzwave/Notification.h>
#include <openzwave/platform/Log.h>
#include <openzwave/value_classes/ValueStore.h>
#include <openzwave/value_classes/Value.h>
#include <openzwave/value_classes/ValueBool.h>

#include "em_zwave.h"

using namespace std;
using namespace OpenZWave;

extern "C"
void g_notification_queue_push(notification_t* notification) {
    notification->next = g_notification_queue;
    g_notification_queue = notification;
}

extern "C"
notification_t* g_notification_queue_pop() {
    notification_t* notification = g_notification_queue;
    if (notification) {
        g_notification_queue = notification->next;
    }
    return notification;
}

extern "C"
int g_notification_count()
{
    int count = 0;

    pthread_mutex_lock(&g_notification_mutex);
    notification_t* current = g_notification_queue;
    while(current != NULL)
    {
        count++;
        current = current->next;
    }
    pthread_mutex_unlock(&g_notification_mutex);

    return count;
}

extern "C"
void zwave_on_notification(Notification const* _notification, void* _context) {
    pthread_mutex_lock(&g_zwave_notification_mutex);

    notification_t* notification = (notification_t*)malloc(sizeof(notification_t));

    notification->type    = _notification->GetType();
    notification->home_id = _notification->GetHomeId();
    notification->node_id = _notification->GetNodeId();

    pthread_mutex_lock(&g_notification_mutex);
    g_notification_queue_push(notification);
    pthread_cond_broadcast(&g_notification_cond);
    pthread_mutex_unlock(&g_notification_mutex);

    switch(_notification->GetType()) {
    case Notification::Type_DriverReady:
        g_zwave_home_id = _notification->GetHomeId();
        break;

    case Notification::Type_DriverFailed:
        g_zwave_init_failed = true;
        pthread_cond_broadcast(&g_zwave_init_cond);
        break;

    case Notification::Type_AwakeNodesQueried:
    case Notification::Type_AllNodesQueried:
    case Notification::Type_AllNodesQueriedSomeDead:
        pthread_cond_broadcast(&g_zwave_init_cond);
        break;
    }

    pthread_mutex_unlock(&g_zwave_notification_mutex);
}

extern "C"
VALUE start_openzwave(void* data) {
    zwave_init_data_t* zwave_data = (zwave_init_data_t*)data;

    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_zwave_notification_mutex, &mutexattr);
    pthread_mutexattr_destroy(&mutexattr);

    pthread_mutex_lock(&g_zwave_init_mutex);

    // VALUE value = rb_ivar_get(self, rb_intern("@device"));
    // char * c_str = StringValuePtr(value);
    std::string config_path = zwave_data->config_path;
    Options::Create(config_path, "", "");
    Options::Get()->AddOptionBool("PerformReturnRoutes", false);
    Options::Get()->AddOptionBool("ConsoleOutput", false);
    Options::Get()->Lock();
    Manager::Create();

    Manager::Get()->AddWatcher(zwave_on_notification, NULL);

    for(int i = 0; i < zwave_data->devices_length; i++)
    {
        Manager::Get()->AddDriver(zwave_data->devices[i]);
    }

    pthread_cond_wait(&g_zwave_init_cond, &g_zwave_init_mutex);

    if(!g_zwave_init_failed) {
        Manager::Get()->WriteConfig(g_zwave_home_id);

        Driver::DriverData data;
        Manager::Get()->GetDriverStatistics(g_zwave_home_id, &data);
        printf("SOF: %d ACK Waiting: %d Read Aborts: %d Bad Checksums: %d\n", data.m_SOFCnt, data.m_ACKWaiting, data.m_readAborts, data.m_badChecksum);
        printf("Reads: %d Writes: %d CAN: %d NAK: %d ACK: %d Out of Frame: %d\n", data.m_readCnt, data.m_writeCnt, data.m_CANCnt, data.m_NAKCnt, data.m_ACKCnt, data.m_OOFCnt);
        printf("Dropped: %d Retries: %d\n", data.m_dropped, data.m_retries);

        g_zwave_init_done = true;
        pthread_mutex_unlock(&g_zwave_init_mutex);

        pthread_mutex_lock(&g_zwave_running_mutex);
        while (g_zwave_keep_running) {
            pthread_cond_wait(&g_zwave_running_cond, &g_zwave_running_mutex);
        }
        pthread_mutex_unlock(&g_zwave_running_mutex);
    } else {
        printf("Unable to initialize OpenZwave driver!\n");
        pthread_mutex_unlock(&g_zwave_init_mutex);
    }
    Manager::Destroy();

    // TODO: clean up

    /* Tell the event thread, that the manager was stopped */
    pthread_mutex_lock(&g_zwave_shutdown_mutex);
    g_zwave_manager_stopped = true;
    pthread_cond_signal(&g_zwave_shutdown_cond);
    pthread_mutex_unlock(&g_zwave_shutdown_mutex);

    return Qnil;
}

extern "C" void stop_openzwave(void*) {}

extern "C"
VALUE em_zwave_openzwave_thread(void* zwave_data)
{
    rb_thread_blocking_region(start_openzwave, zwave_data, stop_openzwave, NULL);
    return Qnil;
}

extern "C"
VALUE em_zwave_shutdown_no_gvl(void*)
{
    /* If the openzwave initialization is stopped prematurely, */
    /* then this initialization should be seen as failed. */
    pthread_mutex_lock(&g_zwave_init_mutex);
    if(!g_zwave_init_done)
    {
        g_zwave_init_failed = true;
        pthread_cond_broadcast(&g_zwave_init_cond);
    }
    pthread_mutex_unlock(&g_zwave_init_mutex);

    /* Finally we can stop the manager.  */
    pthread_mutex_lock(&g_zwave_running_mutex);
    g_zwave_keep_running = false;
    pthread_cond_broadcast(&g_zwave_running_cond);
    pthread_mutex_unlock(&g_zwave_running_mutex);

    /* Wait for the OpenZWave manager to shutdown. */
    pthread_mutex_lock(&g_zwave_shutdown_mutex);
    while(!g_zwave_manager_stopped)
    {
        pthread_cond_wait(&g_zwave_shutdown_cond, &g_zwave_shutdown_mutex);
    }
    pthread_mutex_unlock(&g_zwave_shutdown_mutex);

    /* Tell the event thread, that we want to stop as soon as there */
    /* are no more notifications in the queue! */
    pthread_mutex_lock(&g_notification_mutex);
    g_zwave_event_thread_keep_running = false;
    pthread_mutex_unlock(&g_notification_mutex);
    pthread_cond_broadcast(&g_notification_cond);
}

extern "C" void em_zwave_stop_shutdown(void*) {}

extern "C"
VALUE rb_zwave_shutdown_thread(void*)
{
    rb_thread_blocking_region(em_zwave_shutdown_no_gvl, NULL,
                              em_zwave_stop_shutdown, NULL);
    return Qnil;
}

extern "C"
VALUE rb_zwave_shutdown(VALUE self)
{
    rb_thread_create((VALUE (*)(...))rb_zwave_shutdown_thread, (void*)self);
    return Qnil;
}

extern "C"
VALUE wait_for_notification(void* n) {
    notification_t** notification = (notification_t**)n;

    pthread_mutex_lock(&g_notification_mutex);
    while (g_zwave_event_thread_keep_running & (*notification = g_notification_queue_pop()) == NULL)
    {
        pthread_cond_wait(&g_notification_cond, &g_notification_mutex);
    }

    pthread_mutex_unlock(&g_notification_mutex);
    return Qnil;
}

extern "C" void stop_waiting_for_notification(void* w) {}

extern "C"
VALUE em_zwave_get_notification_type_symbol(int type) {
    switch(type)
    {
    case Notification::Type_ValueAdded: return ID2SYM(rb_intern("value_added:"));
    case Notification::Type_ValueRemoved: return ID2SYM(rb_intern("value_removed"));
    case Notification::Type_ValueChanged: return ID2SYM(rb_intern("value_changed"));
    case Notification::Type_Group: return ID2SYM(rb_intern("group"));
    case Notification::Type_NodeAdded: return ID2SYM(rb_intern("node_added"));
    case Notification::Type_NodeRemoved: return ID2SYM(rb_intern("node_removed"));
    case Notification::Type_NodeEvent: return ID2SYM(rb_intern("node_event"));
    case Notification::Type_PollingDisabled: return ID2SYM(rb_intern("polling_disabled"));
    case Notification::Type_PollingEnabled: return ID2SYM(rb_intern("polling_enabled"));
    case Notification::Type_DriverReady: return ID2SYM(rb_intern("driver_ready"));
    case Notification::Type_DriverFailed: return ID2SYM(rb_intern("driver_failed"));
    case Notification::Type_AwakeNodesQueried: return ID2SYM(rb_intern("awake_nodes_queried"));
    case Notification::Type_AllNodesQueried: return ID2SYM(rb_intern("all_nodes_queried"));
    case Notification::Type_AllNodesQueriedSomeDead: return ID2SYM(rb_intern("all_nodes_queried_some_dead"));
    case Notification::Type_DriverReset: return ID2SYM(rb_intern("driver_reset"));
    case Notification::Type_Notification: return ID2SYM(rb_intern("notification"));
    case Notification::Type_NodeNaming: return ID2SYM(rb_intern("node_naming"));
    case Notification::Type_NodeProtocolInfo: return ID2SYM(rb_intern("node_protocol_info"));
    case Notification::Type_NodeQueriesComplete: return ID2SYM(rb_intern("node_queries_complete"));
    default: return INT2FIX(type);
    }
}

/* This thread loops continuously, waiting for callbacks happening in the C thread. */
extern "C"
VALUE em_zwave_event_thread(void* args)
{
    VALUE zwave = (VALUE)args;

    notification_t* waiting_notification = NULL;

    while(g_zwave_event_thread_keep_running || g_notification_count() > 0)
    {
        rb_thread_blocking_region(wait_for_notification, &waiting_notification,
                                  stop_waiting_for_notification, NULL);

        if(waiting_notification != NULL)
        {
            g_notification_overall++;

            VALUE notification = rb_obj_alloc(rb_cNotification);
            VALUE notification_options = rb_hash_new();

            rb_hash_aset(notification_options, ID2SYM(rb_intern("type")), em_zwave_get_notification_type_symbol(waiting_notification->type));
            rb_hash_aset(notification_options, ID2SYM(rb_intern("home_id")), INT2FIX(waiting_notification->home_id));
            rb_hash_aset(notification_options, ID2SYM(rb_intern("node_id")), INT2FIX(waiting_notification->node_id));

            VALUE init_argv[1];
            init_argv[0] = notification_options;
            rb_obj_call_init(notification, 1, init_argv);

            rb_funcall(zwave, id_push_notification, 1, notification);
        }

        waiting_notification = NULL;
    }

    // TODO: clean up all allocated resources

    rb_funcall(zwave, id_schedule_shutdown, 0, NULL);

    return Qnil;
}

extern "C"
VALUE rb_zwave_initialize_zwave(VALUE self) {
    zwave_init_data_t* zwave_data = (zwave_init_data_t*)malloc(sizeof(zwave_init_data_t));

    VALUE rb_config_path = rb_iv_get(self, "@config_path");
    zwave_data->config_path = StringValuePtr(rb_config_path);

    VALUE rb_devices = rb_iv_get(self, "@devices");
    zwave_data->devices_length = RARRAY_LEN(rb_devices);
    zwave_data->devices = (char**)malloc(zwave_data->devices_length * sizeof(char*));
    for(int i = 0; i < zwave_data->devices_length; i++)
    {
        VALUE rb_device_str = rb_ary_entry(rb_devices, (long)i);
        zwave_data->devices[0] = StringValuePtr(rb_device_str);
    }

    rb_thread_create((VALUE (*)(...))em_zwave_openzwave_thread, (void*)zwave_data);
    rb_thread_create((VALUE (*)(...))em_zwave_event_thread, (void*)self);
    return Qnil;
}

extern "C"
void Init_emzwave() {
    id_push_notification = rb_intern("push_notification");
    id_schedule_shutdown = rb_intern("schedule_shutdown");

    rb_mEm = rb_define_module("EventMachine");

    rb_cZwave = rb_define_class_under(rb_mEm, "Zwave", rb_cObject);
    rb_define_method(rb_cZwave, "initialize_zwave", (VALUE (*)(...))rb_zwave_initialize_zwave, 0);
    rb_define_method(rb_cZwave, "shutdown", (VALUE (*)(...))rb_zwave_shutdown, 0);

    rb_cNotification = rb_define_class_under(rb_cZwave, "Notification", rb_cObject);
}
