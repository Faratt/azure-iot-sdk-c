// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef GENERIC_MESSENGER
#define GENERIC_MESSENGER

#include "azure_c_shared_utility/macro_utils.h"
#include "azure_c_shared_utility/umock_c_prod.h"
#include "azure_c_shared_utility/optionhandler.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef MESSAGE_QUEUE* MESSAGE_QUEUE_HANDLE;
typedef void* MESSAGE_HANDLE;

#define MESSAGE_QUEUE_RESULT_STRINGS  \
    MESSAGE_QUEUE_SUCCESS,            \
    MESSAGE_QUEUE_ERROR,              \
    MESSAGE_QUEUE_RETRYABLE_ERROR,    \
    MESSAGE_QUEUE_TIMEOUT,            \
    MESSAGE_QUEUE_CANCELLED

DEFINE_ENUM(MESSAGE_QUEUE_RESULT, MESSAGE_QUEUE_RESULT_STRINGS);

typedef void(*MESSAGE_PROCESSING_COMPLETED_CALLBACK)(MESSAGE_HANDLE message, MESSAGE_QUEUE_RESULT result, void* reason, void* context);
typedef void(*PROCESS_MESSAGE_CALLBACK)(MESSAGE_HANDLE message, MESSAGE_PROCESSING_COMPLETED_CALLBACK on_processing_completed_callback, void* context);

typedef struct MESSAGE_QUEUE_CONFIG_TAG
{
	size_t message_processing_timeout_secs;
	PROCESS_MESSAGE_CALLBACK on_process_message_callback;
	void* on_process_message_context;
	MESSAGE_PROCESSING_COMPLETED_CALLBACK on_message_processing_completed_callback;
	void* on_message_processing_completed_context;
} MESSAGE_QUEUE_CONFIG;

extern MESSAGE_QUEUE_HANDLE message_queue_create(MESSAGE_QUEUE_CONFIG* config);
extern void message_queue_destroy(MESSAGE_QUEUE_HANDLE message_queue);
extern int message_queue_add(MESSAGE_QUEUE_HANDLE message_queue, MESSAGE_HANDLE message);
extern void message_queue_remove_all(MESSAGE_QUEUE_HANDLE message_queue);
extern int message_queue_is_empty(MESSAGE_QUEUE_HANDLE message_queue, bool* is_empty);
extern void message_queue_do_work(MESSAGE_QUEUE_HANDLE message_queue);

extern OPTIONHANDLER_HANDLE retrieve_options(MESSAGE_QUEUE_HANDLE handle);

#ifdef __cplusplus
}
#endif

#endif /*GENERIC_MESSENGER*/
